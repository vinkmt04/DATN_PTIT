#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sys_core.h"
#include "audio_hal.h"
#include "ai_edge.h"
#include "ai_cloud.h"
#include "iot_cloud.h"
#include "mesh_controller.h"
#include "ui_display.h"
#include "format_wav.h"
#include "esp_log.h"

static const char *TAG = "main";

#define CHUNK_SIZE_BYTES 2048 // 1024 mẫu 16-bit
#define CHUNK_SAMPLES (CHUNK_SIZE_BYTES / sizeof(int16_t))
#define MAX_RECORD_SIZE (1 * 1024 * 1024) // 1MB cho file ghi âm tạm thời

void app_main(void)
{
    ESP_LOGI(TAG, "Starting Voice Gateway (ESP32-S3)");

    // 1. Khởi tạo Audio Hardware (I2S DMA) trước tiên khi SRAM nội bộ còn nguyên vẹn 100%
    audio_hal_init();

    // Khởi tạo Gateway State Manager sớm để UI và các module luôn có trạng thái an toàn
    gateway_state_manager_init();

    // 2. Khởi tạo Màn hình ST7789V (SPI DMA + LVGL double-buffer)
    ui_display_init();

    // 3. Khởi tạo Wi-Fi, NVS & Event Loop
    sys_core_init();

    // 4. Khởi tạo Mesh Controller (OpenThread Border Router Host)
    mesh_controller_init();
    
    // 5. Khởi tạo Cloud & Trợ lý thông minh
    iot_cloud_init();
    ai_cloud_init();

    // 6. Khởi tạo Edge AI (Gatekeeper listening)
    ai_edge_init();

    // Link UI Touch Dismiss to ai_edge_abort
    ui_display_set_abort_callback(ai_edge_abort);

    ESP_LOGI(TAG, "Initialization complete. Edge AI is listening in background.");

    // Xóa task main vì vòng lặp đã được đưa vào ai_edge_task
    vTaskDelete(NULL);
}
