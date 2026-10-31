#include "audio_hal.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_heap_caps.h"
#include "wake_sound.h"
#include "esp_vad.h"

static const char *TAG = "audio_hal";

static RingbufHandle_t tx_ringbuf = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;
static vad_handle_t s_vad_inst = NULL;
static volatile bool s_flush_requested = false;
volatile bool is_buffering = false;
volatile uint32_t buffered_bytes = 0;
static volatile uint32_t buffer_target_bytes = 28800; // 600ms mặc định lúc đầu
static volatile bool is_turn_active = false;

void audio_hal_set_turn_active(bool active) {
    is_turn_active = active;
}

#include "error_sound.h"

// Gọi hàm này từ ai_cloud để ngắt mạng nếu cần
extern void ai_cloud_abort(void);
extern void ai_cloud_on_playback_idle(void);

static volatile bool s_speaker_active = false;
static volatile bool s_is_wake_sound_playing = false;
static TickType_t s_wake_sound_start_tick = 0;
static bool s_idle_notified = false;

static void audio_playback_task(void *arg) {
    size_t item_size;
    bool is_i2s_enabled = true; // Started enabled in init
    uint32_t idle_time_ms = 0;

    while (1) {
        // Nếu có yêu cầu xả đệm (Barge-In hoặc đổi lượt):
        // Chỉ duy nhất task này được đọc và xả ringbuf để đảm bảo an toàn tuyệt đối (Single-Reader Thread Safety)
        if (s_flush_requested) {
            s_flush_requested = false;
            size_t dummy_sz;
            uint8_t *dummy_it;
            while ((dummy_it = (uint8_t *)xRingbufferReceiveUpTo(tx_ringbuf, &dummy_sz, 0, 8192)) != NULL) {
                vRingbufferReturnItem(tx_ringbuf, (void *)dummy_it);
            }
            buffered_bytes = 0;
        }

        if (is_buffering) {
            // Trong lúc gom đệm, bơm Zero-Padding để giữ loa câm tĩnh lặng, triệt tiêu tiếng rè/dè
            if (is_i2s_enabled) {
                static const uint8_t s_silence_buf[512] = {0};
                size_t written = 0;
                i2s_channel_write(tx_handle, s_silence_buf, sizeof(s_silence_buf), &written, 0);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            // Thoát buffering nếu đủ target bytes HOẶC nếu câu trả lời ngắn đã kết thúc (!is_turn_active)
            if (buffered_bytes >= buffer_target_bytes || !is_turn_active) {
                is_buffering = false;
                ESP_LOGI(TAG, "Jitter buffer filled (%lu bytes), starting playback.", buffered_bytes);
            }
            continue;
        }

        // Nhận từng khối nhỏ (tối đa 4096 bytes ~ 85ms âm thanh) bằng xRingbufferReceiveUpTo.
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(tx_ringbuf, &item_size, pdMS_TO_TICKS(20), 4096);
        if (item != NULL) {
            idle_time_ms = 0;
            s_idle_notified = false;
            s_speaker_active = true;
            if (!is_i2s_enabled) {
                esp_err_t err = i2s_channel_enable(tx_handle);
                if (err == ESP_OK) {
                    is_i2s_enabled = true;
                    ESP_LOGI(TAG, "I2S TX Enabled (Resumed playback).");
                }
            }
            size_t written = 0;
            i2s_channel_write(tx_handle, item, item_size, &written, portMAX_DELAY);
            vRingbufferReturnItem(tx_ringbuf, (void *)item);
        } else {
            s_speaker_active = false;
            idle_time_ms += 20;

            if (is_turn_active) {
                // ĐANG TRONG LƯỢT NÓI mà buffer tạm hết dữ liệu trong tích tắc:
                // Để phần cứng DMA (200ms) tự điều tiết êm ái, TUYỆT ĐỐI KHÔNG chèn âm lặng làm rách sóng âm!
            } else {
                // MODEL ĐÃ NÓI XONG HOẶC ÂM THANH CỤC BỘ (Chuông Ding, lỗi) ĐÃ PHÁT XONG:
                // Bơm Zero-Padding liên tục vào I2S DMA để giữ MAX98357A câm lặng tuyệt đối (0V differential),
                // chống rè rỗng DMA và giữ cho chip công suất luôn ở trạng thái nóng (Zero Startup Latency).
                if (is_i2s_enabled) {
                    static const uint8_t s_silence_buf[512] = {0};
                    size_t written = 0;
                    i2s_channel_write(tx_handle, s_silence_buf, sizeof(s_silence_buf), &written, 0);
                }

                // Sau khi phát xong chuông Ding gọn (~240ms): đệm 30ms âm lặng là xác nhận xong chuông Ding
                if (s_is_wake_sound_playing && idle_time_ms >= 30) {
                    s_is_wake_sound_playing = false; // Chuông Ding đã ngân xong hoàn toàn!
                }

                if (idle_time_ms >= 400 && !s_idle_notified) {
                    s_idle_notified = true;
                    s_is_wake_sound_playing = false;
                    ai_cloud_on_playback_idle(); // Báo cho ai_cloud biết âm thanh đã phát xong hoàn toàn (1 lần)
                }
            }
        }
    }
}

#define SAMPLE_RATE 16000
#define VAD_THRESHOLD 280 // Ngưỡng năng lượng để nhận diện giọng nói (Điều chỉnh theo độ nhạy mic INMP441)

void audio_hal_init(void) {
    ESP_LOGI(TAG, "Initializing Audio HAL (I2S)");

    // --- FROM init_microphone() ---
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    rx_chan_cfg.dma_desc_num = 16;
    rx_chan_cfg.dma_frame_num = 1000; // 16 * 1000 = 16000 samples = 1.0s hardware DMA buffer chống tràn khi chạy inference!
    ESP_ERROR_CHECK(i2s_new_channel(&rx_chan_cfg, NULL, &rx_handle));

    i2s_std_config_t rx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = 4,
            .ws = 5,
            .dout = I2S_GPIO_UNUSED,
            .din = 6,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    rx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &rx_std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    ESP_LOGI(TAG, "Microphone initialized.");

    // --- FROM init_speaker() ---
    i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    tx_chan_cfg.dma_desc_num = 10;
    tx_chan_cfg.dma_frame_num = 480; // 10 * 480 = 4800 samples = 200ms hardware DMA buffer cushion
    tx_chan_cfg.auto_clear = true;   // Tự động clear buffer xuất số 0 khi cạn dữ liệu, triệt tiêu 100% tiếng rè cơ học!
    esp_err_t err = i2s_new_channel(&tx_chan_cfg, &tx_handle, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create TX channel: %s", esp_err_to_name(err));
        return;
    }

    i2s_std_config_t tx_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = 15,
            .ws = 16,
            .dout = 17,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    tx_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    
    err = i2s_channel_init_std_mode(tx_handle, &tx_std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TX channel: %s", esp_err_to_name(err));
        return;
    }
    
    err = i2s_channel_enable(tx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable TX channel: %s", esp_err_to_name(err));
        return;
    }
    
    // Tạo RingBuffer 1.5MB (1536KB) trong PSRAM để hấp thụ toàn bộ các câu trả lời dài từ Gemini Live
    tx_ringbuf = xRingbufferCreateWithCaps(1536 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (tx_ringbuf == NULL) {
        ESP_LOGW(TAG, "Failed to create 1.5MB PSRAM ringbuf. Fallback to 1MB...");
        tx_ringbuf = xRingbufferCreateWithCaps(1024 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
        if (tx_ringbuf == NULL) return;
    }
    xTaskCreate(audio_playback_task, "audio_play_task", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "Speaker initialized with 1.5MB RingBuffer in PSRAM.");

    // Khởi tạo Espressif WebRTC VAD với Trigger tích hợp (Phương án A)
    // Mode 3 (Very Aggressive), 16000Hz, frame 30ms, min_speech = 60ms (nhạy hơn, bắt âm sớm), min_noise = 600ms
    s_vad_inst = vad_create_with_param(VAD_MODE_3, 16000, 30, 60, 600);
    if (s_vad_inst) {
        ESP_LOGI(TAG, "Espressif Hardware VAD Engine initialized (Mode 3, Trigger: 60ms speech, 600ms noise).");
    } else {
        ESP_LOGE(TAG, "Failed to initialize Espressif VAD Engine!");
    }
}

size_t audio_hal_record_chunk(int16_t *buffer, size_t buffer_size_bytes) {
    if (!rx_handle || !buffer || buffer_size_bytes == 0) return 0;

    size_t total_bytes_read = 0;
    uint8_t *dest = (uint8_t *)buffer;

    while (total_bytes_read < buffer_size_bytes) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(rx_handle, dest + total_bytes_read,
                                         buffer_size_bytes - total_bytes_read,
                                         &bytes_read, pdMS_TO_TICKS(500));
        if (err != ESP_OK || bytes_read == 0) {
            break;
        }
        total_bytes_read += bytes_read;
    }
    return total_bytes_read;
}

size_t audio_hal_play_chunk(const uint8_t *buffer, size_t buffer_size_bytes) {
    if (tx_ringbuf == NULL) return 0;
    
    // Timeout 1500ms: Nếu buffer tạm thời đầy, hàm sẽ đợi loa xả bớt thay vì drop mất chữ
    if (xRingbufferSend(tx_ringbuf, (void *)buffer, buffer_size_bytes, pdMS_TO_TICKS(1500)) == pdTRUE) {
        buffered_bytes += buffer_size_bytes;
        return buffer_size_bytes;
    } else {
        ESP_LOGW(TAG, "Audio Ringbuffer full, dropped %d bytes", buffer_size_bytes);
        return 0;
    }
}

// Biến trạng thái bộ lọc Biquad High-Pass bậc 2 (Direct Form II Transposed)
static float s_biquad_w1 = 0.0f;
static float s_biquad_w2 = 0.0f;

void audio_hal_apply_dc_filter(int16_t *buffer, size_t num_samples) {
    if (!buffer || num_samples == 0) return;

    // Biquad High-Pass Filter (Butterworth bậc 2, fc = 90Hz @ fs = 16000Hz, Q = 0.7071)
    // Hệ số chuẩn hóa: Triệt tiêu 100% DC offset và suy hao sâu tiếng quạt/ù điện 50-80Hz, bảo toàn 100% âm vực giọng nói (100Hz - 7600Hz)
    const float b0 =  0.97531843f;
    const float b1 = -1.95063686f;
    const float b2 =  0.97531843f;
    const float a1 = -1.95002759f;
    const float a2 =  0.95124614f;

    float w1 = s_biquad_w1;
    float w2 = s_biquad_w2;

    for (size_t i = 0; i < num_samples; i++) {
        float x = (float)buffer[i];
        // Cấu trúc Direct Form II Transposed (tối ưu tính ổn định số học và chống tràn)
        float y = b0 * x + w1;
        w1 = b1 * x - a1 * y + w2;
        w2 = b2 * x - a2 * y;

        if (y > 32767.0f) y = 32767.0f;
        else if (y < -32768.0f) y = -32768.0f;
        buffer[i] = (int16_t)y;
    }

    s_biquad_w1 = w1;
    s_biquad_w2 = w2;
}

int audio_vad_get_rms(const int16_t *buffer, size_t num_samples) {
    if (num_samples == 0) return 0;
    
    int64_t sum_squares = 0;
    for (size_t i = 0; i < num_samples; i++) {
        sum_squares += ((int64_t)buffer[i] * buffer[i]);
    }
    
    return (int)sqrt((double)sum_squares / num_samples);
}

// Thuật toán theo dõi mức nhiễu nền chuẩn (Voice-Immune Noise Floor Tracker):
// Sử dụng biến số thực (float) để triệt tiêu hoàn toàn lỗi làm tròn số nguyên (Integer Truncation Trap).
// Tiếng nói người (RMS lớn < 3s) tuyệt đối không được phép làm tăng sàn ồn.
static volatile float s_ambient_noise_float = 35.0f;
static volatile int s_elevated_noise_counter = 0;

void audio_dsp_update_ambient_noise(int current_rms) {
    if (current_rms <= 0) return;

    float rms_f = (float)current_rms;

    if (rms_f < s_ambient_noise_float) {
        // Môi trường yên tĩnh hơn: Thích ứng giảm nhanh (~8 frames = 2s)
        s_ambient_noise_float = s_ambient_noise_float * 0.85f + rms_f * 0.15f;
        s_elevated_noise_counter = 0;
    } else {
        // Âm thanh lớn hơn sàn ồn hiện tại:
        if (rms_f <= s_ambient_noise_float * 1.8f) {
            // Âm lượng tăng nhẹ (nhiễu nền tĩnh như tiếng quạt gió thoang thoảng):
            // Thích ứng tăng mượt mà với số thực (không bao giờ bị kẹt số nguyên)
            s_ambient_noise_float = s_ambient_noise_float * 0.985f + rms_f * 0.015f;
            s_elevated_noise_counter = 0;
        } else {
            // Âm lượng vọt cao > 1.8x (đặc trưng của tiếng người nói / ho / gõ bàn):
            s_elevated_noise_counter++;
            // Nếu âm thanh duy trì cao liên tục trên 3.0 giây (khoảng 50 chunks):
            // Đây là thiết bị ồn mới bật (như quạt số lớn hoặc điều hòa), cho phép sàn ồn bám theo
            if (s_elevated_noise_counter > 50) {
                s_ambient_noise_float = s_ambient_noise_float * 0.99f + rms_f * 0.01f;
            }
            // Nếu là tiếng nói bình thường (< 3s): BỎ QUA, không cho tiếng nói kéo sàn ồn lên!
        }
    }

    if (s_ambient_noise_float < 15.0f) s_ambient_noise_float = 15.0f;
    if (s_ambient_noise_float > 180.0f) s_ambient_noise_float = 180.0f;
}

int audio_dsp_get_ambient_noise(void) {
    return (int)(s_ambient_noise_float + 0.5f);
}

// Alias tương thích ngược
void audio_vad_update_noise_floor(int current_rms) {
    audio_dsp_update_ambient_noise(current_rms);
}

int audio_vad_get_noise_floor(void) {
    return audio_dsp_get_ambient_noise();
}

bool audio_vad_is_speech_chunk(int16_t *buffer, size_t num_samples, int *out_rms) {
    if (!buffer || num_samples == 0) return false;

    // 1. Áp dụng bộ lọc Biquad High-Pass bậc 2 (~90Hz) để triệt tiêu DC bias và tiếng ù quạt/rung chấn
    audio_hal_apply_dc_filter(buffer, num_samples);

    // 2. Tính RMS năng lượng âm thanh sạch sau lọc
    int rms = audio_vad_get_rms(buffer, num_samples);
    if (out_rms) *out_rms = rms;

    // 3. Nếu chưa khởi tạo esp_vad thì fallback bằng adaptive RMS
    if (!s_vad_inst) {
        int threshold = audio_dsp_get_ambient_noise() + 50;
        return (rms >= threshold);
    }

    // 4. Phân tích qua Espressif WebRTC VAD Engine với Trigger tích hợp (Mode 3)
    // Chia chunk thành các khung 30ms (480 mẫu @ 16kHz)
    bool speech_detected = false;
    const size_t frame_len = 480; // 30ms

    for (size_t offset = 0; offset + frame_len <= num_samples; offset += frame_len) {
        vad_state_t st = vad_process_with_trigger(s_vad_inst, buffer + offset);
        if (st == VAD_SPEECH) {
            speech_detected = true;
            break;
        }
    }

    // 5. Điều kiện kết hợp:
    // - VAD Trigger Mode 3 xác nhận có phổ giọng nói người liên tục >= 120ms
    // - Năng lượng RMS sạch vượt ngưỡng sàn ồn môi trường ít nhất +20 RMS (hoặc tối thiểu 45 RMS)
    int min_speech_rms = audio_dsp_get_ambient_noise() + 20;
    if (min_speech_rms < 45) min_speech_rms = 45;

    return (speech_detected && rms >= min_speech_rms);
}

void audio_vad_reset_trigger(void) {
    if (s_vad_inst) {
        vad_reset_trigger(s_vad_inst);
    }
}

bool audio_vad_is_speech_adaptive(const int16_t *buffer, size_t num_samples, bool is_already_speaking) {
    int rms = 0;
    return audio_vad_is_speech_chunk((int16_t *)buffer, num_samples, &rms);
}

bool audio_vad_is_speech(const int16_t *buffer, size_t num_samples) {
    return audio_vad_is_speech_chunk((int16_t *)buffer, num_samples, NULL);
}

void audio_hal_play_wake_sound(void) {
    uint32_t total_ding_ms = (wake_sound_len * 1000) / (24000 * 2);
    ESP_LOGI(TAG, "Playing wake sound (Full Ding-ding %lu ms)...", (unsigned long)total_ding_ms);
    is_turn_active = false;
    s_is_wake_sound_playing = true;
    s_wake_sound_start_tick = xTaskGetTickCount();
    
    // Phát trọn vẹn toàn bộ chuông Ding-ding gốc (21.600 bytes ~ 450ms ở 24kHz)
    audio_hal_play_chunk(wake_sound_pcm, wake_sound_len);
}

bool audio_hal_is_wake_recording_ready(void) {
    if (!s_is_wake_sound_playing) return true;
    TickType_t elapsed_ticks = xTaskGetTickCount() - s_wake_sound_start_tick;
    uint32_t elapsed_ms = (uint32_t)(elapsed_ticks * portTICK_PERIOD_MS);
    uint32_t total_ding_ms = (wake_sound_len * 1000) / (24000 * sizeof(int16_t));
    return (elapsed_ms >= total_ding_ms + 50); // Đợi dứt chuông 100% + 50ms tiêu âm phòng
}

void audio_hal_flush_rx(void) {
    if (!rx_handle) return;
    int16_t dummy[256];
    size_t bytes_read = 0;
    // Đọc xả sạch bộ đệm DMA RX cho đến khi rỗng hoàn toàn (timeout 0)
    while (i2s_channel_read(rx_handle, dummy, sizeof(dummy), &bytes_read, 0) == ESP_OK && bytes_read > 0) {
        // Xả sạch dữ liệu cũ
    }
}

void audio_hal_start_buffering(void) {
    is_buffering = true;
    buffered_bytes = 0;
    buffer_target_bytes = 28800; // 600ms High Watermark (24000Hz * 2 bytes * 0.6s)
    is_turn_active = true;
    s_flush_requested = false;
    ESP_LOGI(TAG, "Started Jitter Buffering (600ms)...");
}

void audio_hal_force_start_playing(void) {
    if (is_buffering) {
        is_buffering = false;
        ESP_LOGI(TAG, "Forced start playing (Jitter buffer skipped/finished).");
    }
}

void audio_hal_flush_tx(void) {
    s_flush_requested = true;
    int timeout = 6; // Đợi tối đa 60ms để luồng audio_playback_task xả sạch đệm
    while (s_flush_requested && timeout-- > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void audio_hal_play_error_sound(void) {
    ESP_LOGI(TAG, "Playing error sound...");
    is_turn_active = false;
    is_buffering = false;
    audio_hal_flush_tx(); // Xả sạch âm thanh cũ đồng bộ trước khi phát tiếng báo lỗi
    
    audio_hal_play_chunk(error_sound_pcm, error_sound_len);
    vTaskDelay(pdMS_TO_TICKS(error_sound_len / 48 + 50));
}

void audio_hal_stop_playback(void) {
    if (s_is_wake_sound_playing) {
        // Bảo vệ tiếng chuông Ding-ding luôn được phát trọn vẹn, không bị ngắt giữa chừng
        return;
    }
    ESP_LOGI(TAG, "Stopping playback immediately & flushing ringbuffer (Barge-In).");
    is_turn_active = false;
    is_buffering = false;
    s_speaker_active = false;
    s_flush_requested = true; // Yêu cầu luồng playback tự xả sạch an toàn
}

bool audio_hal_is_wake_sound_playing(void) {
    return s_is_wake_sound_playing;
}

bool audio_hal_is_playing(void) {
    return is_turn_active || s_speaker_active || s_is_wake_sound_playing;
}

// [DEBUG TEST] Phát lại âm thanh Model 2 vừa thu ra loa (resample 16kHz -> 24kHz + Auto Boost Target ~26.000)
void audio_hal_debug_play_captured_audio(const int16_t *audio_16k, size_t num_samples) {
    if (!audio_16k || num_samples == 0 || tx_ringbuf == NULL) return;

    // 1. Quét tìm đỉnh biên độ âm thanh thực tế
    int16_t raw_peak = 0;
    for (size_t i = 0; i < num_samples; i++) {
        int16_t v = abs(audio_16k[i]);
        if (v > raw_peak) raw_peak = v;
    }

    // 2. Tính hệ số khuếch đại (Target Peak ~26.000) để phát ra loa to rõ ràng ngang mức chuông Ding
    float gain = 1.0f;
    if (raw_peak > 50) {
        gain = 26000.0f / (float)raw_peak;
        if (gain > 15.0f) gain = 15.0f; // Max boost 15x (+23.5 dB)
    }

    ESP_LOGW(TAG, ">>> [DEBUG TEST] Phát lại %d mẫu (%.1fs @ 16kHz) -> Loa 24kHz | Peak: %d, Boost: %.2fx (Target ~26000) <<<",
             (int)num_samples, (float)num_samples / 16000.0f, raw_peak, gain);

    // 3. Resample 16kHz mono -> 24kHz mono (tỉ lệ 2:3) để khớp xung nhịp loa MAX98357A (24000Hz)
    size_t out_samples = (num_samples * 3) / 2;
    int16_t *resampled = (int16_t *)heap_caps_malloc(out_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!resampled) return;

    for (size_t k = 0; k < out_samples; k++) {
        size_t src_idx = (k * 2) / 3;
        int frac = (k * 2) % 3;
        int16_t s0 = audio_16k[src_idx];
        int16_t s1 = (src_idx + 1 < num_samples) ? audio_16k[src_idx + 1] : s0;
        int32_t val = s0 + ((int32_t)(s1 - s0) * frac) / 3;
        val = (int32_t)(val * gain);
        if (val > 30000) val = 30000;
        else if (val < -30000) val = -30000;
        resampled[k] = (int16_t)val;
    }

    // 4. Phát ra loa: Đánh dấu không phải cloud turn để luồng playback phát ngay lập tức
    is_turn_active = false;
    is_buffering = false;

    // Đẩy từng khối nhỏ 4096 bytes vào RingBuffer để phát trơn tru
    size_t bytes_to_send = out_samples * sizeof(int16_t);
    const uint8_t *p = (const uint8_t *)resampled;
    while (bytes_to_send > 0) {
        size_t chunk = (bytes_to_send > 4096) ? 4096 : bytes_to_send;
        audio_hal_play_chunk(p, chunk);
        p += chunk;
        bytes_to_send -= chunk;
    }

    free(resampled);
}


