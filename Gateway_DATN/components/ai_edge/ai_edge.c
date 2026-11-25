#include "ai_edge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "driver/gpio.h"
#include "audio_hal.h"
#include "ai_cloud.h"
#include "sys_core.h"
#include "esp_heap_caps.h"
#include "ei_wrapper.h"
#include "mesh_controller.h"
#include "gateway_state_manager.h"
#include "ui_display.h"
#include <string.h>
#include <math.h>

static const char *TAG = "ai_edge";

#define BOOT_BUTTON_GPIO         0
#define STREAM_CHUNK_SAMPLES     960    // 60ms @ 16kHz
#define STREAM_SILENCE_TARGET    12     // 12 * 60ms = 720ms im lặng để chốt lượt với Gemini Live (phản xạ nhanh)

// =============================================================================
// UNIFIED VOICE ENDPOINTER (Single Truth VAD State Machine)
// =============================================================================
typedef struct {
    float noise_floor_ema;   // Khởi tạo 25.0f, thích ứng khi không có giọng nói
    float speech_rms_ema;    // Khởi tạo 250.0f, thích ứng theo âm lượng người nói
    int   speech_streak;     // Số chunks liên tiếp có tiếng nói
    int   silence_counter;   // Bộ đếm im lặng (chỉ reset khi speech_streak >= 3)
} voice_endpointer_t;

static inline void voice_endpointer_init(voice_endpointer_t *ep) {
    if (!ep) return;
    float amb = (float)audio_dsp_get_ambient_noise();
    if (amb < 15.0f) amb = 15.0f;
    if (amb > 180.0f) amb = 180.0f;
    ep->noise_floor_ema = amb;
    ep->speech_rms_ema = 250.0f;
    ep->speech_streak = 0;
    ep->silence_counter = 0;
}

static inline bool voice_endpointer_feed(voice_endpointer_t *ep, const int16_t *chunk, size_t samples, int *out_rms, float *out_thresh) {
    int rms = audio_vad_get_rms(chunk, samples);
    if (out_rms) *out_rms = rms;

    bool raw_webrtc = audio_vad_raw_webrtc_chunk(chunk, samples);

    // Ngưỡng tiếng nói động thống nhất (Unified Dynamic Threshold)
    float voice_thresh = fmaxf(ep->noise_floor_ema * 1.75f + 35.0f, ep->speech_rms_ema * 0.32f);
    if (out_thresh) *out_thresh = voice_thresh;

    bool is_speech = (raw_webrtc && ((float)rms >= voice_thresh));

    if (is_speech) {
        ep->speech_streak++;
        ep->speech_rms_ema = ep->speech_rms_ema * 0.85f + (float)rms * 0.15f;
        // Soft Silence Counter: Chỉ reset về 0 khi có tối thiểu 3 chunks liên tiếp có tiếng (chống ho, gõ bàn)
        if (ep->speech_streak >= 3) {
            ep->silence_counter = 0;
        }
    } else {
        ep->speech_streak = 0;
        ep->silence_counter++;
        if (!raw_webrtc) {
            if ((float)rms > ep->noise_floor_ema) {
                ep->noise_floor_ema = ep->noise_floor_ema * 0.90f + (float)rms * 0.10f;
            } else {
                ep->noise_floor_ema = ep->noise_floor_ema * 0.80f + (float)rms * 0.20f;
            }
            if (ep->noise_floor_ema < 15.0f) ep->noise_floor_ema = 15.0f;
            if (ep->noise_floor_ema > 500.0f) ep->noise_floor_ema = 500.0f;
        }
    }
    return is_speech;
}

static voice_endpointer_t s_endpointer;

// =============================================================================
// [CHẾ ĐỘ KIỂM THỬ MODEL 2 CÔ LẬP]
// 1: Khi Model 2 khớp/không khớp, KHÔNG gọi Cloud, phát lại âm thanh vừa thu ra loa để test, rồi tự về IDLE (0% xung đột mạng)
// 0: Chế độ hoạt động chuẩn (Pre-warm WebSocket, Hybrid Cloud Fallback)
// =============================================================================
#define DEBUG_LOCAL_TEST_MODE    0

#define CMD_INFER_EVT_START      (1 << 0)
static TaskHandle_t audio_task_handle = NULL;
static TaskHandle_t button_task_handle = NULL;
static TaskHandle_t s_cmd_infer_task_handle = NULL;
static int16_t *s_cmd_infer_snapshot = NULL;
static volatile bool s_cmd_infer_busy = false;
static volatile size_t s_cmd_infer_sample_count = 0;
static volatile uint32_t s_edge_turn_id = 0;
static volatile uint32_t s_cmd_infer_turn_id = 0;
static volatile int s_cmd_infer_noise_floor = 25;
static portMUX_TYPE s_infer_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile ai_edge_state_t s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
static volatile int s_responding_silent_cycles = 0;
static volatile bool s_manual_wake_requested = false;
static volatile bool s_manual_finish_requested = false;
static bool s_cloud_speaking_notified = false;

void ai_edge_finish_speaking(void) {
    s_manual_finish_requested = true;
}

static bool is_command_device_enabled(const char *cmd_label) {
    if (!cmd_label) return false;
    gateway_device_state_t st;
    gateway_state_get(&st);
    if (strcmp(cmd_label, "bat_den") == 0 || strcmp(cmd_label, "tat_den") == 0) {
        return st.dimmer_registered;
    }
    if (strcmp(cmd_label, "bat_cong_tac") == 0 || strcmp(cmd_label, "tat_cong_tac") == 0) {
        return st.relay_registered;
    }
    return false;
}

