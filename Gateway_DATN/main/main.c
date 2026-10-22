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

    // Initialize core and wait for Wi-Fi
    sys_core_init();
    
    // Initialize Audio
    audio_hal_init();
    
    // Initialize Mesh UART Controller
    mesh_controller_init();
    
    ai_cloud_init();

    // Initialize Edge AI (Gatekeeper)
    ai_edge_init();

    ESP_LOGI(TAG, "Initialization complete. Edge AI is listening in background.");

    // Xóa task main vì vòng lặp đã được đưa vào ai_edge_task
    vTaskDelete(NULL);
}
