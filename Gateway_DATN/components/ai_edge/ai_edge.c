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
#define CLOUD_PREBUFFER_SIZE     128000 // 4.0s @ 16kHz 16-bit mono = 64,000 samples = 128,000 bytes (PSRAM)
#define STREAM_SILENCE_TARGET    17     // 17 * 60ms = 1020ms ~ 1.0s im lặng để chốt lượt với Gemini Live

// =============================================================================
// [CHẾ ĐỘ KIỂM THỬ MODEL 2 CÔ LẬP]
// 1: Khi Model 2 không khớp, KHÔNG gọi Cloud, phát lại 1.6s âm thanh ra loa để test, rồi tự về IDLE (0% xung đột mạng)
// 0: Chế độ hoạt động chuẩn (Pre-warm WebSocket, Hybrid Cloud Fallback)
// =============================================================================
#define DEBUG_LOCAL_TEST_MODE    0
#define FULL_1_6S_SAMPLES        (EI_COMMAND_WINDOW_SAMPLES + EI_COMMAND_STRIDE_SAMPLES) // 25.600 mẫu @ 16kHz = 1.6 giây

static TaskHandle_t audio_task_handle = NULL;
static TaskHandle_t button_task_handle = NULL;
static volatile ai_edge_state_t s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;

static void dispatch_local_mesh_command(const char *cmd_label) {
    if (!cmd_label) return;
    char mesh_cmd[64] = {0};
    if (strcmp(cmd_label, "bat_den") == 0) {
        snprintf(mesh_cmd, sizeof(mesh_cmd), "<CMD:LIGHT:ON>");
    } else if (strcmp(cmd_label, "tat_den") == 0) {
        snprintf(mesh_cmd, sizeof(mesh_cmd), "<CMD:LIGHT:OFF>");
    } else if (strcmp(cmd_label, "bat_quat") == 0) {
        snprintf(mesh_cmd, sizeof(mesh_cmd), "<CMD:FAN:ON>");
    } else if (strcmp(cmd_label, "tat_quat") == 0) {
        snprintf(mesh_cmd, sizeof(mesh_cmd), "<CMD:FAN:OFF>");
    } else if (strcmp(cmd_label, "bat_cong_tac") == 0) {
        snprintf(mesh_cmd, sizeof(mesh_cmd), "<CMD:SWITCH:ON>");
    } else if (strcmp(cmd_label, "tat_cong_tac") == 0) {
        snprintf(mesh_cmd, sizeof(mesh_cmd), "<CMD:SWITCH:OFF>");
    } else {
        snprintf(mesh_cmd, sizeof(mesh_cmd), "<CMD:%s>", cmd_label);
    }
    ESP_LOGI(TAG, "Gửi lệnh thực thi tới Thread Mesh qua UART: %s", mesh_cmd);
    mesh_controller_send_command(mesh_cmd);
}

