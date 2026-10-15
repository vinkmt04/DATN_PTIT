#include "ai_edge.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "audio_hal.h"
#include "ai_cloud.h"
#include "sys_core.h"
#include "esp_heap_caps.h"

static const char *TAG = "ai_edge";

#define BOOT_BUTTON_GPIO 0

static TaskHandle_t audio_task_handle = NULL;
static TaskHandle_t button_task_handle = NULL;
static volatile bool is_recording = false;

static void audio_recording_task(void *arg) {
    ESP_LOGI(TAG, "Audio Recording Task Started.");

    int chunk_size = 2048; // 2048 samples = 128ms tại 16kHz Mono (4096 bytes)
    int16_t *audio_chunk = heap_caps_malloc(chunk_size * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!audio_chunk) {
        audio_chunk = malloc(chunk_size * sizeof(int16_t));
    }
    int loop_counter = 0;

    while (1) {
        if (is_recording) {
            if (loop_counter == 0) {
                // Báo cho Google biết người dùng bắt đầu nói (vì đã tắt VAD tự động)
                ai_cloud_send_activity_start();
            }
            
            size_t bytes_read = audio_hal_record_chunk(audio_chunk, chunk_size * sizeof(int16_t));
            if (bytes_read > 0) {
                // Gửi chunk PCM lên WebSocket
                ai_cloud_send_audio_chunk((uint8_t *)audio_chunk, bytes_read);
                
                loop_counter++;
                if (loop_counter % 8 == 0) { // ~1 second (128ms * 8 = 1024ms)
                    ESP_LOGI(TAG, "Đang ghi âm và truyền lên Cloud Live API...");
                }
            }
        } else {
            // Khi không thu âm, ngủ để nhường CPU
            loop_counter = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    if (audio_chunk) free(audio_chunk);
    vTaskDelete(NULL);
}

static void button_trigger_task(void *arg) {
    ESP_LOGI(TAG, "Button Trigger Ready. Press BOOT button (GPIO 0) once to talk, press again to send.");

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
                    if (!is_recording) {
                        ESP_LOGI(TAG, ">>> BOOT BUTTON CLICK: BẮT ĐẦU THU ÂM <<<");
                        if (sys_core_is_wifi_connected() && ai_cloud_start_turn()) {
                            ESP_LOGI(TAG, "Cloud WebSocket Ready. Bạn hãy nói...");
                            is_recording = true; // Bắt đầu thu âm
                        } else {
                            ESP_LOGE(TAG, "Chưa có Wi-Fi hoặc lỗi kết nối Cloud Live API!");
                            audio_hal_play_error_sound();
                        }
                    } else {
                        ESP_LOGI(TAG, ">>> BOOT BUTTON CLICK: KẾT THÚC THU ÂM & GỬI CLOUD <<<");
                        is_recording = false; // Dừng thu âm
                        ai_cloud_end_turn();  // Gửi activityEnd cho Gemini
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
    ESP_LOGI(TAG, "Initializing Edge Trigger (BOOT Button / GPIO 0)");
    
    xTaskCreatePinnedToCore(button_trigger_task, "button_task", 4096, NULL, 5, &button_task_handle, 1);
    xTaskCreatePinnedToCore(audio_recording_task, "audio_task", 8192, NULL, 5, &audio_task_handle, 1);
}

void ai_edge_stop_recording(void) {
    if (is_recording) {
        ESP_LOGI(TAG, "Dừng ghi âm khẩn cấp (Hủy bởi Cloud/Socket Error).");
        is_recording = false;
    }
}

bool ai_edge_is_recording(void) {
    return is_recording;
}
