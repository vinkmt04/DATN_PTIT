#include "ai_edge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "audio_hal.h"
#include "ai_cloud.h"
#include "sys_core.h"
#include "esp_heap_caps.h"
#include "ei_wrapper.h"
#include "mesh_controller.h"
#include <string.h>

static const char *TAG = "ai_edge";

#define BOOT_BUTTON_GPIO         0
#define STREAM_CHUNK_SAMPLES     960    // 60ms @ 16kHz
#define STREAM_SILENCE_TARGET    17     // 17 * 60ms = 1020ms ~ 1.0s im lặng để chốt lượt với Gemini Live

// =============================================================================
// [CHẾ ĐỘ KIỂM THỬ MODEL 2 CÔ LẬP]
// 1: Khi Model 2 khớp/không khớp, KHÔNG gọi Cloud, phát lại âm thanh vừa thu ra loa để test, rồi tự về IDLE (0% xung đột mạng)
// 0: Chế độ hoạt động chuẩn (Pre-warm WebSocket, Hybrid Cloud Fallback)
// =============================================================================
#define DEBUG_LOCAL_TEST_MODE    0

static TaskHandle_t audio_task_handle = NULL;
static TaskHandle_t button_task_handle = NULL;
static volatile ai_edge_state_t s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
static volatile int s_responding_silent_cycles = 0;

static bool is_command_device_enabled(const char *cmd_label) {
    if (!cmd_label) return false;
    if (strcmp(cmd_label, "bat_den") == 0 || strcmp(cmd_label, "tat_den") == 0) {
        return (GATEWAY_ENABLE_DIMMER == 1);
    }
    if (strcmp(cmd_label, "bat_cong_tac") == 0 || strcmp(cmd_label, "tat_cong_tac") == 0) {
        return (GATEWAY_ENABLE_RELAY == 1);
    }
    return false;
}