static void audio_recording_task(void *arg) {
    ESP_LOGI(TAG, "Audio Processing Task Started (Dual-Model Edge Impulse Engine).");

    // Khởi tạo Model 1 (Hey Cherry) và Model 2 (Smart Home Control)
    ei_wakeword_init();
    ei_command_init();

    // 1. Buffer lát cắt Wake Word (4000 mẫu = 250ms @ 16kHz = 8000 bytes)
    size_t wake_slice_size = ei_wakeword_get_slice_size();
    if (wake_slice_size == 0) wake_slice_size = EI_WAKEWORD_SLICE_SAMPLES;
    int16_t *wake_slice_buffer = (int16_t *)heap_caps_malloc(wake_slice_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!wake_slice_buffer) wake_slice_buffer = (int16_t *)malloc(wake_slice_size * sizeof(int16_t));

    // 2. Buffer streaming 60ms cho mic (960 mẫu @ 16kHz = 1920 bytes)
    int16_t *stream_chunk_buffer = (int16_t *)heap_caps_malloc(STREAM_CHUNK_SAMPLES * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stream_chunk_buffer) stream_chunk_buffer = (int16_t *)malloc(STREAM_CHUNK_SAMPLES * sizeof(int16_t));

    // 3. Buffer gom trọn vẹn 1.2s câu lệnh cho Model 2 (19.200 mẫu = 38.400 bytes trong PSRAM)
    int16_t *command_full_buffer = (int16_t *)heap_caps_malloc(EI_COMMAND_WINDOW_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!command_full_buffer) command_full_buffer = (int16_t *)malloc(EI_COMMAND_WINDOW_SAMPLES * sizeof(int16_t));

    // 4. Buffer gom bước trượt 400ms (6.400 mẫu = 12.800 bytes trong PSRAM)
    int16_t *stride_buffer = (int16_t *)heap_caps_malloc(EI_COMMAND_STRIDE_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!stride_buffer) stride_buffer = (int16_t *)malloc(EI_COMMAND_STRIDE_SAMPLES * sizeof(int16_t));

    // 5. Buffer đệm âm thanh dự phòng cho Cloud (PSRAM 128KB ~ 4.0 giây)
    uint8_t *cloud_prebuffer = (uint8_t *)heap_caps_malloc(CLOUD_PREBUFFER_SIZE, MALLOC_CAP_SPIRAM);
    if (!cloud_prebuffer) cloud_prebuffer = (uint8_t *)malloc(CLOUD_PREBUFFER_SIZE);

    // 6. Buffer kiểm thử 1.6s trọn vẹn (25.600 mẫu = 51.200 bytes trong PSRAM)
    int16_t *debug_1_6s_buffer = (int16_t *)heap_caps_malloc(FULL_1_6S_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!debug_1_6s_buffer) debug_1_6s_buffer = (int16_t *)malloc(FULL_1_6S_SAMPLES * sizeof(int16_t));
    size_t debug_1_6s_samples = 0;

    // 6. Buffer Pre-roll 420ms (7 chunks * 60ms) lùi lại >240ms giữ trọn vẹn 100% phụ âm đầu ("b...", "t...")
    #define PREROLL_CHUNKS 7
    static int16_t s_preroll_buffer[PREROLL_CHUNKS][STREAM_CHUNK_SAMPLES];
    memset(s_preroll_buffer, 0, sizeof(s_preroll_buffer));
    int preroll_count = 0;
    int preroll_idx = 0;

    size_t cloud_prebuffer_len = 0;
    int cmd_inference_count = 0;
    size_t cmd_samples_from_speech = 0;
    size_t stride_samples_accum = 0;
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

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " HỆ THỐNG LẮNG NGHE ĐÃ SẴN SÀNG (HYBRID DUAL-MODEL)!");
    ESP_LOGI(TAG, " - Wake Word: 'Hey Cherry' (Continuous 250ms)");
    ESP_LOGI(TAG, " - Command Control: 8 Smart Home Labels (Window 1.2s Full-Sentence)");
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
                    audio_dsp_update_ambient_noise(rms);

                    idle_slice_counter++;
                    // In nhịp tim định kỳ mỗi ~3 giây (12 lát * 250ms)
                    if (idle_slice_counter % 12 == 0) {
                        ESP_LOGI(TAG, "[IDLE_WAKEWORD] Đang lắng nghe 'Hey Cherry'... (Mic RMS: %d, Sàn ồn: %d)",
                                 rms, audio_dsp_get_ambient_noise());
                    }

                    // Chạy nhận diện Wake Word
                    bool detected = ei_wakeword_process_slice(wake_slice_buffer, wake_slice_size, &confidence, &label);

                    if (detected) {
                        ESP_LOGI(TAG, "**************************************************");
                        ESP_LOGI(TAG, ">>> PHÁT HIỆN TỪ KHÓA 'HEY CHERRY'! (Score: %.1f%%, RMS: %d) <<<", confidence * 100.0f, rms);
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
                        cloud_prebuffer_len = 0;
                        cmd_inference_count = 0;
                        cmd_samples_from_speech = 0;
                        stride_samples_accum = 0;
                        debug_1_6s_samples = 0;
                        command_speech_started = false;
                        wake_dma_flushed = false;
                        command_speech_chunks = 0;
                        command_silence_chunks = 0;
                        command_total_chunks = 0;
                        preroll_count = 0;
                        preroll_idx = 0;
                        if (command_full_buffer) {
                            memset(command_full_buffer, 0, EI_COMMAND_WINDOW_SAMPLES * sizeof(int16_t));
                        }
                        if (stride_buffer) {
                            memset(stride_buffer, 0, EI_COMMAND_STRIDE_SAMPLES * sizeof(int16_t));
                        }
                        if (debug_1_6s_buffer) {
                            memset(debug_1_6s_buffer, 0, FULL_1_6S_SAMPLES * sizeof(int16_t));
                        }
                        ei_command_reset_window();

                        // 4. Chuyển sang lắng nghe câu lệnh (thu âm mở trước khi Ding kết thúc 100ms)
                        s_edge_state = AI_EDGE_STATE_COMMAND_LISTENING;
                    }
                } else {
                    ESP_LOGW(TAG, "Mic read mismatch: %u != %u", (unsigned int)bytes_read, (unsigned int)bytes_to_read);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                break;
            }

            case AI_EDGE_STATE_COMMAND_LISTENING: {
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

                    // Ngay khi chuông Ding vừa dứt: xả sạch DMA RX một lần duy nhất để bảo đảm dữ liệu tươi mới
                    if (!wake_dma_flushed) {
                        wake_dma_flushed = true;
                        audio_hal_flush_rx();
                        audio_vad_reset_trigger();
                        preroll_count = 0;
                        preroll_idx = 0;
                        ESP_LOGI(TAG, ">>> [RECORDING OPEN] Chuông Ding đã dứt hoàn toàn -> Bắt đầu thu âm câu lệnh! <<<");
                        break;
                    }

                    // 2. VAD phát hiện giọng nói người (Phương án A: WebRTC VAD Trigger Mode 3 + Sàn ồn)
                    bool has_voice = audio_vad_is_speech_chunk(stream_chunk_buffer, STREAM_CHUNK_SAMPLES, NULL);

                    // 3. Luôn gom âm thanh vào cloud_prebuffer (đảm bảo 100% âm thanh từ sau Ding được giữ lại cho Cloud)
                    if (cloud_prebuffer && (cloud_prebuffer_len + bytes_read <= CLOUD_PREBUFFER_SIZE)) {
                        memcpy(cloud_prebuffer + cloud_prebuffer_len, stream_chunk_buffer, bytes_read);
                        cloud_prebuffer_len += bytes_read;
                    }

                    command_total_chunks++;

                    // 4. Quản lý Pre-roll và Kích hoạt Speech Start:
                    if (!command_speech_started) {
                        // Lưu chunk vào preroll buffer xoay vòng
                        memcpy(s_preroll_buffer[preroll_idx], stream_chunk_buffer, sizeof(int16_t) * STREAM_CHUNK_SAMPLES);
                        preroll_idx = (preroll_idx + 1) % PREROLL_CHUNKS;
                        if (preroll_count < PREROLL_CHUNKS) preroll_count++;

                        if (has_voice) {
                            command_speech_started = true;
                            command_speech_chunks = 1;
                            command_silence_chunks = 0;
                            ESP_LOGI(TAG, ">>> [VOICE START] Bắt đầu nói (RMS: %d, Sàn ồn: %d) -> Lùi %dms giữ trọn phụ âm đầu! <<<",
                                     rms, floor, preroll_count * 60);

                            // Nạp các chunk Pre-roll (lùi tối đa 420ms) vào đầu command_full_buffer để không mất phụ âm đầu ("b...", "t...")
                            cmd_samples_from_speech = 0;
                            debug_1_6s_samples = 0;
                            int start_idx = (preroll_idx - preroll_count + PREROLL_CHUNKS) % PREROLL_CHUNKS;
                            for (int p = 0; p < preroll_count; p++) {
                                int cur_p = (start_idx + p) % PREROLL_CHUNKS;
                                memcpy(&command_full_buffer[cmd_samples_from_speech], s_preroll_buffer[cur_p], STREAM_CHUNK_SAMPLES * sizeof(int16_t));
                                if (debug_1_6s_buffer && (debug_1_6s_samples + STREAM_CHUNK_SAMPLES <= FULL_1_6S_SAMPLES)) {
                                    memcpy(&debug_1_6s_buffer[debug_1_6s_samples], s_preroll_buffer[cur_p], STREAM_CHUNK_SAMPLES * sizeof(int16_t));
                                    debug_1_6s_samples += STREAM_CHUNK_SAMPLES;
                                }
                                cmd_samples_from_speech += STREAM_CHUNK_SAMPLES;
                            }
                        } else {
                            audio_dsp_update_ambient_noise(rms);
                            // Quá 3.6s không ai nói gì: No-Speech Guard (60 chunks * 60ms = 3.6s)
                            if (command_total_chunks >= 60) {
                                ESP_LOGI(TAG, ">>> [NO-SPEECH GUARD] Không phát hiện tiếng nói sau 3.6s -> Hủy lượt & tiếp tục lắng nghe. <<<");
                                ai_cloud_stop_session();
                                ei_wakeword_reset();
                                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                                break;
                            }
                            break;
                        }
                    } else {
                        // Đã bắt đầu nói
                        if (has_voice) {
                            command_speech_chunks++;
                            command_silence_chunks = 0;
                        } else {
                            command_silence_chunks++;
                        }

                        // Tiếp tục gom âm thanh vào command_full_buffer
                        if (cmd_samples_from_speech + STREAM_CHUNK_SAMPLES <= EI_COMMAND_WINDOW_SAMPLES) {
                            memcpy(&command_full_buffer[cmd_samples_from_speech], stream_chunk_buffer, STREAM_CHUNK_SAMPLES * sizeof(int16_t));
                            cmd_samples_from_speech += STREAM_CHUNK_SAMPLES;
                        }

                        // Gom song song toàn bộ 1.6s cho debug buffer
                        if (debug_1_6s_buffer && (debug_1_6s_samples + STREAM_CHUNK_SAMPLES <= FULL_1_6S_SAMPLES)) {
                            memcpy(&debug_1_6s_buffer[debug_1_6s_samples], stream_chunk_buffer, STREAM_CHUNK_SAMPLES * sizeof(int16_t));
                            debug_1_6s_samples += STREAM_CHUNK_SAMPLES;
                        }
                    }

                    // 5. CƠ CHẾ CĂN KHUNG MODEL 2 (TỐI ĐA 2 LẦN)
                    // LẦN 1: Gom đủ 1.2s từ khi bắt đầu nói (19.200 mẫu)
                    if (cmd_inference_count == 0 && cmd_samples_from_speech >= EI_COMMAND_WINDOW_SAMPLES) {
                        cmd_inference_count = 1;
                        float conf = 0.0f;
                        const char *lbl = NULL;
                        ESP_LOGI(TAG, ">>> [MODEL 2 - LẦN 1/2] Cửa sổ 1.2s từ lúc bắt đầu nói -> Chạy suy luận căn khung! <<<");
                        bool match = ei_command_classify(command_full_buffer, EI_COMMAND_WINDOW_SAMPLES, &conf, &lbl);
                        vTaskDelay(pdMS_TO_TICKS(15));

                        if (match) {
                            ESP_LOGW(TAG, "==================================================");
                            ESP_LOGW(TAG, ">>> [LOCAL COMMAND SUCCESS]: '%s' (Confidence: %.1f%%) <<<", lbl, conf * 100.0f);
                            ESP_LOGW(TAG, "==================================================");
                            dispatch_local_mesh_command(lbl);
                            ai_cloud_stop_session();
                            ei_wakeword_reset();
                            s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                            break;
                        }

                        // Lần 1 chưa khớp: Thu tiếp 400ms đến mốc 1.6s để chạy Lần 2 (cửa sổ trượt 0.4s - 1.6s)
                        ESP_LOGI(TAG, ">>> [MODEL 2 - THU TIẾP 400MS] Lần 1 không khớp ('%s': %.1f%%) -> Thu tiếp 400ms đến 1.6s cho Lần 2... <<<",
                                 lbl ? lbl : "none", conf * 100.0f);
                        stride_samples_accum = 0;
                    } else if (cmd_inference_count == 1) {
                        // Người dùng vẫn đang nói: thu tiếp 400ms (6.400 mẫu) nữa để đạt mốc 1.6s
                        // STREAM_CHUNK_SAMPLES = 960, EI_COMMAND_STRIDE_SAMPLES = 6400 (6400 = 6 * 960 + 640)
                        // Cần nạp phần dư 640 mẫu ở chunk thứ 7 để chạm đúng mốc 6400 mà không bị kẹt
                        if (stride_buffer && (stride_samples_accum < EI_COMMAND_STRIDE_SAMPLES)) {
                            size_t needed = EI_COMMAND_STRIDE_SAMPLES - stride_samples_accum;
                            size_t to_copy = (STREAM_CHUNK_SAMPLES < needed) ? STREAM_CHUNK_SAMPLES : needed;
                            memcpy(&stride_buffer[stride_samples_accum], stream_chunk_buffer, to_copy * sizeof(int16_t));
                            stride_samples_accum += to_copy;
                        }

                        // Tiếp tục gom vào debug_1_6s_buffer cho đến khi đủ trọn vẹn 1.6s (25.600 mẫu)
                        if (debug_1_6s_buffer && (debug_1_6s_samples < FULL_1_6S_SAMPLES)) {
                            size_t needed_16 = FULL_1_6S_SAMPLES - debug_1_6s_samples;
                            size_t to_copy_16 = (STREAM_CHUNK_SAMPLES < needed_16) ? STREAM_CHUNK_SAMPLES : needed_16;
                            memcpy(&debug_1_6s_buffer[debug_1_6s_samples], stream_chunk_buffer, to_copy_16 * sizeof(int16_t));
                            debug_1_6s_samples += to_copy_16;
                        }

                        if (stride_samples_accum >= EI_COMMAND_STRIDE_SAMPLES) {
                            cmd_inference_count = 2;
                            stride_samples_accum = 0;

                            // Trượt cửa sổ sang trái 400ms (6.400 mẫu), nạp 400ms mới vào đuôi -> Cửa sổ 0.4s - 1.6s
                            memmove(&command_full_buffer[0], &command_full_buffer[EI_COMMAND_STRIDE_SAMPLES],
                                    (EI_COMMAND_WINDOW_SAMPLES - EI_COMMAND_STRIDE_SAMPLES) * sizeof(int16_t));
                            memcpy(&command_full_buffer[EI_COMMAND_WINDOW_SAMPLES - EI_COMMAND_STRIDE_SAMPLES],
                                   stride_buffer, EI_COMMAND_STRIDE_SAMPLES * sizeof(int16_t));

                            float conf = 0.0f;
                            const char *lbl = NULL;
                            ESP_LOGI(TAG, ">>> [MODEL 2 - LẦN 2/2] Cửa sổ 0.4s - 1.6s -> Chạy suy luận! <<<");
                            bool match = ei_command_classify(command_full_buffer, EI_COMMAND_WINDOW_SAMPLES, &conf, &lbl);
                            vTaskDelay(pdMS_TO_TICKS(15));

                            if (match) {
                                ESP_LOGW(TAG, "==================================================");
                                ESP_LOGW(TAG, ">>> [LOCAL COMMAND SUCCESS]: '%s' (Confidence: %.1f%%) <<<", lbl, conf * 100.0f);
                                ESP_LOGW(TAG, "==================================================");
                                dispatch_local_mesh_command(lbl);
                                ai_cloud_stop_session();
                                ei_wakeword_reset();
                                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                                break;
                            }

                            // Sau Lần 2 không khớp:
#if DEBUG_LOCAL_TEST_MODE
                            ESP_LOGW(TAG, "==================================================");
                            ESP_LOGW(TAG, ">>> [LOCAL TEST] Model 2 không khớp ('%s': %.1f%%) -> Phát lại trọn vẹn 1.6s ra loa <<<",
                                     lbl ? lbl : "none", conf * 100.0f);
                            ESP_LOGW(TAG, "==================================================");
                            audio_hal_debug_play_captured_audio(debug_1_6s_buffer, FULL_1_6S_SAMPLES);
                            vTaskDelay(pdMS_TO_TICKS(1800)); // Chờ loa phát hết trọn vẹn 1.6s âm thanh
                            audio_hal_flush_rx();
                            ai_cloud_stop_session();
                            ei_wakeword_reset();
                            idle_slice_counter = 0;
                            s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                            break;
#else
                            ESP_LOGI(TAG, ">>> [HYBRID ROUTING] Model 2 Lần 2 không khớp ('%s': %.1f%%) -> Chuyển tiếp sang Gemini Live! <<<",
                                     lbl ? lbl : "none", conf * 100.0f);

                            if (sys_core_is_wifi_connected() && ai_cloud_start_turn()) {
                                if (cloud_prebuffer && cloud_prebuffer_len > 0) {
                                    ai_cloud_send_audio_chunk(cloud_prebuffer, cloud_prebuffer_len);
                                }
                                s_edge_state = AI_EDGE_STATE_STREAMING_SPEECH;
                                streaming_counter = 0;
                                silence_counter = 0;
                                s_consecutive_speech = 0;
                                speech_chunks_total = (cloud_prebuffer_len / (STREAM_CHUNK_SAMPLES * sizeof(int16_t)));
                            } else {
                                ESP_LOGE(TAG, ">>> [OFFLINE FALLBACK] Ngoại tuyến hoặc lỗi kết nối Live API! <<<");
                                audio_hal_play_error_sound();
                                ai_cloud_stop_session();
                                ei_wakeword_reset();
                                s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                            }
                            break;
#endif
                        }
                    }

                    // 6. Failsafe sau tối đa 4.5s (75 chunks) để bảo vệ luồng không bị treo:
                    if (command_total_chunks >= 75) {
#if DEBUG_LOCAL_TEST_MODE
                        if (command_speech_started && debug_1_6s_samples > 0) {
                            ESP_LOGW(TAG, ">>> [LOCAL TEST TIMEOUT] Hết 4.5s -> Phát lại âm thanh thu được ra loa <<<");
                            audio_hal_debug_play_captured_audio(debug_1_6s_buffer, debug_1_6s_samples);
                            vTaskDelay(pdMS_TO_TICKS(1800));
                            audio_hal_flush_rx();
                        }
                        ai_cloud_stop_session();
                        ei_wakeword_reset();
                        idle_slice_counter = 0;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        break;
#else
                        if (command_speech_started) {
                            ESP_LOGI(TAG, ">>> [TIMEOUT ROUTING] Hết thời gian câu lệnh cục bộ -> Chuyển tiếp sang Gemini Live! <<<");
                            if (sys_core_is_wifi_connected() && ai_cloud_start_turn()) {
                                if (cloud_prebuffer && cloud_prebuffer_len > 0) {
                                    ai_cloud_send_audio_chunk(cloud_prebuffer, cloud_prebuffer_len);
                                }
                                s_edge_state = AI_EDGE_STATE_STREAMING_SPEECH;
                                streaming_counter = 0;
                                silence_counter = 0;
                                s_consecutive_speech = 0;
                                speech_chunks_total = (cloud_prebuffer_len / (STREAM_CHUNK_SAMPLES * sizeof(int16_t)));
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
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                        responding_log_counter = 0;
                        break;
                    }

                    // Giới hạn an toàn: Nói tối đa 15 giây thì tự ngắt câu
                    if (streaming_counter >= (15 * 1000 / 60)) {
                        ESP_LOGW(TAG, "Đã đạt giới hạn tối đa 15 giây -> Tự động kết thúc câu hỏi!");
                        ei_wakeword_reset();
                        ai_cloud_end_turn();
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                        responding_log_counter = 0;
                        break;
                    }
                }
                break;
            }

            case AI_EDGE_STATE_AI_RESPONDING: {
                // Xả sạch DMA mic bằng các chunk nhỏ 60ms để âm thanh dội từ loa không bị tồn đọng
                size_t bytes_to_read = STREAM_CHUNK_SAMPLES * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(stream_chunk_buffer, bytes_to_read);
                (void)bytes_read;

                responding_log_counter++;
                if (responding_log_counter % 25 == 0) {
                    ESP_LOGD(TAG, "[AI_RESPONDING] Đang chờ phản hồi & phát âm thanh... (Cloud bận: %d, Loa phát: %d)",
                             ai_cloud_is_busy(), audio_hal_is_playing());
                }

                // Kiểm tra khi cả Cloud đã trả lời xong VÀ loa đã xả hết âm thanh
                if (!ai_cloud_is_busy() && !audio_hal_is_playing()) {
                    if (ai_cloud_is_follow_up_expected()) {
                        ai_cloud_clear_follow_up();
                        ESP_LOGI(TAG, ">>> [FOLLOW-UP ACTIVE] Gemini vừa hỏi xong -> Tự động mở Mic nhận phản hồi (3.6s) KHÔNG CẦN 'Hey Cherry' <<<");
                        vTaskDelay(pdMS_TO_TICKS(400)); // Đệm 400ms tiêu âm phòng, triệt tiêu hoàn toàn tiếng vọng của loa
                        audio_hal_flush_rx();
                        audio_vad_reset_trigger();
                        command_total_chunks = 0;
                        command_speech_started = false;
                        command_speech_chunks = 0;
                        command_silence_chunks = 0;
                        cloud_prebuffer_len = 0;
                        preroll_count = 0;
                        preroll_idx = 0;
                        wake_dma_flushed = true;
                        s_edge_state = AI_EDGE_STATE_COMMAND_LISTENING;
                    } else {
                        ESP_LOGI(TAG, ">>> AI phản hồi hoàn tất & loa đã nghỉ -> Đóng WebSocket & quay lại lắng nghe 'Hey Cherry' <<<");
                        ai_cloud_stop_session();
                        ei_wakeword_reset();
                        idle_slice_counter = 0;
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                    }
                }
                break;
            }
        }
    }

    if (wake_slice_buffer) free(wake_slice_buffer);
    if (stream_chunk_buffer) free(stream_chunk_buffer);
    if (command_full_buffer) free(command_full_buffer);
    if (stride_buffer) free(stride_buffer);
    if (cloud_prebuffer) free(cloud_prebuffer);
    if (debug_1_6s_buffer) free(debug_1_6s_buffer);
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

ai_edge_state_t ai_edge_get_state(void) {
    return s_edge_state;
}
