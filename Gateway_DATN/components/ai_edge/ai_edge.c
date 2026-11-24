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

static const char *TAG = "ai_edge";

#define BOOT_BUTTON_GPIO         0
#define STREAM_CHUNK_SAMPLES     960    // 60ms @ 16kHz
#define STREAM_SILENCE_TARGET    14     // 14 * 60ms = 840ms im lặng để chốt lượt với Gemini Live (phản xạ nhanh)

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
    int cmd_start_voice_streak = 0;
    float stream_noise_floor = 16.0f;

    int streaming_counter = 0;
    int silence_counter = 0;
    int speech_chunks_total = 0;
    int s_consecutive_speech = 0;
    int idle_slice_counter = 0;
    int responding_log_counter = 0;
    int prev_wake_rms = 0;
    int stream_peak_rms = 0;

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " HỆ THỐNG LẮNG NGHE ĐÃ SẴN SÀNG (HYBRID DUAL-MODEL)!");
    ESP_LOGI(TAG, " - Wake Word: 'Hey Cherry' (Continuous 250ms)");
    ESP_LOGI(TAG, " - Command Control: 6 Smart Home Labels (1.5s Buffer, 3-Slice Energy-Centered Scan)");
    ESP_LOGI(TAG, " - Cloud AI: Google Gemini Live API Streaming");
    ESP_LOGI(TAG, "==================================================");

    while (1) {
        switch (s_edge_state) {
            case AI_EDGE_STATE_IDLE_WAKEWORD: {
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

                    cmd_inference_count = 0;
                    cmd_samples_from_speech = 0;
                    command_speech_started = false;
                    wake_dma_flushed = false;
                    command_speech_chunks = 0;
                    command_silence_chunks = 0;
                    command_total_chunks = 0;
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
                    // (confidence < 0.30f và rms < ambient * 2), tránh việc người dùng đang nói làm tăng vọt chỉ số sàn ồn
                    if (confidence < 0.30f && rms < ambient * 2) {
                        audio_dsp_update_ambient_noise(rms);
                    }

                    // Chốt chặn SNR Gate thích ứng theo nhiễu nền:
                    // Chỉ loại bỏ khi biên độ quá nhỏ so với sàn ồn (max_rms_2slices < 20 hoặc < ambient + 4)
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

                        // 1. Kích hoạt kết nối WebSocket ngầm (Pre-warm) nếu có Wi-Fi (Tắt khi test cô lập để tối ưu 100% tài nguyên)
#if !DEBUG_LOCAL_TEST_MODE
                        if (sys_core_is_wifi_connected()) {
                            ai_cloud_prewarm_session();
                        }
#endif

                        // 2. Phát âm thanh báo thức (Ding-ding trọn vẹn ~450ms)
                        audio_hal_play_wake_sound();
                        ui_display_show_voice_overlay();

                        // 3. Reset các bộ đệm và biến trạng thái câu lệnh
                        cmd_inference_count = 0;
                        cmd_samples_from_speech = 0;
                        command_speech_started = false;
                        wake_dma_flushed = false;
                        command_speech_chunks = 0;
                        command_silence_chunks = 0;
                        command_total_chunks = 0;
                        cmd_start_voice_streak = 0;
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
                if (s_manual_finish_requested) {
                    s_manual_finish_requested = false;
                    ESP_LOGI(TAG, ">>> MANUAL FINISH REQUESTED (Touch Button): CHỐT CÂU LỆNH NGAY LẬP TỨC <<<");
                    ei_wakeword_reset();
                    ai_cloud_end_turn();
                    s_responding_silent_cycles = 0;
                    s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                    responding_log_counter = 0;
                    break;
                }
                prev_wake_rms = 0;
                // Đọc từng chunk 60ms (960 mẫu = 1920 bytes) để VAD chính xác từng mili-giây
                size_t bytes_to_read = STREAM_CHUNK_SAMPLES * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(stream_chunk_buffer, bytes_to_read);

                if (bytes_read == bytes_to_read) {
                    audio_hal_apply_dc_filter(stream_chunk_buffer, STREAM_CHUNK_SAMPLES);

                    int rms = audio_vad_get_rms(stream_chunk_buffer, STREAM_CHUNK_SAMPLES);
                    // Cắt đứt lưu lượng SPI2 trong lúc thu âm để không gây nhiễu chéo vào mic INMP441
                    // ui_display_update_audio_rms((uint16_t)rms);
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
                            cmd_start_voice_streak++;
                        } else {
                            cmd_start_voice_streak = 0;
                        }

                        if (cmd_start_voice_streak >= 2) {
                            command_speech_started = true;
                            command_speech_chunks = 2;
                            command_silence_chunks = 0;
                            ESP_LOGI(TAG, ">>> [VOICE START] Bắt đầu nói (2 chunks liên tiếp có tiếng, RMS: %d, Sàn ồn: %d) -> Lùi %dms giữ trọn phụ âm đầu! <<<",
                                     rms, floor, preroll_count * 60);

                            // Kích hoạt Stream song song lên Cloud ngay lập tức (Speculative Streaming)
                            if (sys_core_is_wifi_connected()) {
                                ai_cloud_start_turn();
                            }

                            // Nạp các chunk Pre-roll (lùi tối đa 300ms) vào đầu cmd_audio_buffer VÀ stream ngay lên Cloud
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
                            // Quá 5.0s không ai nói gì: No-Speech Guard (84 chunks * 60ms = 5.04s)
                            if (command_total_chunks >= 84) {
                                ESP_LOGI(TAG, ">>> [NO-SPEECH GUARD] Không phát hiện tiếng nói sau 5.0s -> Hủy lượt & tiếp tục lắng nghe. <<<");
                                ui_display_hide_voice_overlay();
                                ai_cloud_stop_session();
                                ei_wakeword_reset();
                                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                                break;
                            }
                            break;
                        } else {
                            // has_voice chunk 1: Đang chờ chunk 2 xác nhận để loại bỏ hoàn toàn xung nhiễu / đuôi chuông
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

                        // Bước 2.5: Kiểm tra xác thực giọng nói cận trường (Near-field Voice Validation)
                        // Giọng người nói trực diện trước Gateway luôn có max_speech_rms >= 150.
                        // Nếu max_speech_rms < 150, đây là xung tạp âm môi trường/động vật ở xa -> Kích hoạt Noise Rejection!
                        if (max_speech_rms < 150) {
                            ESP_LOGW(TAG, ">>> [NOISE REJECTION] Tạp âm môi trường xa (MaxRMS: %d < 150) -> Hủy bỏ lượt thu âm <<<", max_speech_rms);
                            ui_display_hide_voice_overlay();
                            ai_cloud_stop_session();
                            ei_wakeword_reset();
                            s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                            break;
                        }

                        // Bước 3: Đếm tổng chunk giọng nói, xác định biên độ câu lệnh chính (Gap-aware Utterance Boundary)
                        int first_speech_chunk = -1;
                        int last_speech_chunk = -1;
                        int total_user_chunks = 0;
                        int tail_active_chunks = 0;
                        int silence_gap = 0;
                        bool main_utterance_closed = false;

                        for (int k = 0; k < 25; k++) {
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

                        // Bước 4: Kiểm tra điều kiện Câu dài liên tục (Continuous Long Utterance):
                        // Chỉ bỏ qua Model 2 khi người dùng thực sự vẫn đang nói liên tục không ngừng nghỉ ở mép 1.5s:
                        // - Câu nói chưa dứt (!main_utterance_closed)
                        // - Đuôi 300ms cuối (chunks 20..24) có ít nhất 4/5 chunk năng lượng cao (tail_active_chunks >= 4)
                        // - Cả 2 chunk cuối cùng (23 & 24) đều có tiếng nói
                        // - Tổng số chunk giọng nói tích lũy lớn (total_user_chunks >= 20)
                        bool is_continuous_long_speech = (!main_utterance_closed) &&
                                                         (tail_active_chunks >= 4) &&
                                                         (chunk_rms[23] >= user_voice_thresh && chunk_rms[24] >= user_voice_thresh) &&
                                                         (total_user_chunks >= 20);

                        if (is_continuous_long_speech) {
                            char rms_csv[180] = {0};
                            int roff = 0;
                            for (int k = 0; k < 25; k++) {
                                roff += snprintf(rms_csv + roff, sizeof(rms_csv) - roff, "%d%s", chunk_rms[k], k < 24 ? "," : "");
                            }
                            ESP_LOGI(TAG, ">>> [CONTINUOUS SPEECH DETECTED] Người dùng vẫn đang nói ở cuối 1.5s (Tail: %d/5, Total: %d, MaxRMS: %d, Thresh: %d) -> Chuyển thẳng sang Gemini Live! <<<",
                                     tail_active_chunks, total_user_chunks, max_speech_rms, user_voice_thresh);

                            if (sys_core_is_wifi_connected()) {
                                s_edge_state = AI_EDGE_STATE_STREAMING_SPEECH;
                                streaming_counter = 0;
                                silence_counter = command_silence_chunks;
                                s_consecutive_speech = 2;
                                speech_chunks_total = command_speech_chunks;
                                stream_peak_rms = max_speech_rms;
                                stream_noise_floor = (float)audio_dsp_get_ambient_noise();
                            } else {
                                ESP_LOGE(TAG, ">>> [OFFLINE FALLBACK] Ngoại tuyến hoặc lỗi kết nối Live API! <<<");
                                audio_hal_play_error_sound();
                                ui_display_hide_voice_overlay();
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
                        } else {
                            // Fallback: Ước lượng câu nói bắt đầu sau Pre-roll 300ms (sample 4800)
                            speech_center_sample = 4800 + (EI_COMMAND_WINDOW_SAMPLES / 2); // 4800 + 9600 = 14400 (900ms)
                        }

                        char rms_csv[180] = {0};
                        int roff = 0;
                        for (int k = 0; k < 25; k++) {
                            roff += snprintf(rms_csv + roff, sizeof(rms_csv) - roff, "%d%s", chunk_rms[k], k < 24 ? "," : "");
                        }
                        ESP_LOGI(TAG, ">>> [MODEL 2 INFERENCE] chunk_rms=[%s] | Span: %d chunks (%dms -> %dms, Tâm: %dms) | Tail: %d/5 | Total: %d | Thresh: %d | MaxRMS: %d <<<",
                                 rms_csv, speech_span_chunks,
                                 (first_speech_chunk >= 0 ? first_speech_chunk * 60 : 0),
                                 (last_speech_chunk >= 0 ? (last_speech_chunk + 1) * 60 : 0),
                                 speech_center_sample / 16, tail_active_chunks, total_user_chunks, user_voice_thresh, max_speech_rms);

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
                                ui_display_notify_command_executed(lbl, 45);
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
                            } else if (s == 0) {
                                // Centered Slice: Nếu phân loại xác nhận rõ ràng là câu hỏi mở (unknown >= 70%) hoặc tạp âm (noise >= 75%)
                                // thì lập tức ngắt sớm chuyển thẳng Gemini Live, không tốn thêm 2 lát cắt (~1.3s CPU)!
                                if ((lbl && strcmp(lbl, "unknown") == 0 && conf >= 0.70f) ||
                                    (lbl && strcmp(lbl, "noise") == 0 && conf >= 0.75f)) {
                                    ESP_LOGI(TAG, ">>> [MODEL 2 EARLY ROUTE] Lát cắt tâm rõ ràng không phải lệnh ('%s': %.1f%%) -> Chuyển thẳng Cloud! <<<",
                                             lbl ? lbl : "none", conf * 100.0f);
                                    break;
                                }
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
                        ui_display_hide_voice_overlay();
                        ai_cloud_stop_session();
                        ei_wakeword_reset();
                        idle_slice_counter = 0;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        break;
#else
                        // SMART NOISE GUARD: Nếu Model 2 xác nhận là tạp âm ('noise' >= 80%) và sau đó người dùng hoàn toàn im lặng
                        if (last_lbl && strcmp(last_lbl, "noise") == 0 && last_conf >= 0.80f && command_silence_chunks >= STREAM_SILENCE_TARGET) {
                            ESP_LOGW(TAG, ">>> [SMART NOISE GUARD] Tạp âm ngoại cảnh ('noise': %.1f%%) & người dùng không nói tiếp -> Hủy lượt êm! <<<", last_conf * 100.0f);
                            ui_display_hide_voice_overlay();
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
                                stream_peak_rms = max_speech_rms;
                                stream_noise_floor = (float)audio_dsp_get_ambient_noise();
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
                            ui_display_hide_voice_overlay();
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
                if (s_manual_finish_requested) {
                    s_manual_finish_requested = false;
                    ESP_LOGI(TAG, ">>> MANUAL FINISH REQUESTED (Touch Button): CHỐT CÂU LỆNH NGAY LẬP TỨC <<<");
                    ei_wakeword_reset();
                    ai_cloud_end_turn();
                    s_responding_silent_cycles = 0;
                    s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                    responding_log_counter = 0;
                    break;
                }
                prev_wake_rms = 0;
                // Đọc chunk 60ms từ mic (960 mẫu)
                size_t bytes_to_read = STREAM_CHUNK_SAMPLES * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(stream_chunk_buffer, bytes_to_read);

                if (bytes_read > 0) {
                    // BẮT BUỘC: Lọc sạch DC Offset và sóng hạ âm trước khi tính RMS và VAD!
                    audio_hal_apply_dc_filter(stream_chunk_buffer, bytes_read / sizeof(int16_t));

                    int rms = 0;
                    bool has_voice = audio_vad_is_speech_chunk(stream_chunk_buffer, bytes_read / sizeof(int16_t), &rms);
                    // Cắt đứt lưu lượng SPI2 trong lúc thu âm để không gây nhiễu chéo vào mic INMP441
                    // ui_display_update_audio_rms((uint16_t)rms);

                    // Gửi chunk PCM lên WebSocket RingBuffer
                    ai_cloud_send_audio_chunk((uint8_t *)stream_chunk_buffer, bytes_read);

                    // Thích nghi sàn ồn nhanh khi WebRTC VAD xác nhận không phải giọng người (tiếng quạt, gió lùa, ồn lớp học)
                    if (!has_voice) {
                        if ((float)rms > stream_noise_floor) {
                            stream_noise_floor = stream_noise_floor * 0.85f + (float)rms * 0.15f;
                        } else {
                            stream_noise_floor = stream_noise_floor * 0.80f + (float)rms * 0.20f;
                        }
                        if (stream_noise_floor < 15.0f) stream_noise_floor = 15.0f;
                        if (stream_noise_floor > 600.0f) stream_noise_floor = 600.0f;
                    }

                    int abs_gate = (int)(stream_noise_floor * 1.55f) + 35;
                    if (abs_gate < 110) abs_gate = 110;
                    int rel_gate = (int)(stream_peak_rms * 0.30f);
                    int active_speech_thresh = (abs_gate > rel_gate) ? abs_gate : rel_gate;

                    bool user_speaking = (has_voice && rms >= active_speech_thresh);

                    if (user_speaking) {
                        speech_chunks_total++;
                        s_consecutive_speech++;
                        // Chỉ reset silence_counter = 0 và cập nhật stream_peak_rms khi s_consecutive_speech >= 4
                        // (240ms giọng nói liên tục vượt ngưỡng), tránh tiếng gió quạt hay tiếng ồn thoảng qua xóa sạch tiến độ
                        if (s_consecutive_speech >= 4) {
                            silence_counter = 0;
                            if (rms > stream_peak_rms) stream_peak_rms = rms;
                        }
                    } else {
                        s_consecutive_speech = 0;
                        silence_counter++;
                    }

                    streaming_counter++;

                    // VAD Cutoff Window: Dừng sau ĐÚNG ~840ms im lặng (14 chunks * 60ms = 840ms) để phản xạ cực nhanh!
                    int silence_target = STREAM_SILENCE_TARGET;

                    if (streaming_counter % 8 == 0) {
                        ESP_LOGI(TAG, "[STREAM VAD] RMS: %4d (Peak: %4d, Thresh: %3d, Floor: %2d, NoiseFlr: %d) | %s | Silence: %2d/%d (Speech: %d)",
                                 rms, stream_peak_rms, active_speech_thresh, audio_dsp_get_ambient_noise(),
                                 (int)stream_noise_floor,
                                 user_speaking ? "CÓ TIẾNG" : "Im lặng  ",
                                 silence_counter, silence_target, speech_chunks_total);
                    }

                    // DỨT CÂU NÓI HỢP LỆ (14 chunks * 60ms = 840ms im lặng)
                    if (silence_counter >= silence_target) {
                        ESP_LOGI(TAG, ">>> [CLIENT VAD] Dứt câu nói (%dms im lặng) -> Chốt câu hỏi lên Gemini Live <<<", silence_target * 60);
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

                if ((cloud_busy || speaker_playing) && !s_cloud_speaking_notified) {
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

                // Kiểm tra khi cả Cloud đã trả lời xong VÀ loa đã xả hết âm thanh
                // Chế độ 1 Hỏi - 1 Đáp (Single-Turn): Đóng phiên sạch sẽ & trở về chờ 'Hey Cherry'
                if (!cloud_busy && !speaker_playing) {
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
    if (s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD) {
        ESP_LOGI(TAG, "Dừng ghi âm khẩn cấp (Hủy bởi Cloud/Socket Error).");
        ei_wakeword_reset();
        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
        ui_display_hide_voice_overlay();
    }
}

void ai_edge_abort(void) {
    if (s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD) {
        ESP_LOGI(TAG, "Huỷ phiên tương tác giọng nói (Abort requested by Touch/Button)");
        ai_cloud_stop_session();
        audio_hal_stop_playback();
        ei_wakeword_reset();
        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
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
