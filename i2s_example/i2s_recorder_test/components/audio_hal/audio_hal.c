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

static const char *TAG = "audio_hal";

static RingbufHandle_t tx_ringbuf = NULL;
static i2s_chan_handle_t rx_handle = NULL;
static i2s_chan_handle_t tx_handle = NULL;
volatile bool force_disable_flag = false;
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

static void audio_playback_task(void *arg) {
    size_t item_size;
    bool is_i2s_enabled = true; // Started enabled in init
    uint32_t idle_time_ms = 0;

    while (1) {
        if (is_buffering) {
            // Trong lúc gom đệm, nếu I2S đang bật thì bơm Zero-Padding để giữ loa câm tĩnh lặng, triệt tiêu tiếng rè/dè
            if (is_i2s_enabled) {
                static const uint8_t s_silence_buf[512] = {0};
                size_t written = 0;
                i2s_channel_write(tx_handle, s_silence_buf, sizeof(s_silence_buf), &written, 0);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            if (buffered_bytes >= buffer_target_bytes) {
                is_buffering = false;
                ESP_LOGI(TAG, "Jitter buffer filled (%lu bytes), starting playback.", buffered_bytes);
            }
            continue;
        }

        // Nhận từng khối nhỏ (tối đa 4096 bytes ~ 85ms âm thanh) bằng xRingbufferReceiveUpTo.
        // Cực kỳ quan trọng: giải phóng bộ nhớ cho RingBuffer liên tục sau mỗi 85ms,
        // thay vì giữ chặt cả khối khổng lồ hàng trăm KB khiến xRingbufferSend bị timeout gây rớt gói audio!
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(tx_ringbuf, &item_size, pdMS_TO_TICKS(20), 4096);
        if (item != NULL) {
            idle_time_ms = 0;
            if (!is_i2s_enabled) {
                // An toàn tuyệt đối: chỉ bật lại I2S từ chính task này
                esp_err_t err = i2s_channel_enable(tx_handle);
                if (err == ESP_OK) {
                    is_i2s_enabled = true;
                    ESP_LOGI(TAG, "I2S TX Enabled (Resumed playback).");
                }
            }
            size_t written = 0;
            if (!force_disable_flag) {
                i2s_channel_write(tx_handle, item, item_size, &written, portMAX_DELAY);
            }
            vRingbufferReturnItem(tx_ringbuf, (void *)item);
        } else {
            idle_time_ms += 20;
            if (is_turn_active) {
                // ĐANG TRONG LƯỢT NÓI mà buffer tạm hết dữ liệu:
                // Bơm Zero-Padding liên tục vào I2S DMA để giữ MAX98357A câm lặng tuyệt đối, triệt tiêu tiếng rè/dè!
                // Khi chunk tiếp theo từ Google tới, xRingbufferReceiveUpTo sẽ lấy và phát tiếp NGAY LẬP TỨC mà không bị khựng.
                if (is_i2s_enabled) {
                    static const uint8_t s_silence_buf[512] = {0};
                    size_t written = 0;
                    i2s_channel_write(tx_handle, s_silence_buf, sizeof(s_silence_buf), &written, 0);
                }
            } else {
                // MODEL ĐÃ NÓI XONG (!is_turn_active):
                // Chờ buffer xả hết và im lặng quá 1000ms thì mới tắt I2S để tiết kiệm điện & chống nhiễu mic
                if (force_disable_flag || idle_time_ms >= 1000) {
                    force_disable_flag = false;
                    idle_time_ms = 0; // Reset về 0 để tránh tràn biến và chỉ chạy đoạn này khi cần
                    if (is_i2s_enabled) {
                        ESP_LOGI(TAG, "I2S TX Idle/Forced. Disabling channel to prevent RF noise.");
                        i2s_channel_disable(tx_handle);
                        is_i2s_enabled = false;
                        ai_cloud_on_playback_idle(); // Báo cho ai_cloud biết âm thanh đã phát xong hoàn toàn
                    }
                }
            }
        }
    }
}

#define SAMPLE_RATE 16000
#define VAD_THRESHOLD 1200 // Ngưỡng năng lượng để nhận diện giọng nói (Có thể tinh chỉnh cao hơn nếu môi trường ồn)

void audio_hal_init(void) {
    ESP_LOGI(TAG, "Initializing Audio HAL (I2S)");

    // --- FROM init_microphone() ---
    i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
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
    
    // Tạo RingBuffer 1MB (1024KB) trong PSRAM để hấp thụ toàn bộ các câu trả lời dài từ Gemini Live
    tx_ringbuf = xRingbufferCreateWithCaps(1024 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (tx_ringbuf == NULL) {
        ESP_LOGW(TAG, "Failed to create 1MB PSRAM ringbuf. Fallback to 512KB...");
        tx_ringbuf = xRingbufferCreateWithCaps(512 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
        if (tx_ringbuf == NULL) return;
    }
    xTaskCreate(audio_playback_task, "audio_play_task", 4096, NULL, 5, NULL);
    
    ESP_LOGI(TAG, "Speaker initialized with 1MB RingBuffer in PSRAM.");
}

size_t audio_hal_record_chunk(int16_t *buffer, size_t buffer_size_bytes) {
    size_t bytes_read = 0;
    if (i2s_channel_read(rx_handle, buffer, buffer_size_bytes, &bytes_read, 1000) == ESP_OK) {
        return bytes_read;
    }
    return 0;
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

bool audio_vad_is_speech(const int16_t *buffer, size_t num_samples) {
    if (num_samples == 0) return false;
    
    int64_t sum_squares = 0;
    for (size_t i = 0; i < num_samples; i++) {
        sum_squares += ((int64_t)buffer[i] * buffer[i]);
    }
    
    double rms = sqrt((double)sum_squares / num_samples);
    return (rms > VAD_THRESHOLD);
}

extern volatile bool force_disable_flag;

void audio_hal_play_wake_sound(void) {
    ESP_LOGI(TAG, "Playing wake sound...");
    is_turn_active = false;
    
    // Ép thoát trạng thái buffering nếu bị kẹt
    audio_hal_force_start_playing();

    // Xóa sạch buffer cũ để tránh âm thanh thừa
    size_t dummy_size;
    uint8_t *dummy_item;
    while ((dummy_item = (uint8_t *)xRingbufferReceive(tx_ringbuf, &dummy_size, 0)) != NULL) {
        vRingbufferReturnItem(tx_ringbuf, (void *)dummy_item);
    }

    audio_hal_play_chunk(wake_sound_pcm, wake_sound_len);
    
    // Tính toán thời gian delay dựa trên độ dài PCM (24000Hz 16-bit = 48 bytes/ms)
    vTaskDelay(pdMS_TO_TICKS(wake_sound_len / 48 + 50));
    
    // Force idle after playing
    force_disable_flag = true;
}

void audio_hal_start_buffering(void) {
    is_buffering = true;
    buffered_bytes = 0;
    buffer_target_bytes = 28800; // 600ms High Watermark (24000Hz * 2 bytes * 0.6s)
    is_turn_active = true;
    ESP_LOGI(TAG, "Started Jitter Buffering (600ms)...");
}

void audio_hal_force_start_playing(void) {
    if (is_buffering) {
        is_buffering = false;
        ESP_LOGI(TAG, "Forced start playing (Jitter buffer skipped/finished).");
    }
}

void audio_hal_play_error_sound(void) {
    ESP_LOGI(TAG, "Playing error sound...");
    is_turn_active = false;
    
    // Ép thoát trạng thái buffering nếu bị kẹt do rớt mạng giữa chừng
    audio_hal_force_start_playing();

    // Xóa sạch buffer cũ để tránh lặp âm thanh
    size_t dummy_size;
    uint8_t *dummy_item;
    while ((dummy_item = (uint8_t *)xRingbufferReceive(tx_ringbuf, &dummy_size, 0)) != NULL) {
        vRingbufferReturnItem(tx_ringbuf, (void *)dummy_item);
    }
    
    audio_hal_play_chunk(error_sound_pcm, error_sound_len);
    
    // Tính toán thời gian delay chính xác dựa trên độ dài PCM
    vTaskDelay(pdMS_TO_TICKS(error_sound_len / 48 + 50));
    
    force_disable_flag = true;
}

void audio_hal_stop_playback(void) {
    ESP_LOGI(TAG, "Stopping playback immediately & flushing ringbuffer (Barge-In).");
    is_turn_active = false;
    is_buffering = false;
    if (tx_ringbuf != NULL) {
        size_t dummy_size;
        uint8_t *dummy_item;
        while ((dummy_item = (uint8_t *)xRingbufferReceive(tx_ringbuf, &dummy_size, 0)) != NULL) {
            vRingbufferReturnItem(tx_ringbuf, (void *)dummy_item);
        }
    }
    force_disable_flag = true;
}