static void dispatch_local_mesh_command(const char *cmd_label) {
    if (!cmd_label) return;
    gateway_device_state_t st;
    gateway_state_get(&st);

    if (strcmp(cmd_label, "bat_den") == 0) {
        if (!st.dimmer_registered) {
            ESP_LOGW(TAG, "Local Edge AI: Đèn chưa được đăng ký vào mạng Thread");
            return;
        }
        if (!st.dimmer_node_online) {
            ESP_LOGW(TAG, "Local Edge AI: Đèn hiện đang Offline, không gửi lệnh");
            return;
        }
        ESP_LOGI(TAG, "Local Edge AI: BẬT ĐÈN (khôi phục mức sáng gần nhất)");
        mesh_controller_set_dimmer(true, 0);
    } else if (strcmp(cmd_label, "tat_den") == 0) {
        if (!st.dimmer_registered) {
            ESP_LOGW(TAG, "Local Edge AI: Đèn chưa được đăng ký vào mạng Thread");
            return;
        }
        if (!st.dimmer_node_online) {
            ESP_LOGW(TAG, "Local Edge AI: Đèn hiện đang Offline, không gửi lệnh");
            return;
        }
        ESP_LOGI(TAG, "Local Edge AI: TẮT ĐÈN");
        mesh_controller_set_dimmer(false, 0);
    } else if (strcmp(cmd_label, "bat_cong_tac") == 0) {
        if (!st.relay_registered) {
            ESP_LOGW(TAG, "Local Edge AI: Relay chưa được đăng ký vào mạng Thread");
            return;
        }
        if (!st.relay_node_online) {
            ESP_LOGW(TAG, "Local Edge AI: Relay hiện đang Offline, không gửi lệnh");
            return;
        }
        ESP_LOGI(TAG, "Local Edge AI: BẬT CÔNG TẮC RELAY");
        mesh_controller_set_relay(true);
    } else if (strcmp(cmd_label, "tat_cong_tac") == 0) {
        if (!st.relay_registered) {
            ESP_LOGW(TAG, "Local Edge AI: Relay chưa được đăng ký vào mạng Thread");
            return;
        }
        if (!st.relay_node_online) {
            ESP_LOGW(TAG, "Local Edge AI: Relay hiện đang Offline, không gửi lệnh");
            return;
        }
        ESP_LOGI(TAG, "Local Edge AI: TẮT CÔNG TẮC RELAY");
        mesh_controller_set_relay(false);
    } else {
        ESP_LOGW(TAG, "Bỏ qua nhãn không thuộc hệ thống (như quạt hoặc unknown): %s", cmd_label);
    }
}

