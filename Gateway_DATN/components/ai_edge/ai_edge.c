#include "ai_edge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "audio_hal.h"
#include "ai_cloud.h"
#include "sys_core.h"
#include "esp_heap_caps.h"
#include "wake_word_engine.h"
#include <string.h>

static const char *TAG = "ai_edge";

#define BOOT_BUTTON_GPIO       0
#define STREAM_CHUNK_SAMPLES   960  // 960 mẫu = 60ms tại 16kHz (2 frames 30ms của Espressif WebRTC VAD)
#define MIN_SPEECH_CHUNKS      5    // Cần nói ít nhất ~300ms câu lệnh trước khi xét ngắt câu (5 * 60ms = 300ms)
#define WAIT_SPEECH_MAX_CHUNKS 33   // 33 * 60ms ~ 2.0s: Cửa sổ chờ người dùng mở lời (No-Speech Guard)

static TaskHandle_t audio_task_handle = NULL;
static TaskHandle_t button_task_handle = NULL;
static volatile ai_edge_state_t s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;

// Biến chia sẻ để button task có thể reset bộ đếm khi kích hoạt
static volatile int s_wait_speech_timer = 0;
static volatile int s_consecutive_speech = 0;
static volatile int s_ding_settle_chunks = 0;
static volatile size_t s_prev_bytes = 0;

