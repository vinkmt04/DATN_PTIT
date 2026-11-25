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
#include "esp_timer.h"
#include "ei_wrapper.h"
#include "mesh_controller.h"
#include "gateway_state_manager.h"
#include "ui_display.h"
#include <string.h>
#include <math.h>

static const char *TAG = "ai_edge";

#define BOOT_BUTTON_GPIO         0
#define STREAM_CHUNK_SAMPLES            960    // 60ms @ 16kHz
#define STREAM_SILENCE_TARGET           30     // 30 * 60ms = 1800ms im lặng dự phòng (Client VAD Fallback; Server Neural VAD chốt chính)
#define PRE_COMMIT_CLEAN_SILENCE_CHUNKS 14     // 14 * 60ms = 840ms im lặng sạch trước khi bật Wi-Fi TX (chốt câu ngắn cho Model 2)

// =============================================================================
// UNIFIED VOICE ENDPOINTER (Single Truth VAD State Machine)
// =============================================================================
typedef struct {
    float noise_floor_ema;   // Khởi tạo từ ambient noise, thích ứng khi không có giọng nói
    float speech_rms_ema;    // Khởi tạo 250.0f, thích ứng theo âm lượng người nói
    int   speech_streak;     // Số chunks liên tiếp có tiếng nói
    int   silence_counter;   // Bộ đếm im lặng (chỉ reset khi speech_streak >= 2)
} voice_endpointer_t;

static inline void voice_endpointer_init(voice_endpointer_t *ep) {
    if (!ep) return;
    float amb = (float)audio_dsp_get_ambient_noise();
    if (amb < 15.0f) amb = 15.0f;
    if (amb > 180.0f) amb = 180.0f;
    ep->noise_floor_ema = amb;
    ep->speech_rms_ema = fmaxf(amb * 3.0f, 120.0f);
    ep->speech_streak = 0;
    ep->silence_counter = 0;
}