static void dispatch_local_mesh_command(const char *cmd_label) {
    if (!cmd_label) return;

    if (strcmp(cmd_label, "bat_den") == 0) {
#if GATEWAY_ENABLE_DIMMER
        ESP_LOGI(TAG, "Local Edge AI: BẬT ĐÈN (khôi phục mức sáng gần nhất)");
        mesh_controller_set_dimmer(true, 0);
#else
        ESP_LOGW(TAG, "Local Edge AI: Bỏ qua 'bat_den' do GATEWAY_ENABLE_DIMMER == 0");
#endif
    } else if (strcmp(cmd_label, "tat_den") == 0) {
#if GATEWAY_ENABLE_DIMMER
        ESP_LOGI(TAG, "Local Edge AI: TẮT ĐÈN");
        mesh_controller_set_dimmer(false, 0);
#else
        ESP_LOGW(TAG, "Local Edge AI: Bỏ qua 'tat_den' do GATEWAY_ENABLE_DIMMER == 0");
#endif
    } else if (strcmp(cmd_label, "bat_cong_tac") == 0) {
#if GATEWAY_ENABLE_RELAY
        ESP_LOGI(TAG, "Local Edge AI: BẬT CÔNG TẮC RELAY");
        mesh_controller_set_relay(true);
#else
        ESP_LOGW(TAG, "Local Edge AI: Bỏ qua 'bat_cong_tac' do GATEWAY_ENABLE_RELAY == 0");
#endif
    } else if (strcmp(cmd_label, "tat_cong_tac") == 0) {
#if GATEWAY_ENABLE_RELAY
        ESP_LOGI(TAG, "Local Edge AI: TẮT CÔNG TẮC RELAY");
        mesh_controller_set_relay(false);
#else
        ESP_LOGW(TAG, "Local Edge AI: Bỏ qua 'tat_cong_tac' do GATEWAY_ENABLE_RELAY == 0");
#endif
    } else {
        ESP_LOGW(TAG, "Bỏ qua nhãn không thuộc hệ thống (như quạt hoặc unknown): %s", cmd_label);
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

    int cmd_inference_count = 0;
    size_t cmd_samples_from_speech = 0;
    bool command_speech_started = false;
    bool wake_dma_flushed = false;
    int command_speech_chunks = 0;
    int command_silence_chunks = 0;
    int command_total_chunks = 0;

    int streaming_counter = 0;
    int silence_counter = 0;
    int speech_chunks_total = 0;
    int s_consecutive_speech = 0;
    int idle_slice_counter = 0;
    int responding_log_counter = 0;
    int prev_wake_rms = 0;

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " HỆ THỐNG LẮNG NGHE ĐÃ SẴN SÀNG (HYBRID DUAL-MODEL)!");
    ESP_LOGI(TAG, " - Wake Word: 'Hey Cherry' (Continuous 250ms)");
    ESP_LOGI(TAG, " - Command Control: 6 Smart Home Labels (1.5s Buffer, 3-Slice Energy-Centered Scan)");
    ESP_LOGI(TAG, " - Cloud AI: Google Gemini Live API Streaming");
    ESP_LOGI(TAG, "==================================================");

    while (1) {
        switch (s_edge_state) {
            case AI_EDGE_STATE_IDLE_WAKEWORD: {
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
                        ESP_LOGI(TAG, "[IDLE_WAKEWORD] Đang lắng nghe 'Hey Cherry'... (Mic RMS: %d, Sàn ồn: %d)",
                                 rms, ambient);
                    }

                    // Chạy nhận diện Wake Word
                    bool detected = ei_wakeword_process_slice(wake_slice_buffer, wake_slice_size, &confidence, &label);

                    // Chỉ gọi audio_dsp_update_ambient_noise(rms) khi lát cắt KHÔNG phải là tiếng nói
                    // (confidence < 0.30f và rms < ambient * 2), tránh việc người dùng đang nói làm tăng vọt chỉ số sàn ồn
                    if (confidence < 0.30f && rms < ambient * 2) {
                        audio_dsp_update_ambient_noise(rms);
                    }

                    // Chốt chặn SNR Gate thích ứng theo nhiễu nền:
                    // Yêu cầu max_rms_2slices >= 40 VÀ max_rms_2slices >= (int)(ambient * 1.3f) (tăng 30% so với sàn ồn)
                    if (detected) {
                        int min_snr_rms = (int)(ambient * 1.3f);
                        if (max_rms_2slices < 40 || max_rms_2slices < min_snr_rms) {
                            ESP_LOGW(TAG, ">>> [SNR GATE BLOCKED] Bỏ qua kích hoạt giả (Score: %.1f%%, RMS: %d, Max2Slices: %d < 40 hoặc < Sàn ồn x1.3: %d) <<<",
                                     confidence * 100.0f, rms, max_rms_2slices, min_snr_rms);
                            detected = false;
                        }
                    }

                    if (detected) {
                        prev_wake_rms = 0;
                        ESP_LOGI(TAG, "**************************************************");
                        ESP_LOGI(TAG, ">>> PHÁT HIỆN TỪ KHÓA 'HEY CHERRY'! (Score: %.1f%%, RMS: %d, Max2Slices: %d) <<<", confidence * 100.0f, rms, max_rms_2slices);
                        ESP_LOGI(TAG, "**************************************************");

                        // 1. Kích hoạt kết nối WebSocket ngầm (Pre-warm) nếu có Wi-Fi (Tắt khi test cô lập để tối ưu 100% tài nguyên)
#if !DEBUG_LOCAL_TEST_MODE
                        if (sys_core_is_wifi_connected()) {
                            ai_cloud_prewarm_session();
                        }
#endif

                        // 2. Phát âm thanh báo thức (Ding-ding trọn vẹn ~450ms)
                        audio_hal_play_wake_sound();

                        // 3. Reset các bộ đệm và biến trạng thái câu lệnh
                        cmd_inference_count = 0;
                        cmd_samples_from_speech = 0;
                        command_speech_started = false;
                        wake_dma_flushed = false;
                        command_speech_chunks = 0;
                        command_silence_chunks = 0;
                        command_total_chunks = 0;
                        preroll_count = 0;
                        preroll_idx = 0;
                        if (s_preroll_buffer) {
                            memset(s_preroll_buffer, 0, PREROLL_BUFFER_SIZE);
                        }
                        if (cmd_audio_buffer) {
                            memset(cmd_audio_buffer, 0, EI_COMMAND_BUFFER_SAMPLES * sizeof(int16_t));
                        }
                        ei_command_reset_window();

                        // 4. Chuyển sang lắng nghe câu lệnh (thu âm mở trước khi Ding kết thúc 100ms)
                        s_edge_state = AI_EDGE_STATE_COMMAND_LISTENING;
                    } else {
                        prev_wake_rms = rms;
                    }
                } else {
                    ESP_LOGW(TAG, "Mic read mismatch: %u != %u", (unsigned int)bytes_read, (unsigned int)bytes_to_read);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                vTaskDelay(pdMS_TO_TICKS(2)); // Nhường CPU cho IDLE1 reset Watchdog
                break;
            }

            case AI_EDGE_STATE_COMMAND_LISTENING: {
                prev_wake_rms = 0;
                // Đọc từng chunk 60ms (960 mẫu = 1920 bytes) để VAD chính xác từng mili-giây
                size_t bytes_to_read = STREAM_CHUNK_SAMPLES * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(stream_chunk_buffer, bytes_to_read);

                if (bytes_read == bytes_to_read) {
                    audio_hal_apply_dc_filter(stream_chunk_buffer, STREAM_CHUNK_SAMPLES);

                    int rms = audio_vad_get_rms(stream_chunk_buffer, STREAM_CHUNK_SAMPLES);
                    int floor = audio_dsp_get_ambient_noise();

                    // 1. Quản lý thời điểm bắt đầu thu âm:
                    // Mở thu âm sau khi chuông Ding kết thúc 100% (+ 50ms tiêu âm phòng)
                    if (!audio_hal_is_wake_recording_ready()) {
                        break; // Đang trong thời gian phát chuông Ding: đọc xả DMA để mic không dính tiếng chuông
                    }

                    // Ngay khi chuông Ding vừa dứt: xả sạch DMA RX và reset bộ lọc DC để bảo đảm dữ liệu tươi mới
                    if (!wake_dma_flushed) {
                        wake_dma_flushed = true;
                        audio_hal_flush_rx();
                        audio_hal_reset_dc_filter();
                        audio_vad_reset_trigger();
                        preroll_count = 0;
                        preroll_idx = 0;
                        if (s_preroll_buffer) {
                            memset(s_preroll_buffer, 0, PREROLL_BUFFER_SIZE);
                        }
                        ESP_LOGI(TAG, ">>> [RECORDING OPEN] Chuông Ding đã dứt (+30ms tiêu âm phòng, xả DMA RX & DC Filter) -> Bắt đầu thu âm câu lệnh! <<<");
                        break;
                    }

                    // 2. VAD phát hiện giọng nói người (Phương án A: WebRTC VAD Trigger Mode 3 + Sàn ồn)
                    bool has_voice = audio_vad_is_speech_chunk(stream_chunk_buffer, STREAM_CHUNK_SAMPLES, NULL);


                    command_total_chunks++;

                    // 4. Quản lý Pre-roll và Kích hoạt Speech Start:
                    if (!command_speech_started) {
                        // Lưu chunk vào preroll buffer xoay vòng
                        if (s_preroll_buffer) {
                            memcpy(s_preroll_buffer[preroll_idx], stream_chunk_buffer, sizeof(int16_t) * STREAM_CHUNK_SAMPLES);
                            preroll_idx = (preroll_idx + 1) % PREROLL_CHUNKS;
                            if (preroll_count < PREROLL_CHUNKS) preroll_count++;
                        }

                        if (has_voice) {
                            command_speech_started = true;
                            command_speech_chunks = 1;
                            command_silence_chunks = 0;
                            ESP_LOGI(TAG, ">>> [VOICE START] Bắt đầu nói (RMS: %d, Sàn ồn: %d) -> Lùi %dms giữ trọn phụ âm đầu! <<<",
                                     rms, floor, preroll_count * 60);

                            // Kích hoạt Stream song song lên Cloud ngay lập tức (Speculative Streaming)
                            if (sys_core_is_wifi_connected()) {
                                ai_cloud_start_turn();
                            }

                            // Nạp các chunk Pre-roll (lùi tối đa 180ms) vào đầu cmd_audio_buffer VÀ stream ngay lên Cloud
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
                        } else {
                            audio_dsp_update_ambient_noise(rms);
                            // Quá 5.0s không ai nói gì: No-Speech Guard (84 chunks * 60ms = 5.04s)
                            if (command_total_chunks >= 84) {
                                ESP_LOGI(TAG, ">>> [NO-SPEECH GUARD] Không phát hiện tiếng nói sau 5.0s -> Hủy lượt & tiếp tục lắng nghe. <<<");
                                ai_cloud_stop_session();
                                ei_wakeword_reset();
                                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                                break;
                            }
                            break;
                        }
                    } else {
                        // Đã bắt đầu nói: Stream song song từng chunk 60ms lên Cloud theo thời gian thực (Zero Lag)
                        if (sys_core_is_wifi_connected()) {
                            ai_cloud_send_audio_chunk((const uint8_t *)stream_chunk_buffer, bytes_read);
                        }

                        if (has_voice) {
                            command_speech_chunks++;
                            command_silence_chunks = 0;
                        } else {
                            command_silence_chunks++;
                        }

                        // Tiếp tục gom âm thanh vào cmd_audio_buffer cho Model 2 (TUYỆT ĐỐI KHÔNG SUY LUẬN TẠI ĐÂY)
                        if (cmd_audio_buffer && (cmd_samples_from_speech + STREAM_CHUNK_SAMPLES <= EI_COMMAND_BUFFER_SAMPLES)) {
                            memcpy(&cmd_audio_buffer[cmd_samples_from_speech], stream_chunk_buffer, STREAM_CHUNK_SAMPLES * sizeof(int16_t));
                            cmd_samples_from_speech += STREAM_CHUNK_SAMPLES;
                        }
                    }

                    // 5. CĂN TÂM NĂNG LƯỢNG (ENERGY CENTER-ALIGNMENT) & PHÂN BIỆT CÂU PHỨC VS LỆNH ĐƠN (RELATIVE ENVELOPE DROP)
                    if (cmd_inference_count == 0 && cmd_samples_from_speech >= EI_COMMAND_BUFFER_SAMPLES) {
                        cmd_inference_count = 1;

                        // Bước 1: Tính mảng chunk_rms[25] cho từng đoạn 60ms (960 mẫu) trong cmd_audio_buffer (24.000 mẫu = 1.5s)
                        int chunk_rms[25];
                        int max_speech_rms = 0;
                        for (int k = 0; k < 25; k++) {
                            chunk_rms[k] = audio_vad_get_rms(&cmd_audio_buffer[k * STREAM_CHUNK_SAMPLES], STREAM_CHUNK_SAMPLES);
                            if (chunk_rms[k] > max_speech_rms) {
                                max_speech_rms = chunk_rms[k];
                            }
                        }

                        // Bước 2: Xác định ngưỡng năng lượng giọng nói thực sự của người dùng (tách biệt khỏi tiếng ồn phòng)
                        int ambient_noise = audio_dsp_get_ambient_noise();
                        int min_ambient_gate = ambient_noise * 2 + 20;
                        int rel_peak_gate = (int)(max_speech_rms * 0.32f);
                        int user_voice_thresh = (min_ambient_gate > rel_peak_gate) ? min_ambient_gate : rel_peak_gate;

                        // Bước 3: Đếm tổng chunk giọng nói, chunk đuôi (300ms cuối: 20..24), và tính độ trải dài câu nói (speech_span_chunks)
                        int first_speech_chunk = -1;
                        int last_speech_chunk = -1;
                        int total_user_chunks = 0;
                        int tail_active_chunks = 0;

                        for (int k = 0; k < 25; k++) {
                            if (chunk_rms[k] >= user_voice_thresh) {
                                if (first_speech_chunk == -1) first_speech_chunk = k;
                                last_speech_chunk = k;
                                total_user_chunks++;
                                if (k >= 20) {
                                    tail_active_chunks++;
                                }
                            }
                        }

                        int speech_span_chunks = (first_speech_chunk != -1) ? (last_speech_chunk - first_speech_chunk + 1) : 0;

                        // Bước 4: Kiểm tra điều kiện Câu phức / Câu dài (Chuyển thẳng sang Gemini Live, KHÔNG chạy Model 2):
                        // A1: Người dùng vẫn đang phát âm ở cuối khung 1.5s ((tail >= 2 && total >= 8) || mép cuối chunk 23 hoặc 24 có tiếng)
                        bool cond_a1 = (tail_active_chunks >= 2 && total_user_chunks >= 8) ||
                                       (chunk_rms[23] >= user_voice_thresh || chunk_rms[24] >= user_voice_thresh);
                        // A2: Độ trải dài của câu nói vượt quá độ dài lệnh 2-3 âm tiết (>= 17 chunks ~ 1.02s)
                        bool cond_a2 = (speech_span_chunks >= 17);
                        // A3: Tổng số chunk năng lượng cao quá nhiều (>= 15 chunks)
                        bool cond_a3 = (total_user_chunks >= 15);

                        if (cond_a1 || cond_a2 || cond_a3) {
                            ESP_LOGI(TAG, ">>> [COMPLEX SENTENCE DETECTED] Phát hiện câu nói dài/tiếp diễn (TailActive: %d/5, Span: %d chunks, TotalActive: %d, MaxRMS: %d, Thresh: %d) -> Bỏ qua Model 2, chuyển thẳng sang Gemini Live! <<<",
                                     tail_active_chunks, speech_span_chunks, total_user_chunks, max_speech_rms, user_voice_thresh);

                            if (sys_core_is_wifi_connected()) {
                                s_edge_state = AI_EDGE_STATE_STREAMING_SPEECH;
                                streaming_counter = 0;
                                silence_counter = command_silence_chunks;
                                s_consecutive_speech = 2;
                                speech_chunks_total = command_speech_chunks;
                            } else {
                                ESP_LOGE(TAG, ">>> [OFFLINE FALLBACK] Ngoại tuyến hoặc lỗi kết nối Live API! <<<");
                                audio_hal_play_error_sound();
                                ai_cloud_stop_session();
                                ei_wakeword_reset();
                                idle_slice_counter = 0;
                                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                            }
                            break;
                        }

                        // Bước 5: Căn tâm câu nói (Auto-Center Window) vào giữa khung 19.200 mẫu (1.2s)
                        int speech_center_sample = 12000; // Mặc định ở chính giữa buffer 1.5s
                        if (first_speech_chunk != -1 && last_speech_chunk >= first_speech_chunk) {
                            int speech_start_sample = first_speech_chunk * STREAM_CHUNK_SAMPLES;
                            int speech_end_sample = (last_speech_chunk + 1) * STREAM_CHUNK_SAMPLES;
                            speech_center_sample = (speech_start_sample + speech_end_sample) / 2;
                            ESP_LOGI(TAG, ">>> [ENERGY CENTER] Lệnh ngắn dứt câu (Tail: %d/5, Span: %d chunks, Total: %d). Phát hiện từ sample %d -> %d (%dms -> %dms, tâm: %dms, Thresh: %d, MaxRMS: %d) <<<",
                                     tail_active_chunks, speech_span_chunks, total_user_chunks,
                                     speech_start_sample, speech_end_sample,
                                     speech_start_sample / 16, speech_end_sample / 16,
                                     speech_center_sample / 16, user_voice_thresh, max_speech_rms);
                        } else {
                            // Fallback: Ước lượng câu nói bắt đầu sau Pre-roll 300ms (sample 4800)
                            speech_center_sample = 4800 + (EI_COMMAND_WINDOW_SAMPLES / 2); // 4800 + 9600 = 14400 (900ms)
                            ESP_LOGI(TAG, ">>> [ENERGY CENTER] Không xác định rõ tâm năng lượng -> Dùng tâm dự phòng %dms <<<",
                                     speech_center_sample / 16);
                        }

                        // Căn tâm: Khung 19,200 mẫu (1.2s) có tâm là (best_offset + 9600) -> best_offset = speech_center_sample - 9600
                        int best_offset = speech_center_sample - (EI_COMMAND_WINDOW_SAMPLES / 2);
                        const int MAX_OFFSET = EI_COMMAND_BUFFER_SAMPLES - EI_COMMAND_WINDOW_SAMPLES; // 24000 - 19200 = 4800 (300ms)
                        if (best_offset < 0) best_offset = 0;
                        if (best_offset > MAX_OFFSET) best_offset = MAX_OFFSET;

                        // Tính toán 3 lát cắt mịn trên RAM:
                        // Lát 1: Centered Slice (căn tâm năng lượng chuẩn xác nhất theo thuật toán Edge Impulse Split)
                        int slice1_offset = best_offset;

                        // Lát 2: Dịch lùi -1600 mẫu (-100ms) quanh tâm để đón câu nói sớm
                        int slice2_offset = best_offset - 1600;
                        if (slice2_offset < 0) {
                            slice2_offset = (best_offset + 1600 <= MAX_OFFSET) ? (best_offset + 1600) : 0;
                        }

                        // Lát 3: Dịch tiến +1600 mẫu (+100ms) hoặc offset 3600 để bắt trọn câu nói dài
                        int slice3_offset = best_offset + 1600;
                        if (slice3_offset > MAX_OFFSET) {
                            slice3_offset = (slice2_offset != 0) ? 0 : MAX_OFFSET;
                        }
                        if (slice3_offset == slice1_offset || slice3_offset == slice2_offset) {
                            if (slice1_offset != MAX_OFFSET && slice2_offset != MAX_OFFSET) slice3_offset = MAX_OFFSET;
                            else if (slice1_offset != 0 && slice2_offset != 0) slice3_offset = 0;
                            else slice3_offset = 2400;
                        }

                        int slice_offsets[3] = { slice1_offset, slice2_offset, slice3_offset };
                        const char *slice_names[3] = { "1/3 - Centered", "2/3 - Left Shift", "3/3 - Right Shift" };

                        bool command_matched = false;
                        float last_conf = 0.0f;
                        const char *last_lbl = "unknown";

                        for (int s = 0; s < 3; s++) {
                            int cur_offset = slice_offsets[s];
                            float conf = 0.0f;
                            const char *lbl = NULL;

                            ESP_LOGI(TAG, ">>> [MODEL 2 - SLICE %s] Offset: %d samples (%dms -> %dms) <<<",
                                     slice_names[s], cur_offset, cur_offset / 16, (cur_offset + EI_COMMAND_WINDOW_SAMPLES) / 16);

                            bool match = ei_command_classify(&cmd_audio_buffer[cur_offset], EI_COMMAND_WINDOW_SAMPLES, &conf, &lbl);
                            last_conf = conf;
                            if (lbl) last_lbl = lbl;

                            if (match && is_command_device_enabled(lbl)) {
                                // Syllable Span Guard: Kiểm tra độ khớp độ dài âm tiết đối với 2 lệnh 2 âm tiết ('bat_den', 'tat_den')
                                bool is_two_syllable_cmd = (lbl && (strcmp(lbl, "bat_den") == 0 || strcmp(lbl, "tat_den") == 0));
                                if (is_two_syllable_cmd && speech_span_chunks >= 13 && conf < 0.86f) {
                                    ESP_LOGW(TAG, ">>> [SPAN MISMATCH] Câu nói dài %d chunks nhưng khớp lệnh 2 âm tiết '%s' với điểm thấp (%.1f%% < 86%%) -> Bỏ qua lát cắt này! <<<",
                                             speech_span_chunks, lbl, conf * 100.0f);
                                    continue; // Tiếp tục kiểm tra lát cắt tiếp theo (ví dụ Right Shift có thể khớp 'bat_cong_tac')
                                }

                                ESP_LOGW(TAG, "==================================================");
                                ESP_LOGW(TAG, ">>> [LOCAL COMMAND SUCCESS - SLICE %s (EARLY EXIT)]: '%s' (Confidence: %.1f%%) <<<",
                                         slice_names[s], lbl, conf * 100.0f);
                                ESP_LOGW(TAG, "==================================================");
                                dispatch_local_mesh_command(lbl);
#if DEBUG_LOCAL_TEST_MODE
                                ESP_LOGW(TAG, ">>> [LOCAL TEST] Khớp lệnh '%s'! Phát lại âm thanh vừa thu ra loa <<<", lbl);
                                audio_hal_debug_play_captured_audio(cmd_audio_buffer, cmd_samples_from_speech);
                                vTaskDelay(pdMS_TO_TICKS(cmd_samples_from_speech * 1000 / 16000 + 200));
                                audio_hal_flush_rx();
#endif
                                ai_cloud_cancel_turn();
                                ei_wakeword_reset();
                                idle_slice_counter = 0;
                                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                                command_matched = true;
                                break;
                            } else if (match) {
                                ESP_LOGI(TAG, ">>> [LOCAL IGNORE]: Khớp nhãn '%s' (%.1f%%) nhưng thiết bị đang tắt/chưa có (ENABLE == 0) -> Bỏ qua lệnh cục bộ <<<",
                                         lbl ? lbl : "none", conf * 100.0f);
                            }
                        }

                        if (command_matched) {
                            break;
                        }

                        // Cả 3 lát cắt đều không khớp lệnh:
#if DEBUG_LOCAL_TEST_MODE
                        ESP_LOGW(TAG, "==================================================");
                        ESP_LOGW(TAG, ">>> [LOCAL TEST] Cả 3 lát cắt đều không khớp ('%s': %.1f%%) -> Phát lại trọn vẹn 1.5s ra loa <<<",
                                 last_lbl ? last_lbl : "none", last_conf * 100.0f);
                        ESP_LOGW(TAG, "==================================================");
                        audio_hal_debug_play_captured_audio(cmd_audio_buffer, cmd_samples_from_speech);
                        vTaskDelay(pdMS_TO_TICKS(cmd_samples_from_speech * 1000 / 16000 + 200));
                        audio_hal_flush_rx();
                        ai_cloud_stop_session();
                        ei_wakeword_reset();
                        idle_slice_counter = 0;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        break;
#else
                        // SMART NOISE GUARD: Nếu Model 2 xác nhận là tạp âm ('noise' >= 80%) và sau đó người dùng hoàn toàn im lặng
                        if (last_lbl && strcmp(last_lbl, "noise") == 0 && last_conf >= 0.80f && command_silence_chunks >= STREAM_SILENCE_TARGET) {
                            ESP_LOGW(TAG, ">>> [SMART NOISE GUARD] Tạp âm ngoại cảnh ('noise': %.1f%%) & người dùng không nói tiếp -> Hủy lượt êm! <<<", last_conf * 100.0f);
                            ai_cloud_cancel_turn();
                            ei_wakeword_reset();
                            idle_slice_counter = 0;
                            s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                            break;
                        }

                        ESP_LOGI(TAG, ">>> [HYBRID ROUTING] Model 2 không khớp sau 3 lát cắt ('%s': %.1f%%) -> Chuyển tiếp ngay sang Gemini Live! <<<",
                                 last_lbl ? last_lbl : "none", last_conf * 100.0f);

                        if (sys_core_is_wifi_connected()) {
                            if (command_silence_chunks >= STREAM_SILENCE_TARGET) {
                                ESP_LOGI(TAG, ">>> [CLIENT VAD] Dứt câu nói trong lúc Model 2 chạy (%dms im lặng) -> Chốt câu hỏi ngay lập tức! <<<",
                                         command_silence_chunks * 60);
                                ei_wakeword_reset();
                                ai_cloud_end_turn();
                                s_responding_silent_cycles = 0;
                                s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                                responding_log_counter = 0;
                            } else {
                                s_edge_state = AI_EDGE_STATE_STREAMING_SPEECH;
                                streaming_counter = 0;
                                silence_counter = command_silence_chunks;
                                s_consecutive_speech = (command_silence_chunks == 0) ? 2 : 0;
                                speech_chunks_total = command_speech_chunks;
                            }
                        } else {
                            ESP_LOGE(TAG, ">>> [OFFLINE FALLBACK] Ngoại tuyến hoặc lỗi kết nối Live API! <<<");
                            audio_hal_play_error_sound();
                            ai_cloud_stop_session();
                            ei_wakeword_reset();
                            idle_slice_counter = 0;
                            s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        }
                        break;
#endif
                    }

                    // 6. Failsafe sau tối đa 4.5s (75 chunks) để bảo vệ luồng không bị treo:
                    if (command_total_chunks >= 75) {
#if DEBUG_LOCAL_TEST_MODE
                        if (command_speech_started && cmd_samples_from_speech > 0) {
                            ESP_LOGW(TAG, ">>> [LOCAL TEST TIMEOUT] Hết 4.5s -> Phát lại âm thanh thu được (%d mẫu) ra loa <<<", (int)cmd_samples_from_speech);
                            audio_hal_debug_play_captured_audio(cmd_audio_buffer, cmd_samples_from_speech);
                            vTaskDelay(pdMS_TO_TICKS(cmd_samples_from_speech * 1000 / 16000 + 200));
                            audio_hal_flush_rx();
                        }
                        ai_cloud_stop_session();
                        ei_wakeword_reset();
                        idle_slice_counter = 0;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        break;
#else
                        if (command_speech_started) {
                            ESP_LOGI(TAG, ">>> [TIMEOUT ROUTING] Hết thời gian câu lệnh cục bộ -> Chốt dứt câu sang Gemini Live! <<<");
                            if (sys_core_is_wifi_connected()) {
                                ei_wakeword_reset();
                                ai_cloud_end_turn();
                                s_responding_silent_cycles = 0;
                                s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                                responding_log_counter = 0;
                            } else {
                                ESP_LOGE(TAG, ">>> [OFFLINE FALLBACK] Ngoại tuyến hoặc lỗi kết nối Live API! <<<");
                                audio_hal_play_error_sound();
                                ai_cloud_stop_session();
                                ei_wakeword_reset();
                                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                            }
                        } else {
                            ESP_LOGI(TAG, ">>> [NO-SPEECH GUARD] Hết thời gian chờ mà không phát hiện tiếng nói -> Hủy lượt & tiếp tục lắng nghe. <<<");
                            ai_cloud_stop_session();
                            ei_wakeword_reset();
                            s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        }
                        break;
#endif
                    }
                }
                break;
            }

            case AI_EDGE_STATE_STREAMING_SPEECH: {
                prev_wake_rms = 0;
                // Đọc chunk 60ms từ mic (960 mẫu)
                size_t bytes_to_read = STREAM_CHUNK_SAMPLES * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(stream_chunk_buffer, bytes_to_read);

                if (bytes_read > 0) {
                    int rms = 0;
                    bool has_voice = audio_vad_is_speech_chunk(stream_chunk_buffer, bytes_read / sizeof(int16_t), &rms);

                    // Gửi chunk PCM lên WebSocket RingBuffer
                    ai_cloud_send_audio_chunk((uint8_t *)stream_chunk_buffer, bytes_read);

                    if (has_voice) {
                        speech_chunks_total++;
                        s_consecutive_speech++;
                        if (s_consecutive_speech >= 2) {
                            silence_counter = 0;
                        }
                    } else {
                        s_consecutive_speech = 0;
                        silence_counter++;
                    }

                    streaming_counter++;

                    // VAD Cutoff Window: Dừng sau ĐÚNG 1.0s im lặng (17 chunks * 60ms = 1020ms) để phản xạ cực nhanh!
                    int silence_target = STREAM_SILENCE_TARGET;

                    if (streaming_counter % 8 == 0) {
                        ESP_LOGI(TAG, "[STREAM VAD] RMS: %4d (Floor: %2d) | %s | Silence: %2d/%d (Speech: %d)",
                                 rms, audio_dsp_get_ambient_noise(),
                                 has_voice ? "CÓ TIẾNG" : "Im lặng  ",
                                 silence_counter, silence_target, speech_chunks_total);
                    }

                    // DỨT CÂU NÓI HỢP LỆ (1.5s im lặng)
                    if (silence_counter >= silence_target) {
                        ESP_LOGI(TAG, ">>> [CLIENT VAD] Dứt câu nói (1.0s im lặng) -> Chốt câu hỏi lên Gemini Live <<<");
                        ei_wakeword_reset();
                        ai_cloud_end_turn();
                        s_responding_silent_cycles = 0;
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                        responding_log_counter = 0;
                        break;
                    }

                    // Giới hạn an toàn: Nói tối đa 15 giây thì tự ngắt câu
                    if (streaming_counter >= (15 * 1000 / 60)) {
                        ESP_LOGW(TAG, "Đã đạt giới hạn tối đa 15 giây -> Tự động kết thúc câu hỏi!");
                        ei_wakeword_reset();
                        ai_cloud_end_turn();
                        s_responding_silent_cycles = 0;
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
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
                        ai_cloud_stop_session();
                        ei_wakeword_reset();
                        idle_slice_counter = 0;
                        wake_dma_flushed = false;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        break;
                    }
                }

                // Kiểm tra khi cả Cloud đã trả lời xong VÀ loa đã xả hết âm thanh
                // Chế độ 1 Hỏi - 1 Đáp (Single-Turn): Đóng phiên sạch sẽ & trở về chờ 'Hey Cherry'
                if (!cloud_busy && !speaker_playing) {
                    ESP_LOGI(TAG, ">>> [TURN COMPLETE] AI trả lời hoàn tất & loa đã nghỉ -> Trở về lắng nghe 'Hey Cherry' <<<");
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
                            if (sys_core_is_wifi_connected()) {
                                ai_cloud_prewarm_session();
                            }
                            audio_hal_play_wake_sound();
                            s_edge_state = AI_EDGE_STATE_COMMAND_LISTENING;
                            break;
                        }

                        case AI_EDGE_STATE_COMMAND_LISTENING:
                        case AI_EDGE_STATE_STREAMING_SPEECH: {
                            ESP_LOGI(TAG, ">>> BOOT BUTTON CLICK: CHỐT CÂU LỆNH NGAY LẬP TỨC <<<");
                            ei_wakeword_reset();
                            ai_cloud_end_turn();
                            s_responding_silent_cycles = 0;
                            s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                            break;
                        }

                        case AI_EDGE_STATE_AI_RESPONDING: {
                            ESP_LOGI(TAG, ">>> BOOT BUTTON CLICK: BARGE-IN NGẮT LỜI AI <<<");
                            audio_hal_stop_playback();
                            ai_cloud_abort();
                            audio_hal_play_wake_sound();
                            s_edge_state = AI_EDGE_STATE_COMMAND_LISTENING;
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

    xTaskCreatePinnedToCore(button_trigger_task, "button_task", 4096, NULL, 5, &button_task_handle, 1);
    // Stack 16KB đảm bảo an toàn tuyệt đối cho TFLite Micro inference
    xTaskCreatePinnedToCore(audio_recording_task, "audio_task", 16384, NULL, 5, &audio_task_handle, 1);
}

void ai_edge_stop_recording(void) {
    if (s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD) {
        ESP_LOGI(TAG, "Dừng ghi âm khẩn cấp (Hủy bởi Cloud/Socket Error).");
        ei_wakeword_reset();
        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
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