static void audio_recording_task(void *arg) {
    ESP_LOGI(TAG, "Audio Processing Task Started (Continuous 250ms + Hardware DMA 250ms).");

    // Khởi tạo engine Wake Word
    wake_word_init();

    // 1. Buffer cho lát cắt nhận diện Wake Word (4000 mẫu = 250ms = 8000 bytes)
    size_t slice_size = wake_word_get_slice_size();
    if (slice_size == 0) slice_size = 4000;
    int16_t *slice_buffer = (int16_t *)heap_caps_malloc(slice_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!slice_buffer) slice_buffer = (int16_t *)malloc(slice_size * sizeof(int16_t));

    // 2. Buffer cho streaming âm thanh lên Cloud khi đang nói (960 mẫu = 60ms tại 16kHz = 1920 bytes)
    int stream_chunk_size = STREAM_CHUNK_SAMPLES;
    int16_t *stream_chunk = (int16_t *)heap_caps_malloc(stream_chunk_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!stream_chunk) stream_chunk = (int16_t *)malloc(stream_chunk_size * sizeof(int16_t));

    // 3. Buffer lưu 1 chunk đệm trước khi nói (pre-roll 60ms) để không bị cắt phụ âm đầu
    int16_t *prev_chunk = (int16_t *)heap_caps_malloc(stream_chunk_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!prev_chunk) prev_chunk = (int16_t *)malloc(stream_chunk_size * sizeof(int16_t));

    int silence_counter = 0;
    int streaming_counter = 0;
    int speech_chunks_total = 0;
    int idle_slice_counter = 0;
    int responding_log_counter = 0;

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " HỆ THỐNG LẮNG NGHE ĐÃ SẴN SÀNG (REALTIME CONTINUOUS)!");
    ESP_LOGI(TAG, " Hãy nói 'Hey Cherry' để đánh thức trợ lý ảo!");
    ESP_LOGI(TAG, " (Hoặc bấm nút BOOT để kích hoạt / ngắt lời)");
    ESP_LOGI(TAG, "==================================================");

    while (1) {
        switch (s_edge_state) {
            case AI_EDGE_STATE_IDLE_WAKEWORD: {
                // Đọc lát cắt chuẩn 250ms (4000 mẫu) từ microphone DMA
                size_t bytes_to_read = slice_size * sizeof(int16_t);
                size_t bytes_read = audio_hal_record_chunk(slice_buffer, bytes_to_read);

                if (bytes_read == bytes_to_read) {
                    float confidence = 0.0f;
                    const char *label = NULL;

                    // Lọc DC offset & rung chấn với bộ lọc 90Hz (loại trừ ù quạt 50-80Hz, bảo toàn giọng người)
                    audio_hal_apply_dc_filter(slice_buffer, slice_size);

                    int rms = audio_vad_get_rms(slice_buffer, bytes_read / sizeof(int16_t));

                    // Cập nhật mức ồn nền môi trường bằng thuật toán cực tiểu Minimum Statistics
                    audio_dsp_update_ambient_noise(rms);

                    idle_slice_counter++;
                    // In nhịp tim định kỳ mỗi ~3 giây (12 lát cắt * 250ms)
                    if (idle_slice_counter % 12 == 0) {
                        ESP_LOGI(TAG, "[IDLE_WAKEWORD] Đang lắng nghe 'Hey Cherry'... (Mic RMS: %d, Sàn ồn: %d)",
                                 rms, audio_dsp_get_ambient_noise());
                    }

                    // Chạy mô hình Edge Impulse continuous trên lát cắt 250ms
                    bool detected = wake_word_process_slice(slice_buffer, slice_size, &confidence, &label);

                    // Kích hoạt ngay khi Edge Impulse xác nhận từ khóa thành công (đã bao gồm bộ lọc nhãn và margin)
                    if (detected) {
                        ESP_LOGI(TAG, "**************************************************");
                        ESP_LOGI(TAG, ">>> PHÁT HIỆN TỪ KHÓA 'HEY CHERRY'! (Score: %.1f%%, RMS: %d) <<<", confidence * 100.0f, rms);
                        ESP_LOGI(TAG, "**************************************************");

                        // 1. Kích hoạt kết nối WebSocket ngầm (Pre-warm) ngay lập tức trong lúc chuông kêu
                        ai_cloud_prewarm_session();

                        // 2. Phát tiếng chuông 'Ding!' duy nhất
                        audio_hal_play_wake_sound();

                        // 3. Chuyển sang Trạng thái 2: Chờ người dùng mở lời (No-Speech Guard 2.0s)
                        s_edge_state = AI_EDGE_STATE_WAIT_SPEECH;
                        s_wait_speech_timer = 0;
                        s_consecutive_speech = 0;
                        s_ding_settle_chunks = 4; // Đệm xả âm vang 240ms sau chuông
                        s_prev_bytes = 0;
                    }
                } else {
                    ESP_LOGW(TAG, "Mic read mismatch: %u != %u", (unsigned int)bytes_read, (unsigned int)bytes_to_read);
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
                break;
            }

            case AI_EDGE_STATE_WAIT_SPEECH: {
                // Đọc chunk 60ms từ mic (960 mẫu = 2 frames 30ms của WebRTC VAD)
                size_t bytes_read = audio_hal_record_chunk(stream_chunk, stream_chunk_size * sizeof(int16_t));
                if (bytes_read > 0) {
                    // Nếu tiếng chuông 'Ding' vẫn đang phát ra loa (~450ms):
                    // Bỏ qua VAD để tránh micro tự nghe thấy tiếng chuông của chính mình
                    if (audio_hal_is_wake_sound_playing()) {
                        s_consecutive_speech = 0;
                        s_ding_settle_chunks = 4;
                        break;
                    }

                    // Acoustic Settling Guard: Xả sạch âm vang dội của chuông Ding trong phòng (240ms) sau khi loa tắt
                    if (s_ding_settle_chunks > 0) {
                        s_ding_settle_chunks--;
                        s_consecutive_speech = 0;
                        break;
                    }

                    s_wait_speech_timer++;
                    int rms = 0;
                    bool has_voice = audio_vad_is_speech_chunk(stream_chunk, bytes_read / sizeof(int16_t), &rms);

                    if (has_voice) {
                        s_consecutive_speech++;
                    } else {
                        s_consecutive_speech = 0;
                        audio_dsp_update_ambient_noise(rms);
                    }

                    // In log giám sát định kỳ mỗi ~480ms (8 chunks) trong lúc chờ mở lời
                    if (s_wait_speech_timer % 8 == 0) {
                        ESP_LOGI(TAG, "[WAIT_SPEECH] Chờ mở lời... (Tick: %2d/%d | RMS: %3d | Floor: %d | %s)",
                                 s_wait_speech_timer, WAIT_SPEECH_MAX_CHUNKS, rms,
                                 audio_dsp_get_ambient_noise(),
                                 has_voice ? "CÓ TIẾNG" : "Im lặng ");
                    }

                    // TÌNH HUỐNG 1: Người dùng BẮT ĐẦU NÓI THẬT SỰ (>= 3 chunks ~180ms và RMS vượt trội hơn sàn ồn)
                    int min_start_speech_rms = audio_dsp_get_ambient_noise() + 35;
                    if (min_start_speech_rms < 90) min_start_speech_rms = 90;

                    if (s_consecutive_speech >= 3 && rms >= min_start_speech_rms) {
                        ESP_LOGI(TAG, ">>> [ESP VAD] Phát hiện người dùng bắt đầu nói (RMS: %d, Floor: %d) -> Mở Cloud Live API <<<",
                                 rms, audio_dsp_get_ambient_noise());
                        if (sys_core_is_wifi_connected() && ai_cloud_start_turn()) {
                            // Gửi chunk pre-roll đệm trước đó để bảo toàn âm đầu không bị mất
                            if (s_prev_bytes > 0 && prev_chunk) {
                                ai_cloud_send_audio_chunk((uint8_t *)prev_chunk, s_prev_bytes);
                            }
                            ai_cloud_send_audio_chunk((uint8_t *)stream_chunk, bytes_read);

                            s_edge_state = AI_EDGE_STATE_STREAMING_SPEECH;
                            streaming_counter = 0;
                            silence_counter = 0;
                            speech_chunks_total = s_consecutive_speech;
                        } else {
                            ESP_LOGE(TAG, "Lỗi kết nối Wi-Fi hoặc Cloud Live API!");
                            audio_hal_play_error_sound();
                            wake_word_reset();
                            s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        }
                        break;
                    }

                    // TÌNH HUỐNG 2: Hết 2.0 giây (33 chunks * 60ms ~ 2.0s) mà KHÔNG NÓI GÌ
                    if (s_wait_speech_timer >= WAIT_SPEECH_MAX_CHUNKS) {
                        ESP_LOGI(TAG, ">>> [NO-SPEECH GUARD] Không phát hiện tiếng nói sau 2.0s -> Hủy lượt & tiếp tục lắng nghe. <<<");
                        wake_word_reset();
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        break;
                    }

                    // Lưu chunk hiện tại làm đệm pre-roll cho tick tiếp theo
                    if (prev_chunk && bytes_read <= stream_chunk_size * sizeof(int16_t)) {
                        memcpy(prev_chunk, stream_chunk, bytes_read);
                        s_prev_bytes = bytes_read;
                    }
                }
                break;
            }

            case AI_EDGE_STATE_STREAMING_SPEECH: {
                // Đọc chunk 60ms từ mic (960 mẫu)
                size_t bytes_read = audio_hal_record_chunk(stream_chunk, stream_chunk_size * sizeof(int16_t));
                if (bytes_read > 0) {
                    int rms = 0;
                    bool has_voice = audio_vad_is_speech_chunk(stream_chunk, bytes_read / sizeof(int16_t), &rms);

                    // Gửi chunk PCM lên WebSocket RingBuffer
                    ai_cloud_send_audio_chunk((uint8_t *)stream_chunk, bytes_read);

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

                    // Dynamic Cutoff Window thích ứng:
                    // - Câu ngắn <= 1.8s: cần im lặng 12 chunks = 720ms
                    // - Câu dài > 1.8s: cần im lặng 16 chunks = 960ms
                    int silence_target = (speech_chunks_total <= 30) ? 12 : 16;

                    if (streaming_counter % 8 == 0) {
                        ESP_LOGI(TAG, "[ESP VAD] RMS: %4d (Floor: %2d) | %s | Silence: %2d/%d (Speech: %d)",
                                 rms, audio_dsp_get_ambient_noise(),
                                 has_voice ? "CÓ TIẾNG" : "Im lặng  ",
                                 silence_counter, silence_target, speech_chunks_total);
                    }

                    // TÌNH HUỐNG 1: NO-SPEECH / CANCEL GUARD (Hủy êm ái, giữ WebSocket, không kêu tít tít lỗi!)
                    if (speech_chunks_total < MIN_SPEECH_CHUNKS && silence_counter >= 12) {
                        ESP_LOGI(TAG, ">>> [STREAM NO-SPEECH] Không có câu lệnh hợp lệ (< 300ms nói, %d ms im lặng) -> Hủy lượt êm ái! <<<",
                                 silence_counter * 60);
                        wake_word_reset();
                        ai_cloud_cancel_turn();
                        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                        break;
                    }

                    // TÌNH HUỐNG 2: DỨT CÂU LỆNH HỢP LỆ
                    if (speech_chunks_total >= MIN_SPEECH_CHUNKS && silence_counter >= silence_target) {
                        ESP_LOGI(TAG, ">>> [ESP VAD] Phát hiện dứt câu (%d ms im lặng) -> Gửi chốt câu lệnh lên Gemini Live <<<",
                                 silence_counter * 60);
                        wake_word_reset();
                        ai_cloud_end_turn();
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                        responding_log_counter = 0;
                        break;
                    }

                    // Giới hạn an toàn: Nói tối đa 15 giây thì tự ngắt câu
                    if (streaming_counter >= (15 * 1000 / 60)) {
                        ESP_LOGW(TAG, "Đã đạt giới hạn tối đa 15 giây -> Tự động kết thúc câu lệnh!");
                        wake_word_reset();
                        ai_cloud_end_turn();
                        s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                        responding_log_counter = 0;
                        break;
                    }
                }
                break;
            }

            case AI_EDGE_STATE_AI_RESPONDING: {
                // Xả sạch DMA mic bằng các chunk nhỏ 60ms để âm thanh dội từ loa không bị tích tụ
                size_t bytes_read = audio_hal_record_chunk(stream_chunk, stream_chunk_size * sizeof(int16_t));
                (void)bytes_read;

                responding_log_counter++;
                if (responding_log_counter % 25 == 0) {
                    ESP_LOGD(TAG, "[AI_RESPONDING] Đang chờ phản hồi & phát âm thanh... (Cloud bận: %d, Loa phát: %d)",
                             ai_cloud_is_busy(), audio_hal_is_playing());
                }

                // Kiểm tra khi cả Cloud đã trả lời xong VÀ loa đã xả hết âm thanh
                if (!ai_cloud_is_busy() && !audio_hal_is_playing()) {
                    ESP_LOGI(TAG, ">>> AI phản hồi hoàn tất & loa đã nghỉ -> Quay lại lắng nghe 'Hey Cherry' <<<");
                    wake_word_reset();
                    idle_slice_counter = 0;
                    s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
                }

                vTaskDelay(pdMS_TO_TICKS(10));
                break;
            }
        }
    }

    if (slice_buffer) free(slice_buffer);
    if (stream_chunk) free(stream_chunk);
    if (prev_chunk) free(prev_chunk);
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
                                audio_hal_play_wake_sound();
                                s_wait_speech_timer = 0;
                                s_consecutive_speech = 0;
                                s_ding_settle_chunks = 4;
                                s_prev_bytes = 0;
                                s_edge_state = AI_EDGE_STATE_WAIT_SPEECH;
                            } else {
                                ESP_LOGE(TAG, "Chưa kết nối Wi-Fi!");
                                audio_hal_play_error_sound();
                            }
                            break;
                        }

                        case AI_EDGE_STATE_WAIT_SPEECH:
                        case AI_EDGE_STATE_STREAMING_SPEECH: {
                            ESP_LOGI(TAG, ">>> BOOT BUTTON CLICK: CHỐT CÂU LỆNH NGAY LẬP TỨC <<<");
                            wake_word_reset();
                            ai_cloud_end_turn();
                            s_edge_state = AI_EDGE_STATE_AI_RESPONDING;
                            break;
                        }

                        case AI_EDGE_STATE_AI_RESPONDING: {
                            ESP_LOGI(TAG, ">>> BOOT BUTTON CLICK: BARGE-IN NGẮT LỜI AI <<<");
                            audio_hal_stop_playback();
                            ai_cloud_abort();
                            audio_hal_play_wake_sound();
                            s_wait_speech_timer = 0;
                            s_consecutive_speech = 0;
                            s_ding_settle_chunks = 4;
                            s_prev_bytes = 0;
                            s_edge_state = AI_EDGE_STATE_WAIT_SPEECH;
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
    ESP_LOGI(TAG, "Initializing Edge AI (Realtime Continuous 250ms + Pre-Gain 3.5x)");
    
    xTaskCreatePinnedToCore(button_trigger_task, "button_task", 4096, NULL, 5, &button_task_handle, 1);
    // Stack 16KB đảm bảo an toàn tuyệt đối cho TFLite Micro inference
    xTaskCreatePinnedToCore(audio_recording_task, "audio_task", 16384, NULL, 5, &audio_task_handle, 1);
}

void ai_edge_stop_recording(void) {
    if (s_edge_state != AI_EDGE_STATE_IDLE_WAKEWORD) {
        ESP_LOGI(TAG, "Dừng ghi âm khẩn cấp (Hủy bởi Cloud/Socket Error).");
        wake_word_reset();
        s_edge_state = AI_EDGE_STATE_IDLE_WAKEWORD;
    }
}

bool ai_edge_is_recording(void) {
    return (s_edge_state == AI_EDGE_STATE_WAIT_SPEECH || s_edge_state == AI_EDGE_STATE_STREAMING_SPEECH);
}

ai_edge_state_t ai_edge_get_state(void) {
    return s_edge_state;
}