// Luồng suy luận Model 2 bất đồng bộ (Core 1, Priority 4, Stack 24KB trong PSRAM)
// Giúp audio_recording_task (Priority 6) không bao giờ bị nghẽn DMA I2S mic trong 200-400ms!
static void cmd_infer_task(void *arg) {
    ESP_LOGI(TAG, "Decoupled Command Inference Task Started on Core %d (Priority 4, 24KB PSRAM Stack).", xPortGetCoreID());

    while (1) {
        uint32_t notify_bits = 0;
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &notify_bits, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!(notify_bits & CMD_INFER_EVT_START) || !s_cmd_infer_snapshot || s_cmd_infer_sample_count == 0) {
            taskENTER_CRITICAL(&s_infer_mux);
            s_cmd_infer_busy = false;
            taskEXIT_CRITICAL(&s_infer_mux);
            continue;
        }

        taskENTER_CRITICAL(&s_infer_mux);
        s_cmd_infer_busy = true;
        uint32_t local_turn_id = s_cmd_infer_turn_id;
        size_t raw_samples = s_cmd_infer_sample_count;
        int ambient_noise = s_cmd_infer_noise_floor;
        taskEXIT_CRITICAL(&s_infer_mux);

        int num_chunks = raw_samples / STREAM_CHUNK_SAMPLES;
        if (num_chunks > 25) num_chunks = 25;

        // Bước 1: Tính mảng chunk_rms cho từng đoạn 60ms trong snapshot thực tế
        int chunk_rms[25] = {0};
        int max_speech_rms = 0;
        for (int k = 0; k < num_chunks; k++) {
            chunk_rms[k] = audio_vad_get_rms(&s_cmd_infer_snapshot[k * STREAM_CHUNK_SAMPLES], STREAM_CHUNK_SAMPLES);
            if (chunk_rms[k] > max_speech_rms) {
                max_speech_rms = chunk_rms[k];
            }
        }

        int min_ambient_gate = ambient_noise * 2 + 20;
        int rel_peak_gate = (int)(max_speech_rms * 0.32f);
        int user_voice_thresh = (min_ambient_gate > rel_peak_gate) ? min_ambient_gate : rel_peak_gate;

        // Near-field Voice Validation
        if (max_speech_rms < 150) {
            ESP_LOGW(TAG, ">>> [NOISE REJECTION (ASYNC)] Tạp âm môi trường xa (MaxRMS: %d < 150) -> Hủy bỏ lượt thu âm <<<", max_speech_rms);
            taskENTER_CRITICAL(&s_infer_mux);
            bool is_valid_turn = (local_turn_id == s_edge_turn_id && s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
            if (is_valid_turn) {
                s_edge_turn_id++;
                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
            }
            s_cmd_infer_busy = false;
            taskEXIT_CRITICAL(&s_infer_mux);

            if (is_valid_turn) {
                ui_display_hide_voice_overlay();
                s_cloud_speaking_notified = false;
                ai_cloud_cancel_turn();
                ei_wakeword_reset();
            }
            continue;
        }

        // Bước 3: Đếm tổng chunk giọng nói, xác định biên độ câu lệnh chính
        int first_speech_chunk = -1;
        int last_speech_chunk = -1;
        int total_user_chunks = 0;
        int tail_active_chunks = 0;
        int silence_gap = 0;
        bool main_utterance_closed = false;

        for (int k = 0; k < num_chunks; k++) {
            if (chunk_rms[k] >= user_voice_thresh) {
                if (first_speech_chunk == -1) first_speech_chunk = k;
                if (!main_utterance_closed) {
                    last_speech_chunk = k;
                }
                silence_gap = 0;
                total_user_chunks++;
                if (k >= 20) {
                    tail_active_chunks++;
                }
            } else {
                if (first_speech_chunk != -1) {
                    silence_gap++;
                    if (silence_gap >= 3) {
                        main_utterance_closed = true;
                    }
                }
            }
        }

        int speech_span_chunks = (first_speech_chunk != -1 && last_speech_chunk >= first_speech_chunk) ?
                                 (last_speech_chunk - first_speech_chunk + 1) : 0;

        // Continuous Long Speech check (tính đến 5 chunk đầu là Pre-roll im lặng)
        bool is_continuous_long_speech = (num_chunks >= 25) &&
                                         ((tail_active_chunks >= 2 && total_user_chunks >= 8) ||
                                          (chunk_rms[23] >= user_voice_thresh || chunk_rms[24] >= user_voice_thresh) ||
                                          (speech_span_chunks >= 17));

        if (is_continuous_long_speech) {
            ESP_LOGI(TAG, ">>> [CONTINUOUS SPEECH (ASYNC)] Câu nói dài/liên tục (Span:%d, Tail:%d, Active:%d) -> Chuyển thẳng sang Gemini Live! <<<",
                     speech_span_chunks, tail_active_chunks, total_user_chunks);
            taskENTER_CRITICAL(&s_infer_mux);
            bool is_valid_turn = (local_turn_id == s_edge_turn_id && s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
            bool need_end_turn = (is_valid_turn && s_edge_state == AI_EDGE_STATE_AI_RESPONDING);
            s_cmd_infer_busy = false;
            taskEXIT_CRITICAL(&s_infer_mux);

            if (is_valid_turn) {
                if (sys_core_is_wifi_connected()) {
                    ai_cloud_commit_turn();
                    if (need_end_turn) {
                        ai_cloud_end_turn();
                    }
                } else {
                    ESP_LOGE(TAG, ">>> [OFFLINE FALLBACK] Ngoại tuyến hoặc lỗi kết nối Live API! <<<");
                    audio_hal_play_error_sound();
                    ui_display_hide_voice_overlay();
                    s_cloud_speaking_notified = false;
                    ai_cloud_stop_session();
                    ei_wakeword_reset();
                    taskENTER_CRITICAL(&s_infer_mux);
                    if (local_turn_id == s_edge_turn_id) {
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                    }
                    taskEXIT_CRITICAL(&s_infer_mux);
                }
            }
            continue;
        }

        // Nếu câu nói ngắn hơn khung 1.2s (19200 mẫu), dùng phần đã zero-pad sẵn trong s_cmd_infer_snapshot để Model 2 luôn chạy được
        size_t samples_available = (raw_samples < EI_COMMAND_WINDOW_SAMPLES) ? EI_COMMAND_WINDOW_SAMPLES : raw_samples;

        // Căn tâm câu nói vào giữa khung 19.200 mẫu (1.2s)
        int speech_center_sample = 12000;
        if (first_speech_chunk != -1 && last_speech_chunk >= first_speech_chunk) {
            int speech_start_sample = first_speech_chunk * STREAM_CHUNK_SAMPLES;
            int speech_end_sample = (last_speech_chunk + 1) * STREAM_CHUNK_SAMPLES;
            speech_center_sample = (speech_start_sample + speech_end_sample) / 2;
        } else {
            speech_center_sample = 4800 + (EI_COMMAND_WINDOW_SAMPLES / 2);
        }

        int best_offset = speech_center_sample - (EI_COMMAND_WINDOW_SAMPLES / 2);
        const int MAX_OFFSET = (samples_available >= EI_COMMAND_WINDOW_SAMPLES) ? (samples_available - EI_COMMAND_WINDOW_SAMPLES) : 0;
        if (best_offset < 0) best_offset = 0;
        if (best_offset > MAX_OFFSET) best_offset = MAX_OFFSET;

        int slice1_offset = best_offset;
        int slice2_offset = best_offset - 1600;
        if (slice2_offset < 0) {
            slice2_offset = (best_offset + 1600 <= MAX_OFFSET) ? (best_offset + 1600) : 0;
        }
        int slice3_offset = best_offset + 1600;
        if (slice3_offset > MAX_OFFSET) {
            slice3_offset = (slice2_offset != 0) ? 0 : MAX_OFFSET;
        }
        if (slice3_offset == slice1_offset || slice3_offset == slice2_offset) {
            if (slice1_offset != MAX_OFFSET && slice2_offset != MAX_OFFSET) slice3_offset = MAX_OFFSET;
            else if (slice1_offset != 0 && slice2_offset != 0) slice3_offset = 0;
            else slice3_offset = 2400;
        }
        if (slice3_offset > MAX_OFFSET) slice3_offset = MAX_OFFSET;

        int slice_offsets[3] = { slice1_offset, slice2_offset, slice3_offset };
        const char *slice_names[3] = { "1/3 - Centered", "2/3 - Left Shift", "3/3 - Right Shift" };

        bool command_matched = false;
        float last_conf = 0.0f;
        const char *last_lbl = "unknown";

        for (int s = 0; s < 3; s++) {
            taskENTER_CRITICAL(&s_infer_mux);
            bool aborted_mid_loop = (local_turn_id != s_edge_turn_id || s_edge_state == AI_EDGE_STATE_IDLE_WAKEWORD);
            taskEXIT_CRITICAL(&s_infer_mux);
            if (aborted_mid_loop) {
                ESP_LOGW(TAG, ">>> [MODEL 2 ABORTED] Turn ID changed (%lu != %lu) or IDLE -> Bỏ qua kết quả! <<<",
                         (unsigned long)local_turn_id, (unsigned long)s_edge_turn_id);
                break;
            }
            int cur_offset = slice_offsets[s];
            if (cur_offset + EI_COMMAND_WINDOW_SAMPLES > samples_available) {
                continue;
            }
            if ((s == 1 && cur_offset == slice_offsets[0]) ||
                (s == 2 && (cur_offset == slice_offsets[0] || cur_offset == slice_offsets[1]))) {
                continue;
            }
            float conf = 0.0f;
            const char *lbl = NULL;

            ESP_LOGI(TAG, ">>> [MODEL 2 ASYNC - SLICE %s] Offset: %d samples (%dms -> %dms) <<<",
                     slice_names[s], cur_offset, cur_offset / 16, (cur_offset + EI_COMMAND_WINDOW_SAMPLES) / 16);

            bool match = ei_command_classify(&s_cmd_infer_snapshot[cur_offset], EI_COMMAND_WINDOW_SAMPLES, &conf, &lbl);
            last_conf = conf;
            if (lbl) last_lbl = lbl;

            if (match && is_command_device_enabled(lbl)) {
                bool is_two_syllable_cmd = (lbl && (strcmp(lbl, "bat_den") == 0 || strcmp(lbl, "tat_den") == 0));
                if (is_two_syllable_cmd && speech_span_chunks >= 13) {
                    ESP_LOGW(TAG, ">>> [SPAN MISMATCH] Câu nói dài %d chunks (>= 13) -> Từ chối hoàn toàn nhãn 2 âm tiết '%s' (%.1f%%)! <<<",
                             speech_span_chunks, lbl, conf * 100.0f);
                    continue;
                }

                taskENTER_CRITICAL(&s_infer_mux);
                bool is_valid_match = (local_turn_id == s_edge_turn_id && s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
                if (is_valid_match) {
                    s_edge_turn_id++;
                    s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                }
                taskEXIT_CRITICAL(&s_infer_mux);

                if (!is_valid_match) {
                    ESP_LOGW(TAG, ">>> [MODEL 2 STALE RESULT] Turn đã bị hủy trước khi khớp lệnh '%s' -> Bỏ qua! <<<", lbl);
                    break;
                }

                ESP_LOGW(TAG, "==================================================");
                ESP_LOGW(TAG, ">>> [LOCAL COMMAND SUCCESS - SLICE %s (ASYNC)]: '%s' (Confidence: %.1f%%) <<<",
                         slice_names[s], lbl, conf * 100.0f);
                ESP_LOGW(TAG, "==================================================");
                dispatch_local_mesh_command(lbl);
                ui_display_notify_command_executed(lbl, 45);
                s_cloud_speaking_notified = false;
                ai_cloud_cancel_turn();
                ei_wakeword_reset();
                command_matched = true;
                break;
            } else if (match) {
                ESP_LOGI(TAG, ">>> [LOCAL IGNORE]: Khớp nhãn '%s' (%.1f%%) nhưng thiết bị đang tắt/chưa có -> Bỏ qua <<<",
                         lbl ? lbl : "none", conf * 100.0f);
            } else if (s == 0) {
                if ((lbl && strcmp(lbl, "unknown") == 0 && conf >= 0.70f) ||
                    (lbl && strcmp(lbl, "noise") == 0 && conf >= 0.75f)) {
                    ESP_LOGI(TAG, ">>> [MODEL 2 EARLY ROUTE] Lát cắt tâm rõ ràng không phải lệnh ('%s': %.1f%%) -> Chuyển thẳng Cloud! <<<",
                             lbl ? lbl : "none", conf * 100.0f);
                    break;
                }
            }
        }

        if (command_matched) {
            taskENTER_CRITICAL(&s_infer_mux);
            s_cmd_infer_busy = false;
            taskEXIT_CRITICAL(&s_infer_mux);
            continue;
        }

        taskENTER_CRITICAL(&s_infer_mux);
        bool is_valid_turn = (local_turn_id == s_edge_turn_id && s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
        bool need_end_turn = (is_valid_turn && s_edge_state == AI_EDGE_STATE_AI_RESPONDING);
        s_cmd_infer_busy = false;
        taskEXIT_CRITICAL(&s_infer_mux);

        if (!is_valid_turn) {
            continue;
        }

        // Cả 3 lát cắt không khớp lệnh -> Commit lượt sang Google Live API!
        ESP_LOGI(TAG, ">>> [HYBRID ROUTING (ASYNC)] Model 2 không khớp ('%s': %.1f%%) -> Commit buffer sang Gemini Live! <<<",
                 last_lbl ? last_lbl : "none", last_conf * 100.0f);

        if (sys_core_is_wifi_connected()) {
            ai_cloud_commit_turn();
            if (need_end_turn) {
                ai_cloud_end_turn();
            }
        } else {
            ESP_LOGE(TAG, ">>> [OFFLINE FALLBACK] Ngoại tuyến hoặc lỗi kết nối Live API! <<<");
            audio_hal_play_error_sound();
            ui_display_hide_voice_overlay();
            s_cloud_speaking_notified = false;
            ai_cloud_stop_session();
            ei_wakeword_reset();
            taskENTER_CRITICAL(&s_infer_mux);
            if (local_turn_id == s_edge_turn_id) {
                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
            }
            taskEXIT_CRITICAL(&s_infer_mux);
        }
    }
}

static void audio_recording_task(void *arg) {
    ESP_LOGI(TAG, "Audio Processing Task Started (Dual-Model Edge Impulse Engine).");

    // Khởi tạo Model 1 (Hey Cherry) và Model 2 (Smart Home Control)
    ei_wakeword_init();
    ei_command_init();

    // 1. Buffer lát cắt Wake Word (4000 mẫu = 250ms @ 16kHz = 8000 bytes trong PSRAM)
    size_t wake_slice_size = ei_wakeword_get_slice_size();
    if (wake_slice_size == 0) wake_slice_size = EI_WAKEWORD_SLICE_SAMPLES;
    int16_t *wake_slice_buffer = (int16_t *)heap_caps_malloc(wake_slice_size * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wake_slice_buffer) wake_slice_buffer = (int16_t *)heap_caps_malloc(wake_slice_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!wake_slice_buffer) wake_slice_buffer = (int16_t *)malloc(wake_slice_size * sizeof(int16_t));

    // 2. Buffer streaming 60ms cho mic (960 mẫu @ 16kHz = 1920 bytes)
    int16_t *stream_chunk_buffer = (int16_t *)heap_caps_malloc(STREAM_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stream_chunk_buffer) stream_chunk_buffer = (int16_t *)malloc(STREAM_CHUNK_SAMPLES * sizeof(int16_t));

    // 3. Buffer gom âm thanh trọn vẹn 1.5s cho Model 2 (24.000 mẫu @ 16kHz = 48.000 bytes trong PSRAM)
    int16_t *cmd_audio_buffer = (int16_t *)heap_caps_malloc(EI_COMMAND_BUFFER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!cmd_audio_buffer) cmd_audio_buffer = (int16_t *)malloc(EI_COMMAND_BUFFER_SAMPLES * sizeof(int16_t));

    // 6. Buffer Pre-roll 300ms (5 chunks * 60ms = 4800 samples) lùi giữ trọn vẹn phụ âm đầu ("b...", "t...")
    // Khớp 100% với khoảng lặng 250-300ms của thuật toán Edge Impulse Split khi cắt tập huấn luyện
    #define PREROLL_CHUNKS 5
    #define PREROLL_BUFFER_SIZE (PREROLL_CHUNKS * STREAM_CHUNK_SAMPLES * sizeof(int16_t))
    int16_t (*s_preroll_buffer)[STREAM_CHUNK_SAMPLES] = (int16_t (*)[STREAM_CHUNK_SAMPLES])heap_caps_malloc(PREROLL_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_preroll_buffer) {
        s_preroll_buffer = (int16_t (*)[STREAM_CHUNK_SAMPLES])malloc(PREROLL_BUFFER_SIZE);
    }
    if (s_preroll_buffer) {
        memset(s_preroll_buffer, 0, PREROLL_BUFFER_SIZE);
    } else {
        ESP_LOGE(TAG, "Failed to allocate s_preroll_buffer in PSRAM!");
    }
    int preroll_count = 0;
    int preroll_idx = 0;

    size_t cmd_samples_from_speech = 0;
    bool command_speech_started = false;
    bool wake_dma_flushed = false;
    int command_total_chunks = 0;
    int cmd_start_voice_streak = 0;
    bool cmd_inference_dispatched = false;

    int streaming_counter = 0;
    int idle_slice_counter = 0;
    int responding_log_counter = 0;
    int prev_wake_rms = 0;

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " HỆ THỐNG LẮNG NGHE ĐÃ SẴN SÀNG (HYBRID DUAL-MODEL)!");
    ESP_LOGI(TAG, " - Wake Word: 'Hey Cherry' (Continuous 250ms)");
    ESP_LOGI(TAG, " - Command Control: 6 Smart Home Labels (1.5s Buffer, Decoupled Core 1 Task)");
    ESP_LOGI(TAG, " - Cloud AI: Google Gemini Live API Streaming");
    ESP_LOGI(TAG, "==================================================");

    while (1) {
        switch (s_edge_state) {
            case AI_EDGE_STATE_IDLE_WAKEWORD: {
                // Mutually Exclusive Guard: Không chạy Wake Word khi Model 2 đang suy luận
                if (s_cmd_infer_busy) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    break;
                }

                // Kiểm tra yêu cầu kích hoạt thủ công từ nút nhấn BOOT
                if (s_manual_wake_requested) {
                    s_manual_wake_requested = false;
                    ESP_LOGI(TAG, "**************************************************");
                    ESP_LOGI(TAG, ">>> KÍCH HOẠT THỦ CÔNG TỪ NÚT BẤM BOOT! <<<");
                    ESP_LOGI(TAG, "**************************************************");
#if !DEBUG_LOCAL_TEST_MODE
                    if (sys_core_is_wifi_connected()) {
                        ai_cloud_prewarm_session();
                    }
#endif
                    audio_hal_play_wake_sound();
                    ui_display_show_voice_overlay();

                    cmd_samples_from_speech = 0;
                    command_speech_started = false;
                    wake_dma_flushed = false;
                    command_total_chunks = 0;
                    cmd_start_voice_streak = 0;
                    cmd_inference_dispatched = false;
                    s_cloud_speaking_notified = false;
                    s_edge_turn_id++;
                    voice_endpointer_init(&s_endpointer);
                    preroll_count = 0;
                    preroll_idx = 0;
                    if (s_preroll_buffer) memset(s_preroll_buffer, 0, PREROLL_BUFFER_SIZE);
                    if (cmd_audio_buffer) memset(cmd_audio_buffer, 0, EI_COMMAND_BUFFER_SAMPLES * sizeof(int16_t));
                    ei_command_reset_window();

                    s_edge_state = AI_EDGE_STATE_COMMAND_LISTENING;
                    break;
                }

                // Đọc lát cắt 250ms (4000 mẫu) từ mic DMA
                size_t bytes_to_read = wake_slice_size * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(wake_slice_buffer, bytes_to_read);

                if (bytes_read == bytes_to_read) {
                    float confidence = 0.0f;
                    const char *label = NULL;

                    // Lọc DC offset và rung chấn cơ học
                    audio_hal_apply_dc_filter(wake_slice_buffer, wake_slice_size);

                    int rms = audio_vad_get_rms(wake_slice_buffer, wake_slice_size);
                    int ambient = audio_dsp_get_ambient_noise();
                    int max_rms_2slices = (rms > prev_wake_rms) ? rms : prev_wake_rms;

                    idle_slice_counter++;
                    // In nhịp tim định kỳ mỗi ~3 giây (12 lát * 250ms)
                    if (idle_slice_counter % 12 == 0) {
                        ESP_LOGD(TAG, "[IDLE_WAKEWORD] Đang lắng nghe 'Hey Cherry'... (Mic RMS: %d, Sàn ồn: %d)",
                                 rms, ambient);
                    }

                    // Chạy nhận diện Wake Word
                    bool detected = ei_wakeword_process_slice(wake_slice_buffer, wake_slice_size, &confidence, &label);

                    // Chỉ gọi audio_dsp_update_ambient_noise(rms) khi lát cắt KHÔNG phải là tiếng nói
                    if (confidence < 0.30f && rms < ambient * 2) {
                        audio_dsp_update_ambient_noise(rms);
                    }

                    // Chốt chặn SNR Gate thích ứng theo nhiễu nền:
                    if (detected) {
                        int min_snr_rms = ambient + 4;
                        if (max_rms_2slices < 20 || max_rms_2slices < min_snr_rms) {
                            ESP_LOGW(TAG, ">>> [SNR GATE BLOCKED] Bỏ qua kích hoạt giả (Score: %.1f%%, RMS: %d, Max2Slices: %d < 20 hoặc < Sàn ồn + 4: %d) <<<",
                                     confidence * 100.0f, rms, max_rms_2slices, min_snr_rms);
                            detected = false;
                        }
                    }

                    if (detected) {
                        prev_wake_rms = 0;
                        ESP_LOGI(TAG, "**************************************************");
                        ESP_LOGI(TAG, ">>> PHÁT HIỆN TỪ KHÓA 'HEY CHERRY'! (Score: %.1f%%, RMS: %d, Max2Slices: %d) <<<", confidence * 100.0f, rms, max_rms_2slices);
                        ESP_LOGI(TAG, "**************************************************");

#if !DEBUG_LOCAL_TEST_MODE
                        if (sys_core_is_wifi_connected()) {
                            ai_cloud_prewarm_session();
                        }
#endif
                        audio_hal_play_wake_sound();
                        ui_display_show_voice_overlay();

                        cmd_samples_from_speech = 0;
                        command_speech_started = false;
                        wake_dma_flushed = false;
                        command_total_chunks = 0;
                        cmd_start_voice_streak = 0;
                        cmd_inference_dispatched = false;
                        s_cloud_speaking_notified = false;
                        s_edge_turn_id++;
                        voice_endpointer_init(&s_endpointer);
                        preroll_count = 0;
                        preroll_idx = 0;
                        if (s_preroll_buffer) {
                            memset(s_preroll_buffer, 0, PREROLL_BUFFER_SIZE);
                        }
                        if (cmd_audio_buffer) {
                            memset(cmd_audio_buffer, 0, EI_COMMAND_BUFFER_SAMPLES * sizeof(int16_t));
                        }
                        ei_command_reset_window();

                        s_edge_state = AI_EDGE_STATE_COMMAND_LISTENING;
                    } else {
                        prev_wake_rms = rms;
                    }
                } else {
                    ESP_LOGW(TAG, "Mic read mismatch: %u != %u", (unsigned int)bytes_read, (unsigned int)bytes_to_read);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                vTaskDelay(pdMS_TO_TICKS(2));
                break;
            }

            case AI_EDGE_STATE_COMMAND_LISTENING: {
                if (s_manual_finish_requested) {
                    s_manual_finish_requested = false;
                    if (!command_speech_started) {
                        ESP_LOGI(TAG, ">>> MANUAL FINISH REQUESTED (Touch Button) khi chưa nói -> Hủy lượt gọn gàng! <<<");
                        taskENTER_CRITICAL(&s_infer_mux);
                        s_edge_turn_id++;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        taskEXIT_CRITICAL(&s_infer_mux);
                        ui_display_hide_voice_overlay();
                        s_cloud_speaking_notified = false;
                        ai_cloud_stop_session();
                        ei_wakeword_reset();
                    } else {
                        ESP_LOGI(TAG, ">>> MANUAL FINISH REQUESTED (Touch Button): CHỐT CÂU LỆNH NGAY LẬP TỨC <<<");
                        taskENTER_CRITICAL(&s_infer_mux);
                        s_edge_turn_id++;
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                        taskEXIT_CRITICAL(&s_infer_mux);
                        ei_wakeword_reset();
                        ai_cloud_commit_turn();
                        ai_cloud_end_turn();
                        s_responding_silent_cycles = 0;
                        responding_log_counter = 0;
                    }
                    break;
                }
                prev_wake_rms = 0;
                size_t bytes_to_read = STREAM_CHUNK_SAMPLES * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(stream_chunk_buffer, bytes_to_read);

                if (bytes_read == bytes_to_read) {
                    audio_hal_apply_dc_filter(stream_chunk_buffer, STREAM_CHUNK_SAMPLES);

                    // 1. Quản lý thời điểm bắt đầu thu âm: Mở sau khi chuông Ding kết thúc
                    if (!audio_hal_is_wake_recording_ready()) {
                        break;
                    }

                    // Ngay khi chuông Ding vừa dứt: xả sạch DMA RX và reset bộ lọc DC
                    if (!wake_dma_flushed) {
                        wake_dma_flushed = true;
                        audio_hal_flush_rx();
                        audio_hal_reset_dc_filter();
                        audio_vad_reset_trigger();
                        voice_endpointer_init(&s_endpointer);
                        preroll_count = 0;
                        preroll_idx = 0;
                        if (s_preroll_buffer) {
                            memset(s_preroll_buffer, 0, PREROLL_BUFFER_SIZE);
                        }
                        ESP_LOGI(TAG, ">>> [RECORDING OPEN] Chuông Ding đã dứt -> Bắt đầu thu âm câu lệnh! <<<");
                        break;
                    }

                    int rms = 0;
                    float voice_thresh = 0.0f;
                    bool has_voice = voice_endpointer_feed(&s_endpointer, stream_chunk_buffer, STREAM_CHUNK_SAMPLES, &rms, &voice_thresh);

                    command_total_chunks++;

                    // 2. Quản lý Pre-roll và Kích hoạt Speech Start:
                    if (!command_speech_started) {
                        // Tuyệt đối không đếm im lặng trước khi người dùng mở miệng nói!
                        s_endpointer.silence_counter = 0;

                        if (s_preroll_buffer) {
                            memcpy(s_preroll_buffer[preroll_idx], stream_chunk_buffer, sizeof(int16_t) * STREAM_CHUNK_SAMPLES);
                            preroll_idx = (preroll_idx + 1) % PREROLL_CHUNKS;
                            if (preroll_count < PREROLL_CHUNKS) preroll_count++;
                        }

                        if (has_voice) {
                            cmd_start_voice_streak++;
                        } else {
                            cmd_start_voice_streak = 0;
                        }

                        if (cmd_start_voice_streak >= 2) {
                            command_speech_started = true;
                            s_endpointer.silence_counter = 0;
                            s_endpointer.speech_streak = 2;
                            ESP_LOGI(TAG, ">>> [VOICE START] Bắt đầu nói (RMS: %d, Thresh: %d, Sàn ồn: %d) -> Lùi %dms giữ trọn phụ âm đầu! <<<",
                                     rms, (int)voice_thresh, (int)s_endpointer.noise_floor_ema, preroll_count * 60);

                            // Kích hoạt Stream song song lên Cloud (Buffered mode)
                            if (sys_core_is_wifi_connected()) {
                                ai_cloud_start_turn();
                            }

                            // Nạp các chunk Pre-roll vào đầu cmd_audio_buffer VÀ stream ngay lên Cloud
                            cmd_samples_from_speech = 0;
                            if (s_preroll_buffer) {
                                int start_idx = (preroll_idx - preroll_count + PREROLL_CHUNKS) % PREROLL_CHUNKS;
                                for (int p = 0; p < preroll_count; p++) {
                                    int cur_p = (start_idx + p) % PREROLL_CHUNKS;
                                    if (cmd_audio_buffer && (cmd_samples_from_speech + STREAM_CHUNK_SAMPLES <= EI_COMMAND_BUFFER_SAMPLES)) {
                                        memcpy(&cmd_audio_buffer[cmd_samples_from_speech], s_preroll_buffer[cur_p], STREAM_CHUNK_SAMPLES * sizeof(int16_t));
                                        cmd_samples_from_speech += STREAM_CHUNK_SAMPLES;
                                    }
                                    if (sys_core_is_wifi_connected()) {
                                        ai_cloud_send_audio_chunk((const uint8_t *)s_preroll_buffer[cur_p], STREAM_CHUNK_SAMPLES * sizeof(int16_t));
                                    }
                                }
                            }
                        } else if (!has_voice) {
                            audio_dsp_update_ambient_noise(rms);
                            // Quá ~4.0s không ai nói gì: No-Speech Guard (67 chunks * 60ms = 4.02s)
                            if (command_total_chunks >= 67) {
                                ESP_LOGI(TAG, ">>> [NO-SPEECH GUARD] Không phát hiện tiếng nói sau 4.0s -> Hủy lượt & tiếp tục lắng nghe. <<<");
                                s_edge_turn_id++;
                                ui_display_hide_voice_overlay();
                                ai_cloud_stop_session();
                                ei_wakeword_reset();
                                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                                break;
                            }
                            break;
                        } else {
                            break;
                        }
                    } else {
                        // Đã bắt đầu nói: Stream song song từng chunk 60ms lên Cloud theo thời gian thực (Zero Lag)
                        if (sys_core_is_wifi_connected()) {
                            ai_cloud_send_audio_chunk((const uint8_t *)stream_chunk_buffer, bytes_read);
                        }

                        // Tiếp tục gom âm thanh vào cmd_audio_buffer cho Model 2 (tối đa 1.5s = 24.000 mẫu)
                        if (cmd_audio_buffer && (cmd_samples_from_speech + STREAM_CHUNK_SAMPLES <= EI_COMMAND_BUFFER_SAMPLES)) {
                            memcpy(&cmd_audio_buffer[cmd_samples_from_speech], stream_chunk_buffer, STREAM_CHUNK_SAMPLES * sizeof(int16_t));
                            cmd_samples_from_speech += STREAM_CHUNK_SAMPLES;
                        }
                    }

                    // 3. Đánh thức luồng suy luận Model 2 bất đồng bộ (Decoupled Task Trigger):
                    // Khi gom đủ 1.5s HOẶC khi phát hiện im lặng dứt câu sớm (tối thiểu 12800 mẫu = 0.8s tính cả pre-roll)
                    bool early_endpoint = (cmd_samples_from_speech >= 12800 && s_endpointer.silence_counter >= STREAM_SILENCE_TARGET);
                    if (!cmd_inference_dispatched && command_speech_started &&
                        (cmd_samples_from_speech >= EI_COMMAND_BUFFER_SAMPLES || early_endpoint)) {
                        cmd_inference_dispatched = true;

                        if (s_cmd_infer_snapshot && s_cmd_infer_task_handle) {
                            memset(s_cmd_infer_snapshot, 0, EI_COMMAND_BUFFER_SAMPLES * sizeof(int16_t));
                            memcpy(s_cmd_infer_snapshot, cmd_audio_buffer, cmd_samples_from_speech * sizeof(int16_t));
                            taskENTER_CRITICAL(&s_infer_mux);
                            s_cmd_infer_sample_count = cmd_samples_from_speech;
                            s_cmd_infer_turn_id = s_edge_turn_id;
                            s_cmd_infer_noise_floor = (int)s_endpointer.noise_floor_ema;
                            s_cmd_infer_busy = true;
                            taskEXIT_CRITICAL(&s_infer_mux);
                            xTaskNotify(s_cmd_infer_task_handle, CMD_INFER_EVT_START, eSetBits);
                        }

                        if (early_endpoint) {
                            ESP_LOGI(TAG, ">>> [EARLY SILENCE ENDPOINT] Dứt câu sớm (%dms im lặng, %u mẫu) -> Chờ Model 2 / phản hồi AI! <<<",
                                     s_endpointer.silence_counter * 60, (unsigned int)cmd_samples_from_speech);
                            s_responding_silent_cycles = 0;
                            s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                            responding_log_counter = 0;
                            break;
                        } else {
                            s_edge_state = AI_EDGE_STATE_STREAMING_SPEECH;
                            streaming_counter = 0;
                            break;
                        }
                    }
                }
                break;
            }

            case AI_EDGE_STATE_STREAMING_SPEECH: {
                if (s_manual_finish_requested) {
                    s_manual_finish_requested = false;
                    ESP_LOGI(TAG, ">>> MANUAL FINISH REQUESTED (Touch Button): CHỐT CÂU LỆNH NGAY LẬP TỨC <<<");
                    taskENTER_CRITICAL(&s_infer_mux);
                    s_edge_turn_id++;
                    s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                    taskEXIT_CRITICAL(&s_infer_mux);
                    ei_wakeword_reset();
                    ai_cloud_commit_turn();
                    ai_cloud_end_turn();
                    s_responding_silent_cycles = 0;
                    responding_log_counter = 0;
                    break;
                }
                prev_wake_rms = 0;
                size_t bytes_to_read = STREAM_CHUNK_SAMPLES * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(stream_chunk_buffer, bytes_to_read);

                if (bytes_read > 0) {
                    audio_hal_apply_dc_filter(stream_chunk_buffer, bytes_read / sizeof(int16_t));

                    int rms = 0;
                    float voice_thresh = 0.0f;
                    bool has_voice = voice_endpointer_feed(&s_endpointer, stream_chunk_buffer, bytes_read / sizeof(int16_t), &rms, &voice_thresh);

                    ai_cloud_send_audio_chunk((uint8_t *)stream_chunk_buffer, bytes_read);

                    streaming_counter++;
                    if (streaming_counter % 8 == 0) {
                        ESP_LOGI(TAG, "[STREAM VAD] RMS: %4d (Thresh: %3d, Floor: %2d) | %s | Silence: %2d/%d",
                                 rms, (int)voice_thresh, (int)s_endpointer.noise_floor_ema,
                                 has_voice ? "CÓ TIẾNG" : "Im lặng  ",
                                 s_endpointer.silence_counter, STREAM_SILENCE_TARGET);
                    }

                    // DỨT CÂU NÓI HỢP LỆ (12 chunks * 60ms = 720ms im lặng)
                    if (s_endpointer.silence_counter >= STREAM_SILENCE_TARGET) {
                        ESP_LOGI(TAG, ">>> [CLIENT VAD] Dứt câu nói (%dms im lặng) -> Chuyển sang chờ Model 2 / Gemini Live <<<", STREAM_SILENCE_TARGET * 60);
                        taskENTER_CRITICAL(&s_infer_mux);
                        bool infer_still_busy = s_cmd_infer_busy;
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                        taskEXIT_CRITICAL(&s_infer_mux);
                        ei_wakeword_reset();
                        if (!infer_still_busy) {
                            ai_cloud_commit_turn();
                            ai_cloud_end_turn();
                        }
                        s_responding_silent_cycles = 0;
                        responding_log_counter = 0;
                        break;
                    }

                    // Giới hạn an toàn: Nói tối đa 15 giây thì tự ngắt câu
                    if (streaming_counter >= (15 * 1000 / 60)) {
                        ESP_LOGW(TAG, "Đã đạt giới hạn tối đa 15 giây -> Tự động kết thúc câu hỏi!");
                        taskENTER_CRITICAL(&s_infer_mux);
                        s_edge_turn_id++;
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                        taskEXIT_CRITICAL(&s_infer_mux);
                        ei_wakeword_reset();
                        ai_cloud_commit_turn();
                        ai_cloud_end_turn();
                        s_responding_silent_cycles = 0;
                        responding_log_counter = 0;
                        break;
                    }
                }
                break;
            }

            case AI_EDGE_STATE_AI_RESPONDING: {
                prev_wake_rms = 0;
                // Xả sạch DMA mic bằng các chunk nhỏ 60ms để âm thanh dội từ loa không bị tồn đọng
                size_t bytes_to_read = STREAM_CHUNK_SAMPLES * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(stream_chunk_buffer, bytes_to_read);
                (void)bytes_read;

                bool cloud_busy = ai_cloud_is_busy();
                bool speaker_playing = audio_hal_is_playing();

                if (!s_cmd_infer_busy && (cloud_busy || speaker_playing) && !s_cloud_speaking_notified) {
                    s_cloud_speaking_notified = true;
                    ui_display_notify_cloud_speaking();
                }

                responding_log_counter++;
                if (responding_log_counter % 25 == 0) {
                    ESP_LOGD(TAG, "[AI_RESPONDING] Đang chờ phản hồi & phát âm thanh... (Cloud bận: %d, Loa phát: %d, SilentCycles: %d)",
                             cloud_busy, speaker_playing, s_responding_silent_cycles);
                }

                // Cầu chì an toàn lớp cuối: Nếu loa im lặng liên tục > 15.0 giây (250 chu kỳ * 60ms,
                // lớn hơn thời gian xả TX + RESPONSE_TIMEOUT_MS 8s của ai_cloud) mà Cloud vẫn bận -> Giải phóng về IDLE!
                if (speaker_playing) {
                    s_responding_silent_cycles = 0;
                } else if (cloud_busy) {
                    s_responding_silent_cycles++;
                    if (s_responding_silent_cycles >= 250) {
                        ESP_LOGW(TAG, ">>> [AI_RESPONDING WATCHDOG] Loa im lặng liên tục > 15.0s nhưng Cloud vẫn bận -> Cưỡng bức đóng phiên về IDLE! <<<");
                        s_responding_silent_cycles = 0;
                        ui_display_hide_voice_overlay();
                        s_cloud_speaking_notified = false;
                        ai_cloud_stop_session();
                        ei_wakeword_reset();
                        idle_slice_counter = 0;
                        wake_dma_flushed = false;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        break;
                    }
                }

                // Kiểm tra khi Model 2 đã xong, Cloud đã trả lời xong VÀ loa đã xả hết âm thanh
                // Chế độ 1 Hỏi - 1 Đáp (Single-Turn): Đóng phiên sạch sẽ & trở về chờ 'Hey Cherry'
                if (!s_cmd_infer_busy && !cloud_busy && !speaker_playing) {
                    ESP_LOGI(TAG, ">>> [TURN COMPLETE] AI trả lời hoàn tất & loa đã nghỉ -> Trở về lắng nghe 'Hey Cherry' <<<");
                    ui_display_hide_voice_overlay();
                    s_cloud_speaking_notified = false;
                    s_responding_silent_cycles = 0;
                    ai_cloud_stop_session();
                    ei_wakeword_reset();
                    idle_slice_counter = 0;
                    wake_dma_flushed = false;
                    s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                }
                break;
            }
        }
    }

    if (wake_slice_buffer) free(wake_slice_buffer);
    if (stream_chunk_buffer) free(stream_chunk_buffer);
    if (cmd_audio_buffer) free(cmd_audio_buffer);
    if (s_preroll_buffer) free(s_preroll_buffer);
    vTaskDelete(NULL);
}

static void button_trigger_task(void *arg) {
    ESP_LOGI(TAG, "Button Trigger Ready. (Nhấn nhả để kích hoạt/ngắt lời, giữ 5s để reset Wi-Fi)");

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 1,
    };
    gpio_config(&io_conf);

    int press_ticks = 0;
    bool button_was_pressed = false;

    while (1) {
        bool is_pressed = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);

        if (is_pressed) {
            press_ticks++;
            button_was_pressed = true;
            if (press_ticks == 100) { // 100 * 50ms = 5 giây
                ESP_LOGW(TAG, "BOOT button held for 5s. Resetting Wi-Fi credentials...");
                sys_core_reset_wifi();
            }
        } else {
            if (button_was_pressed) {
                // Nhấn nhả (Short press < 1.5s = 30 ticks của 50ms)
                if (press_ticks > 0 && press_ticks < 30) {
                    switch (s_edge_state) {
                        case AI_EDGE_STATE_IDLE_WAKEWORD: {
                            ESP_LOGI(TAG, ">>> BOOT BUTTON CLICK: KÍCH HOẠT THỦ CÔNG <<<");
                            s_manual_wake_requested = true;
                            break;
                        }

                        case AI_EDGE_STATE_COMMAND_LISTENING:
                        case AI_EDGE_STATE_STREAMING_SPEECH: {
                            ESP_LOGI(TAG, ">>> BOOT BUTTON CLICK: CHỐT CÂU LỆNH NGAY LẬP TỨC <<<");
                            ai_edge_finish_speaking();
                            break;
                        }

                        case AI_EDGE_STATE_AI_RESPONDING: {
                            ESP_LOGI(TAG, ">>> BOOT BUTTON CLICK: BARGE-IN NGẮT LỜI AI <<<");
                            ai_edge_abort();
                            s_cloud_speaking_notified = false;
                            s_responding_silent_cycles = 0;
                            s_manual_wake_requested = true;
                            break;
                        }
                    }
                }
                press_ticks = 0;
                button_was_pressed = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ai_edge_init(void) {
    ESP_LOGI(TAG, "Initializing Edge AI (Dual-Model Edge Impulse + Hybrid Cloud Routing)");
    
    // Đăng ký kiểm tra trạng thái bận với sys_core để hoãn quét Wi-Fi khi đang tương tác giọng nói
    sys_core_set_voice_busy_check(ai_edge_is_busy);

    // Cấp phát snapshot buffer trong PSRAM cho Model 2 async inference
    s_cmd_infer_snapshot = (int16_t *)heap_caps_malloc(EI_COMMAND_BUFFER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_cmd_infer_snapshot) {
        ESP_LOGE(TAG, "Failed to allocate s_cmd_infer_snapshot in PSRAM!");
    }

    // Khởi tạo luồng suy luận Model 2 bất đồng bộ (Core 1, Priority 4, Stack 24KB PSRAM)
    BaseType_t infer_ret = xTaskCreatePinnedToCoreWithCaps(cmd_infer_task, "cmd_infer_task", 24576, NULL, 4, &s_cmd_infer_task_handle, 1, MALLOC_CAP_SPIRAM);
    if (infer_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create cmd_infer_task in PSRAM (ret=%d), trying DRAM...", (int)infer_ret);
        xTaskCreatePinnedToCore(cmd_infer_task, "cmd_infer_task", 16384, NULL, 4, &s_cmd_infer_task_handle, 1);
    }

    // Cấp phát button_task trong PSRAM (SPIRAM) để giải phóng DRAM nội
    xTaskCreatePinnedToCoreWithCaps(button_trigger_task, "button_task", 4096, NULL, 5, &button_task_handle, 1, MALLOC_CAP_SPIRAM);

    // Cấp phát Stack 32KB trong PSRAM (SPIRAM) để giải phóng hoàn toàn DRAM nội cho DMA và đảm bảo đủ bộ nhớ cho TFLite Micro inference
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(audio_recording_task, "audio_task", 32768, NULL, 6, &audio_task_handle, 1, MALLOC_CAP_SPIRAM);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "FAILED to create audio_task in PSRAM (ret=%d)! Thử fallback về Internal DRAM...", (int)task_ret);
        task_ret = xTaskCreatePinnedToCore(audio_recording_task, "audio_task", 16384, NULL, 6, &audio_task_handle, 1);
        if (task_ret != pdPASS) {
            ESP_LOGE(TAG, "CRITICAL: audio_task creation failed completely in DRAM as well! (ret=%d)", (int)task_ret);
        }
    } else {
        ESP_LOGI(TAG, ">>> SUCCESS: audio_task created with 32KB stack in Octal PSRAM (SPIRAM)! <<<");
    }
}

void ai_edge_stop_recording(void) {
    taskENTER_CRITICAL(&s_infer_mux);
    bool was_active = (s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
    if (was_active) {
        s_edge_turn_id++;
        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
    }
    taskEXIT_CRITICAL(&s_infer_mux);

    if (was_active) {
        ESP_LOGI(TAG, "Dừng ghi âm khẩn cấp (Hủy bởi Cloud/Socket Error).");
        s_cloud_speaking_notified = false;
        ei_wakeword_reset();
        ui_display_hide_voice_overlay();
    }
}

void ai_edge_abort(void) {
    taskENTER_CRITICAL(&s_infer_mux);
    bool was_active = (s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
    if (was_active) {
        s_edge_turn_id++;
        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
    }
    taskEXIT_CRITICAL(&s_infer_mux);

    if (was_active) {
        ESP_LOGI(TAG, "Huỷ phiên tương tác giọng nói (Abort requested by Touch/Button)");
        s_cloud_speaking_notified = false;
        ai_cloud_stop_session();
        audio_hal_stop_playback();
        ei_wakeword_reset();
        ui_display_hide_voice_overlay();
    }
}

bool ai_edge_is_recording(void) {
    return (s_edge_state == AI_EDGE_STATE_COMMAND_LISTENING || 
            s_edge_state == AI_EDGE_STATE_STREAMING_SPEECH);
}

bool ai_edge_is_busy(void) {
    return (s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
}

ai_edge_state_t ai_edge_get_state(void) {
    return s_edge_state;
}