static inline bool voice_endpointer_feed(voice_endpointer_t *ep, const int16_t *chunk, size_t samples,
                                         bool is_speaking_turn, int *out_rms, float *out_thresh, bool *out_webrtc) {
    int rms = audio_vad_get_rms(chunk, samples);
    if (out_rms) *out_rms = rms;

    bool raw_webrtc = audio_vad_raw_webrtc_chunk(chunk, samples);
    if (out_webrtc) *out_webrtc = raw_webrtc;

    // Ngưỡng tiếng nói động thống nhất (Unified Dynamic Threshold)
    float noise_gate    = ep->noise_floor_ema * 1.65f + 22.0f;
    float rel_gate      = ep->speech_rms_ema * 0.22f;
    float voice_thresh  = fmaxf(noise_gate, rel_gate);
    if (out_thresh) *out_thresh = voice_thresh;

    bool is_speech = false;
    if (!is_speaking_turn) {
        // Trước [VOICE START]: bắt buộc thỏa WebRTC VAD + ngưỡng năng lượng, không dùng nhánh rescue bỏ qua WebRTC
        float start_thresh = fmaxf(ep->noise_floor_ema * 2.0f + 35.0f, 75.0f);
        is_speech = raw_webrtc && ((float)rms >= start_thresh);
    } else {
        float rescue_thresh = ep->noise_floor_ema * 2.4f + 55.0f;
        is_speech = (raw_webrtc && ((float)rms >= voice_thresh)) || ((float)rms >= rescue_thresh);
    }

    if (is_speech) {
        ep->speech_streak++;
        ep->speech_rms_ema = ep->speech_rms_ema * 0.85f + (float)rms * 0.15f;
        if (ep->speech_rms_ema < 80.0f) ep->speech_rms_ema = 80.0f;
        // Soft Silence Counter: Chỉ reset về 0 khi có tối thiểu 2 chunks liên tiếp có tiếng (120ms: chặn gai nhiễu 1-chunk)
        if (ep->speech_streak >= 2) {
            ep->silence_counter = 0;
        }
    } else {
        ep->speech_streak = 0;
        ep->silence_counter++;
        if (!raw_webrtc) {
            if (is_speaking_turn) {
                // Khóa tăng noise_floor_ema trong lúc người dùng đang nói (chỉ cho phép giảm nhẹ)
                if ((float)rms < ep->noise_floor_ema) {
                    ep->noise_floor_ema = ep->noise_floor_ema * 0.85f + (float)rms * 0.15f;
                }
            } else {
                if ((float)rms < ep->noise_floor_ema) {
                    ep->noise_floor_ema = ep->noise_floor_ema * 0.80f + (float)rms * 0.20f;
                } else if ((float)rms <= ep->noise_floor_ema * 1.6f) {
                    ep->noise_floor_ema = ep->noise_floor_ema * 0.95f + (float)rms * 0.05f;
                }
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
static int16_t *s_cmd_infer_centered_buf = NULL;
static uint8_t s_cmd_infer_webrtc_flags[25] = {0};
static volatile bool s_cmd_infer_busy = false;
static volatile size_t s_cmd_infer_sample_count = 0;
static volatile uint32_t s_edge_turn_id = 0;
static volatile uint32_t s_cmd_infer_turn_id = 0;
static volatile int s_cmd_infer_noise_floor = 25;
static volatile bool s_turn_cloud_attached = false;
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

static bool is_command_device_online(const char *cmd_label) {
    if (!cmd_label) return false;
    gateway_device_state_t st;
    gateway_state_get(&st);
    if (strcmp(cmd_label, "bat_den") == 0 || strcmp(cmd_label, "tat_den") == 0) {
        return st.dimmer_node_online;
    }
    if (strcmp(cmd_label, "bat_cong_tac") == 0 || strcmp(cmd_label, "tat_cong_tac") == 0) {
        return st.relay_node_online;
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
        uint8_t local_webrtc_flags[25];
        memcpy(local_webrtc_flags, s_cmd_infer_webrtc_flags, sizeof(local_webrtc_flags));
        taskEXIT_CRITICAL(&s_infer_mux);

        int num_chunks = raw_samples / STREAM_CHUNK_SAMPLES;
        if (num_chunks > 25) num_chunks = 25;

        // Bước 1: Tính mảng chunk_rms & đếm số chunk có cờ WebRTC trong snapshot thực tế
        int chunk_rms[25] = {0};
        int max_speech_rms = 0;
        int webrtc_speech_chunks = 0;
        for (int k = 0; k < num_chunks; k++) {
            chunk_rms[k] = audio_vad_get_rms(&s_cmd_infer_snapshot[k * STREAM_CHUNK_SAMPLES], STREAM_CHUNK_SAMPLES);
            if (chunk_rms[k] > max_speech_rms) {
                max_speech_rms = chunk_rms[k];
            }
            if (local_webrtc_flags[k]) {
                webrtc_speech_chunks++;
            }
        }

        int min_ambient_gate = ambient_noise * 2 + 20;
        int rel_peak_gate = (int)(max_speech_rms * 0.32f);
        int user_voice_thresh = (min_ambient_gate > rel_peak_gate) ? min_ambient_gate : rel_peak_gate;

        // Near-field Voice Validation (thích ứng theo sàn ồn thực tế thay vì ngưỡng cứng 150)
        int min_valid_rms = ambient_noise * 2 + 45;
        if (min_valid_rms < 75) min_valid_rms = 75;
        if (max_speech_rms < min_valid_rms) {
            ESP_LOGW(TAG, ">>> [NOISE REJECTION (ASYNC)] Tạp âm môi trường xa (MaxRMS: %d < %d) -> Hủy bỏ lượt thu âm <<<",
                     max_speech_rms, min_valid_rms);
            taskENTER_CRITICAL(&s_infer_mux);
            bool is_valid_turn = (local_turn_id == s_edge_turn_id && s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
            if (is_valid_turn) {
                s_edge_turn_id++;
                s_turn_cloud_attached = false;
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

        int b_ambient_gate = (int)(ambient_noise * 1.6f + 15.0f);
        int b_rel_gate = (int)(max_speech_rms * 0.18f);
        int boundary_thresh = (b_ambient_gate > b_rel_gate) ? b_ambient_gate : b_rel_gate;

        // Bước 3: Đếm tổng chunk giọng nói (user_voice_thresh) & xác định biên âm tiết mềm (boundary_thresh)
        int first_speech_chunk = -1;
        int last_speech_chunk = -1;
        int b_first_chunk = -1;
        int b_last_chunk = -1;
        int total_user_chunks = 0;
        int tail_active_chunks = 0;
        int silence_gap = 0;
        int b_silence_gap = 0;
        bool main_utterance_closed = false;
        bool b_utterance_closed = false;

        for (int k = 0; k < num_chunks; k++) {
            if (chunk_rms[k] >= boundary_thresh) {
                if (b_first_chunk == -1) b_first_chunk = k;
                if (!b_utterance_closed) {
                    b_last_chunk = k;
                }
                b_silence_gap = 0;
            } else if (b_first_chunk != -1) {
                b_silence_gap++;
                if (b_silence_gap >= 3) {
                    b_utterance_closed = true;
                }
            }

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

        // Continuous Long Speech check (siết chặt hơn để không bị 1 tiếng động cuối 1.5s bỏ qua Model 2)
        bool is_continuous_long_speech = (num_chunks >= 25) &&
                                         (!main_utterance_closed) &&
                                         (tail_active_chunks >= 3) &&
                                         (chunk_rms[23] >= user_voice_thresh && chunk_rms[24] >= user_voice_thresh) &&
                                         (speech_span_chunks >= 15);

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
                        ai_cloud_end_turn_manual();
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
                        s_turn_cloud_attached = false;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                    }
                    taskEXIT_CRITICAL(&s_infer_mux);
                }
            }
            continue;
        }

        // Nếu câu nói ngắn hơn khung 1.2s (19200 mẫu), dùng phần đã zero-pad sẵn trong s_cmd_infer_snapshot để Model 2 luôn chạy được
        size_t samples_available = (raw_samples < EI_COMMAND_WINDOW_SAMPLES) ? EI_COMMAND_WINDOW_SAMPLES : raw_samples;

        // Căn tâm câu nói bằng Trọng tâm năng lượng (Energy-Weighted Centroid) trên biên âm tiết mềm
        int c_first = (b_first_chunk != -1 && b_last_chunk >= b_first_chunk) ? b_first_chunk : first_speech_chunk;
        int c_last = (b_first_chunk != -1 && b_last_chunk >= b_first_chunk) ? b_last_chunk : last_speech_chunk;
        int speech_center_sample = 4800 + (EI_COMMAND_WINDOW_SAMPLES / 2);
        if (c_first != -1 && c_last >= c_first) {
            int64_t weighted_sum = 0;
            int64_t total_weight = 0;
            for (int k = c_first; k <= c_last; k++) {
                if (chunk_rms[k] >= boundary_thresh) {
                    int w = chunk_rms[k] - boundary_thresh;
                    if (w > 0) {
                        int chunk_center = k * STREAM_CHUNK_SAMPLES + (STREAM_CHUNK_SAMPLES / 2);
                        weighted_sum += (int64_t)w * (int64_t)chunk_center;
                        total_weight += (int64_t)w;
                    }
                }
            }
            if (total_weight > 0) {
                speech_center_sample = (int)(weighted_sum / total_weight);
            } else {
                int speech_start_sample = c_first * STREAM_CHUNK_SAMPLES;
                int speech_end_sample = (c_last + 1) * STREAM_CHUNK_SAMPLES;
                speech_center_sample = (speech_start_sample + speech_end_sample) / 2;
            }
        }

        const int MAX_OFFSET = (samples_available >= EI_COMMAND_WINDOW_SAMPLES) ? (samples_available - EI_COMMAND_WINDOW_SAMPLES) : 0;
        int desired_offset = speech_center_sample - (EI_COMMAND_WINDOW_SAMPLES / 2);

        bool use_left_pad = (desired_offset < 0) && (s_cmd_infer_centered_buf != NULL);
        const int16_t *slice_ptrs[2] = { NULL, NULL };
        int slice_offsets[2] = { 0, 0 };
        const char *slice_names[2];

        if (use_left_pad) {
            int left_pad = -desired_offset;
            if (left_pad > 3200) left_pad = 3200; // Giới hạn tối đa 200ms (3200 mẫu)
            size_t copy_samples = EI_COMMAND_WINDOW_SAMPLES - (size_t)left_pad;
            if (copy_samples > samples_available) copy_samples = samples_available;

            // Lấp vùng left_pad bằng đoạn nhiễu phòng thật im lặng nhất (RMS < 60) thay vì 0 tuyệt đối (-inf dB làm méo MFCC)
            int quietest_k = -1;
            int min_q_rms = 60;
            for (int k = 0; k < num_chunks; k++) {
                if (chunk_rms[k] < min_q_rms) {
                    min_q_rms = chunk_rms[k];
                    quietest_k = k;
                }
            }
            if (quietest_k != -1) {
                const int16_t *bg_chunk = &s_cmd_infer_snapshot[quietest_k * STREAM_CHUNK_SAMPLES];
                int filled = 0;
                while (filled < left_pad) {
                    int to_copy = left_pad - filled;
                    if (to_copy > STREAM_CHUNK_SAMPLES) to_copy = STREAM_CHUNK_SAMPLES;
                    memcpy(&s_cmd_infer_centered_buf[filled], bg_chunk, (size_t)to_copy * sizeof(int16_t));
                    filled += to_copy;
                }
            } else {
                memset(s_cmd_infer_centered_buf, 0, (size_t)left_pad * sizeof(int16_t));
            }
            memcpy(&s_cmd_infer_centered_buf[left_pad], s_cmd_infer_snapshot, copy_samples * sizeof(int16_t));

            slice_ptrs[0] = s_cmd_infer_centered_buf;
            slice_offsets[0] = -left_pad;
            slice_names[0] = "1/2 - True Centered (Left-Pad)";

            slice_ptrs[1] = &s_cmd_infer_snapshot[0];
            slice_offsets[1] = 0;
            slice_names[1] = "2/2 - Natural Start";
        } else {
            int slice1_offset = (desired_offset < 0) ? 0 : desired_offset;
            if (slice1_offset > MAX_OFFSET) slice1_offset = MAX_OFFSET;

            int slice2_offset = (slice1_offset <= (MAX_OFFSET / 2)) ? (slice1_offset + 2000) : (slice1_offset - 2000);
            if (slice2_offset < 0) slice2_offset = 0;
            if (slice2_offset > MAX_OFFSET) slice2_offset = MAX_OFFSET;

            slice_ptrs[0] = &s_cmd_infer_snapshot[slice1_offset];
            slice_offsets[0] = slice1_offset;
            slice_names[0] = "1/2 - Centroid";

            slice_ptrs[1] = (slice2_offset != slice1_offset && (size_t)(slice2_offset + EI_COMMAND_WINDOW_SAMPLES) <= samples_available)
                            ? &s_cmd_infer_snapshot[slice2_offset] : NULL;
            slice_offsets[1] = slice2_offset;
            slice_names[1] = "2/2 - Adaptive Shift";
        }

        const char *best_valid_lbl = NULL;
        float best_valid_conf = 0.0f;
        const char *best_valid_slice_name = NULL;
        float noise_conf = 0.0f;
        float last_conf = 0.0f;
        const char *last_lbl = "unknown";
        int64_t infer_start_us = esp_timer_get_time();

        for (int s = 0; s < 2; s++) {
            taskENTER_CRITICAL(&s_infer_mux);
            bool aborted_mid_loop = (local_turn_id != s_edge_turn_id || s_edge_state == AI_EDGE_STATE_IDLE_WAKEWORD);
            taskEXIT_CRITICAL(&s_infer_mux);
            if (aborted_mid_loop) {
                ESP_LOGW(TAG, ">>> [MODEL 2 ABORTED] Turn ID changed (%lu != %lu) or IDLE -> Bỏ qua kết quả! <<<",
                         (unsigned long)local_turn_id, (unsigned long)s_edge_turn_id);
                best_valid_lbl = NULL;
                noise_conf = 0.0f;
                break;
            }
            if (!slice_ptrs[s]) {
                continue;
            }
            if (s == 1) {
                // Nhường CPU 10ms giữa lát 1 và lát 2 để nuôi Watchdog IDLE1
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            float conf = 0.0f;
            const char *lbl = NULL;

            ESP_LOGI(TAG, ">>> [MODEL 2 ASYNC - SLICE %s] Offset: %d samples <<<",
                     slice_names[s], slice_offsets[s]);

            bool match = ei_command_classify(slice_ptrs[s], EI_COMMAND_WINDOW_SAMPLES, &conf, &lbl);
            last_conf = conf;
            if (lbl) last_lbl = lbl;

            if (match && is_command_device_enabled(lbl)) {
                bool is_two_syllable_cmd = (lbl && (strcmp(lbl, "bat_den") == 0 || strcmp(lbl, "tat_den") == 0));
                if (is_two_syllable_cmd && speech_span_chunks >= 16 && conf < 0.86f) {
                    ESP_LOGW(TAG, ">>> [SPAN MISMATCH] Câu nói dài %d chunks (>= 16) với điểm thấp '%s' (%.1f%% < 86%%) -> Bỏ qua! <<<",
                             speech_span_chunks, lbl, conf * 100.0f);
                    continue;
                }

                if (conf > best_valid_conf) {
                    best_valid_lbl = lbl;
                    best_valid_conf = conf;
                    best_valid_slice_name = slice_names[s];
                }

                // Chỉ thoát sớm sau Lát 1 khi đạt độ tin cậy cao (>= 85%) để Lát 2 vẫn có cơ hội cứu lệnh lệch biên
                if (conf >= 0.85f) {
                    break;
                }
            } else if (match) {
                ESP_LOGI(TAG, ">>> [LOCAL IGNORE]: Khớp nhãn '%s' (%.1f%%) nhưng thiết bị đang tắt/chưa có -> Bỏ qua <<<",
                         lbl ? lbl : "none", conf * 100.0f);
            } else if (lbl && strcmp(lbl, "noise") == 0) {
                if (conf > noise_conf) {
                    noise_conf = conf;
                }
                if (s == 0 && conf >= 0.85f) {
                    break;
                }
            }
        }

        bool strong_human_speech = (total_user_chunks >= 6 && webrtc_speech_chunks >= 5 && max_speech_rms >= 180);

        if (best_valid_lbl) {
            if (!is_command_device_online(best_valid_lbl)) {
                ESP_LOGW(TAG, ">>> [LOCAL OFFLINE GUARD] Khớp lệnh '%s' (%.1f%%) nhưng thiết bị đang Offline -> Bỏ qua thực thi cục bộ (chuyển Gemini báo lỗi/âm lỗi)! <<<",
                         best_valid_lbl, best_valid_conf * 100.0f);
            } else {
                taskENTER_CRITICAL(&s_infer_mux);
                bool is_valid_match = (local_turn_id == s_edge_turn_id && s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
                if (is_valid_match) {
                    s_edge_turn_id++;
                    s_turn_cloud_attached = false;
                    s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                }
                s_cmd_infer_busy = false;
                taskEXIT_CRITICAL(&s_infer_mux);

                if (is_valid_match) {
                    uint32_t infer_ms = (uint32_t)((esp_timer_get_time() - infer_start_us) / 1000);
                    ESP_LOGW(TAG, "==================================================");
                    ESP_LOGW(TAG, ">>> [LOCAL COMMAND SUCCESS - SLICE %s (ASYNC)]: '%s' (Confidence: %.1f%%, %lums) <<<",
                             best_valid_slice_name, best_valid_lbl, best_valid_conf * 100.0f, (unsigned long)infer_ms);
                    ESP_LOGW(TAG, "==================================================");
                    dispatch_local_mesh_command(best_valid_lbl);
                    ui_display_notify_command_executed(best_valid_lbl, (int)infer_ms);
                    s_cloud_speaking_notified = false;
                    ai_cloud_cancel_turn();
                    ei_wakeword_reset();
                } else {
                    ESP_LOGW(TAG, ">>> [MODEL 2 STALE RESULT] Turn đã bị hủy trước khi khớp lệnh '%s' -> Bỏ qua! <<<", best_valid_lbl);
                }
                continue;
            }
        } else {
            bool insufficient_speech = (total_user_chunks < 4 || webrtc_speech_chunks < 3);
            bool top_is_noise = (last_lbl && strcmp(last_lbl, "noise") == 0 && last_conf >= 0.65f);
            bool model_noise_reject = (!strong_human_speech && (noise_conf >= 0.80f || top_is_noise));

            if (insufficient_speech || model_noise_reject) {
                taskENTER_CRITICAL(&s_infer_mux);
                bool is_valid_silent = (local_turn_id == s_edge_turn_id && s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
                if (is_valid_silent) {
                    s_edge_turn_id++;
                    s_turn_cloud_attached = false;
                    s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                }
                s_cmd_infer_busy = false;
                taskEXIT_CRITICAL(&s_infer_mux);

                if (is_valid_silent) {
                    ESP_LOGW(TAG, ">>> [%s] UserChunks:%d, WebRTC:%d, MaxRMS:%d, NoiseConf:%.1f%%, Top:'%s' (%.1f%%) -> Hủy êm! <<<",
                             insufficient_speech ? "SILENT CANCEL GATE" : "SMART NOISE GUARD",
                             total_user_chunks, webrtc_speech_chunks, max_speech_rms,
                             noise_conf * 100.0f, last_lbl ? last_lbl : "none", last_conf * 100.0f);
                    ui_display_hide_voice_overlay();
                    s_cloud_speaking_notified = false;
                    ai_cloud_cancel_turn();
                    ei_wakeword_reset();
                }
                continue;
            }
        }

        taskENTER_CRITICAL(&s_infer_mux);
        bool is_valid_turn = (local_turn_id == s_edge_turn_id && s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD);
        bool need_end_turn = (is_valid_turn && s_edge_state == AI_EDGE_STATE_AI_RESPONDING);
        s_cmd_infer_busy = false;
        taskEXIT_CRITICAL(&s_infer_mux);

        if (!is_valid_turn) {
            continue;
        }

        // Cả 2 lát cắt không khớp lệnh (hoặc thiết bị đang Offline) -> Commit lượt sang Google Live API!
        ESP_LOGI(TAG, ">>> [HYBRID ROUTING (ASYNC)] Model 2 không khớp / Offline ('%s': %.1f%%) -> Commit buffer sang Gemini Live! <<<",
                 last_lbl ? last_lbl : "none", last_conf * 100.0f);

        if (sys_core_is_wifi_connected()) {
            ai_cloud_commit_turn();
            if (need_end_turn) {
                ai_cloud_end_turn_manual();
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
                s_turn_cloud_attached = false;
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

    // 6. Buffer Pre-roll 480ms (8 chunks * 60ms = 3 chunks trigger + 300ms đệm trước âm tiết đầu)
    #define PREROLL_CHUNKS 8
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
    uint8_t s_preroll_webrtc[PREROLL_CHUNKS] = {0};
    uint8_t cmd_webrtc_flags[25] = {0};
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
    bool was_waiting_infer = false;

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
                taskENTER_CRITICAL(&s_infer_mux);
                bool infer_busy_idle = s_cmd_infer_busy;
                taskEXIT_CRITICAL(&s_infer_mux);
                if (infer_busy_idle) {
                    was_waiting_infer = true;
                    vTaskDelay(pdMS_TO_TICKS(10));
                    break;
                }
                if (was_waiting_infer) {
                    was_waiting_infer = false;
                    audio_hal_flush_rx();
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
                    s_turn_cloud_attached = false;
                    s_edge_turn_id++;
                    voice_endpointer_init(&s_endpointer);
                    preroll_count = 0;
                    preroll_idx = 0;
                    if (s_preroll_buffer) memset(s_preroll_buffer, 0, PREROLL_BUFFER_SIZE);
                    memset(s_preroll_webrtc, 0, sizeof(s_preroll_webrtc));
                    memset(cmd_webrtc_flags, 0, sizeof(cmd_webrtc_flags));
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

                    // Chốt chặn SNR Gate thích ứng theo nhiễu nền (chặn Wakeword giả lúc phòng trống):
                    if (detected) {
                        int min_snr_rms = ambient * 2 + 15;
                        if (min_snr_rms < 50) min_snr_rms = 50;
                        if (max_rms_2slices < min_snr_rms) {
                            ESP_LOGW(TAG, ">>> [SNR GATE BLOCKED] Bỏ qua kích hoạt giả (Score: %.1f%%, RMS: %d, Max2Slices: %d < MinSNR: %d) <<<",
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
                        s_turn_cloud_attached = false;
                        s_edge_turn_id++;
                        voice_endpointer_init(&s_endpointer);
                        preroll_count = 0;
                        preroll_idx = 0;
                        if (s_preroll_buffer) {
                            memset(s_preroll_buffer, 0, PREROLL_BUFFER_SIZE);
                        }
                        memset(s_preroll_webrtc, 0, sizeof(s_preroll_webrtc));
                        memset(cmd_webrtc_flags, 0, sizeof(cmd_webrtc_flags));
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
                        if (sys_core_is_wifi_connected()) {
                            ESP_LOGI(TAG, ">>> MANUAL FINISH REQUESTED (Touch Button): CHỐT CÂU LỆNH NGAY LẬP TỨC <<<");
                            taskENTER_CRITICAL(&s_infer_mux);
                            s_edge_turn_id++;
                            s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                            taskEXIT_CRITICAL(&s_infer_mux);
                            ei_wakeword_reset();
                            ai_cloud_commit_turn();
                            ai_cloud_end_turn_manual();
                            s_responding_silent_cycles = 0;
                            responding_log_counter = 0;
                        } else {
                            ESP_LOGW(TAG, ">>> MANUAL FINISH REQUESTED nhưng đang mất Wi-Fi -> Phát âm lỗi & về IDLE! <<<");
                            taskENTER_CRITICAL(&s_infer_mux);
                            s_edge_turn_id++;
                            s_turn_cloud_attached = false;
                            s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                            taskEXIT_CRITICAL(&s_infer_mux);
                            audio_hal_play_error_sound();
                            ui_display_hide_voice_overlay();
                            s_cloud_speaking_notified = false;
                            ai_cloud_stop_session();
                            ei_wakeword_reset();
                        }
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
                        memset(s_preroll_webrtc, 0, sizeof(s_preroll_webrtc));
                        memset(cmd_webrtc_flags, 0, sizeof(cmd_webrtc_flags));
                        ESP_LOGI(TAG, ">>> [RECORDING OPEN] Chuông Ding đã dứt -> Bắt đầu thu âm câu lệnh! <<<");
                        break;
                    }

                    int rms = 0;
                    float voice_thresh = 0.0f;
                    bool chunk_webrtc = false;
                    bool has_voice = voice_endpointer_feed(&s_endpointer, stream_chunk_buffer, STREAM_CHUNK_SAMPLES,
                                                           command_speech_started, &rms, &voice_thresh, &chunk_webrtc);

                    command_total_chunks++;

                    // 2. Quản lý Pre-roll và Kích hoạt Speech Start:
                    if (!command_speech_started) {
                        // Tuyệt đối không đếm im lặng trước khi người dùng mở miệng nói!
                        s_endpointer.silence_counter = 0;

                        if (s_preroll_buffer) {
                            memcpy(s_preroll_buffer[preroll_idx], stream_chunk_buffer, sizeof(int16_t) * STREAM_CHUNK_SAMPLES);
                            s_preroll_webrtc[preroll_idx] = chunk_webrtc ? 1 : 0;
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
                            s_endpointer.speech_rms_ema = fmaxf((float)rms, 80.0f);
                            ESP_LOGI(TAG, ">>> [VOICE START] Bắt đầu nói (RMS: %d, Thresh: %d, Sàn ồn: %d) -> Lùi %dms giữ trọn phụ âm đầu! <<<",
                                     rms, (int)voice_thresh, (int)s_endpointer.noise_floor_ema, preroll_count * 60);

                            // Kích hoạt Stream song song lên Cloud (Buffered mode)
                            if (sys_core_is_wifi_connected()) {
                                s_turn_cloud_attached = true;
                                ai_cloud_start_turn();
                            }

                            // Nạp các chunk Pre-roll vào đầu cmd_audio_buffer VÀ stream ngay lên Cloud
                            cmd_samples_from_speech = 0;
                            if (s_preroll_buffer) {
                                int start_idx = (preroll_idx - preroll_count + PREROLL_CHUNKS) % PREROLL_CHUNKS;
                                for (int p = 0; p < preroll_count; p++) {
                                    int cur_p = (start_idx + p) % PREROLL_CHUNKS;
                                    if (cmd_audio_buffer && (cmd_samples_from_speech + STREAM_CHUNK_SAMPLES <= EI_COMMAND_BUFFER_SAMPLES)) {
                                        size_t chunk_slot = cmd_samples_from_speech / STREAM_CHUNK_SAMPLES;
                                        if (chunk_slot < 25) {
                                            cmd_webrtc_flags[chunk_slot] = s_preroll_webrtc[cur_p];
                                        }
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
                                s_turn_cloud_attached = false;
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
                            size_t chunk_slot = cmd_samples_from_speech / STREAM_CHUNK_SAMPLES;
                            if (chunk_slot < 25) {
                                cmd_webrtc_flags[chunk_slot] = chunk_webrtc ? 1 : 0;
                            }
                            memcpy(&cmd_audio_buffer[cmd_samples_from_speech], stream_chunk_buffer, STREAM_CHUNK_SAMPLES * sizeof(int16_t));
                            cmd_samples_from_speech += STREAM_CHUNK_SAMPLES;
                        }
                    }

                    // 3. Đánh thức luồng suy luận Model 2 bất đồng bộ (Decoupled Task Trigger):
                    // Luôn thu đủ trọn vẹn 1.5s (EI_COMMAND_BUFFER_SAMPLES = 24000 mẫu) rồi mới chạy Model 2 và chuyển sang STREAMING_SPEECH
                    if (!cmd_inference_dispatched && command_speech_started &&
                        (cmd_samples_from_speech >= EI_COMMAND_BUFFER_SAMPLES)) {
                        cmd_inference_dispatched = true;

                        if (s_cmd_infer_snapshot && s_cmd_infer_task_handle) {
                            memset(s_cmd_infer_snapshot, 0, EI_COMMAND_BUFFER_SAMPLES * sizeof(int16_t));
                            memcpy(s_cmd_infer_snapshot, cmd_audio_buffer, cmd_samples_from_speech * sizeof(int16_t));
                            taskENTER_CRITICAL(&s_infer_mux);
                            memcpy(s_cmd_infer_webrtc_flags, cmd_webrtc_flags, sizeof(s_cmd_infer_webrtc_flags));
                            s_cmd_infer_sample_count = cmd_samples_from_speech;
                            s_cmd_infer_turn_id = s_edge_turn_id;
                            s_cmd_infer_noise_floor = (int)s_endpointer.noise_floor_ema;
                            s_cmd_infer_busy = true;
                            taskEXIT_CRITICAL(&s_infer_mux);
                            xTaskNotify(s_cmd_infer_task_handle, CMD_INFER_EVT_START, eSetBits);
                        }

                        s_edge_state = AI_EDGE_STATE_STREAMING_SPEECH;
                        streaming_counter = 0;
                        break;
                    }
                }
                break;
            }

            case AI_EDGE_STATE_STREAMING_SPEECH: {
                if (s_manual_finish_requested) {
                    s_manual_finish_requested = false;
                    if (sys_core_is_wifi_connected()) {
                        ESP_LOGI(TAG, ">>> MANUAL FINISH REQUESTED (Touch Button): CHỐT CÂU LỆNH NGAY LẬP TỨC <<<");
                        taskENTER_CRITICAL(&s_infer_mux);
                        s_edge_turn_id++;
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                        taskEXIT_CRITICAL(&s_infer_mux);
                        ei_wakeword_reset();
                        ai_cloud_commit_turn();
                        ai_cloud_end_turn_manual();
                        s_responding_silent_cycles = 0;
                        responding_log_counter = 0;
                    } else {
                        ESP_LOGW(TAG, ">>> MANUAL FINISH REQUESTED nhưng đang mất Wi-Fi -> Phát âm lỗi & về IDLE! <<<");
                        taskENTER_CRITICAL(&s_infer_mux);
                        s_edge_turn_id++;
                        s_turn_cloud_attached = false;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        taskEXIT_CRITICAL(&s_infer_mux);
                        audio_hal_play_error_sound();
                        ui_display_hide_voice_overlay();
                        s_cloud_speaking_notified = false;
                        ai_cloud_stop_session();
                        ei_wakeword_reset();
                    }
                    break;
                }
                prev_wake_rms = 0;
                size_t bytes_to_read = STREAM_CHUNK_SAMPLES * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(stream_chunk_buffer, bytes_to_read);

                // Race guard: nếu cmd_infer_task vừa khớp lệnh cục bộ & chuyển sang IDLE_WAKEWORD trong lúc đọc mic -> thoát ngay
                taskENTER_CRITICAL(&s_infer_mux);
                bool still_streaming = (s_edge_state == AI_EDGE_STATE_STREAMING_SPEECH);
                taskEXIT_CRITICAL(&s_infer_mux);
                if (!still_streaming) {
                    break;
                }

                if (bytes_read > 0) {
                    audio_hal_apply_dc_filter(stream_chunk_buffer, bytes_read / sizeof(int16_t));

                    int rms = 0;
                    float voice_thresh = 0.0f;
                    bool raw_webrtc = false;
                    bool has_voice = voice_endpointer_feed(&s_endpointer, stream_chunk_buffer, bytes_read / sizeof(int16_t),
                                                           true, &rms, &voice_thresh, &raw_webrtc);

                    ai_cloud_send_audio_chunk((uint8_t *)stream_chunk_buffer, bytes_read);

                    // Nguồn chốt câu chính: Server Neural VAD (Gemini Live) hoặc toolCall / inlineData đã kết thúc pha thu âm
                    taskENTER_CRITICAL(&s_infer_mux);
                    bool infer_busy_now = s_cmd_infer_busy;
                    taskEXIT_CRITICAL(&s_infer_mux);
                    if (sys_core_is_wifi_connected() && !infer_busy_now && !ai_cloud_is_accepting_audio()) {
                        ESP_LOGI(TAG, ">>> [SERVER VAD END] Gemini đã chốt dứt câu -> Dừng thu âm & chờ phát loa! <<<");
                        taskENTER_CRITICAL(&s_infer_mux);
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                        taskEXIT_CRITICAL(&s_infer_mux);
                        ei_wakeword_reset();
                        s_responding_silent_cycles = 0;
                        responding_log_counter = 0;
                        break;
                    }

                    // Chốt sạch câu ngắn trước khi bật Wi-Fi TX (khi Model 2 đang suy luận và ai_cloud vẫn BUFFERING, mic cực sạch)
                    if (infer_busy_now && s_endpointer.silence_counter >= PRE_COMMIT_CLEAN_SILENCE_CHUNKS) {
                        ESP_LOGI(TAG, ">>> [PRE-COMMIT CUTOFF] Đã im lặng %dms trước khi bật Wi-Fi TX -> Chốt câu & chờ Model 2! <<<",
                                 PRE_COMMIT_CLEAN_SILENCE_CHUNKS * 60);
                        taskENTER_CRITICAL(&s_infer_mux);
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING; // KHÔNG tăng s_edge_turn_id để cmd_infer_task vẫn hoàn tất hợp lệ
                        taskEXIT_CRITICAL(&s_infer_mux);
                        ei_wakeword_reset();
                        s_responding_silent_cycles = 0;
                        responding_log_counter = 0;
                        break;
                    }

                    streaming_counter++;
                    if (streaming_counter % 8 == 0) {
                        ESP_LOGI(TAG, "[STREAM VAD] RMS: %4d (Thresh: %3d, Floor: %2d, EMA: %3d, VAD:%d) | %s | Silence: %2d/%d",
                                 rms, (int)voice_thresh, (int)s_endpointer.noise_floor_ema,
                                 (int)s_endpointer.speech_rms_ema, raw_webrtc ? 1 : 0,
                                 has_voice ? "CÓ TIẾNG" : "Im lặng  ",
                                 s_endpointer.silence_counter, STREAM_SILENCE_TARGET);
                    }

                    // Cầu chì dự phòng (Client VAD Fallback: 30 chunks * 60ms = 1800ms im lặng tuyệt đối)
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

                taskENTER_CRITICAL(&s_infer_mux);
                bool still_responding = (s_edge_state == AI_EDGE_STATE_AI_RESPONDING);
                taskEXIT_CRITICAL(&s_infer_mux);
                if (!still_responding) {
                    break;
                }

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
                        taskENTER_CRITICAL(&s_infer_mux);
                        s_turn_cloud_attached = false;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        taskEXIT_CRITICAL(&s_infer_mux);
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
                    taskENTER_CRITICAL(&s_infer_mux);
                    s_turn_cloud_attached = false;
                    s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                    taskEXIT_CRITICAL(&s_infer_mux);
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

    // Cấp phát snapshot & centered left-pad buffer trong PSRAM cho Model 2 async inference
    s_cmd_infer_snapshot = (int16_t *)heap_caps_malloc(EI_COMMAND_BUFFER_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_cmd_infer_snapshot) {
        ESP_LOGE(TAG, "Failed to allocate s_cmd_infer_snapshot in PSRAM!");
    }
    s_cmd_infer_centered_buf = (int16_t *)heap_caps_malloc(EI_COMMAND_WINDOW_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_cmd_infer_centered_buf) {
        ESP_LOGE(TAG, "Failed to allocate s_cmd_infer_centered_buf in PSRAM!");
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
    // Chỉ hủy khi đang thực sự gắn với Cloud (tránh tín hiệu dọn dẹp trễ từ lượt cũ tắt nhầm lượt Wakeword mới)
    bool was_active = (s_edge_state == AI_EDGE_STATE_STREAMING_SPEECH ||
                       s_edge_state == AI_EDGE_STATE_AI_RESPONDING ||
                       (s_edge_state == AI_EDGE_STATE_COMMAND_LISTENING && s_turn_cloud_attached));
    if (was_active) {
        s_edge_turn_id++;
        s_turn_cloud_attached = false;
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
        s_turn_cloud_attached = false;
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
