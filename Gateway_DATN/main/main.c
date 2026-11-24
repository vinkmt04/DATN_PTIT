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
#include "nvs_flash.h"
#include "gateway_state_manager.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, ">>> STARTING SMART HOME VOICE GATEWAY (ESP32-S3) <<<");
    ESP_LOGI(TAG, "=================================================");

    // -------------------------------------------------------------
    // GIAI ĐOẠN 1: NỀN TẢNG CORE & STATE MODEL (0% -> 15%)
    // -------------------------------------------------------------
    // 0. Khởi tạo NVS, Netif, Event Loop và mDNS sớm cho toàn bộ hệ thống
    sys_core_platform_init();

    // Khởi tạo Gateway State Manager trung tâm (Nạp NVS registry Relay, Dimmer, Sensor)
    gateway_state_manager_init();

    // Khởi tạo Màn hình ST7789V (SPI DMA + LVGL) và hiển thị Splash Screen ngay lập tức
    ui_display_init();
    ui_display_update_boot_progress(15, "Khởi tạo NVS & State Manager...");
    vTaskDelay(pdMS_TO_TICKS(150));

    // -------------------------------------------------------------
    // GIAI ĐOẠN 2: MẠNG KHÔNG DÂY CỤC BỘ THREAD MESH (15% -> 45%)
    // (Khởi tạo mạng Mesh trước để chuẩn bị hạ tầng điều khiển thiết bị)
    // -------------------------------------------------------------
    ui_display_update_boot_progress(25, "Kết nối Thread RCP (ESP32-H2)...");
    mesh_controller_init();
    ui_display_update_boot_progress(45, "Thread Mesh sẵn sàng (Leader PanID 0xa05e)");
    vTaskDelay(pdMS_TO_TICKS(150));

    // -------------------------------------------------------------
    // GIAI ĐOẠN 3: PHẦN CỨNG ÂM THANH & TRỢ LÝ GIỌNG NÓI EDGE AI (45% -> 70%)
    // (Sau khi Mesh đã sẵn sàng, kích hoạt Audio HAL và nạp Model nhận diện 'Hey Cherry')
    // -------------------------------------------------------------
    ui_display_update_boot_progress(50, "Khởi tạo Audio HAL (I2S DMA & VAD)...");
    audio_hal_init();

    ui_display_update_boot_progress(60, "Nạp mô hình Edge AI 'Hey Cherry'...");
    ai_edge_init();
    ui_display_set_abort_callback(ai_edge_abort);
    ui_display_set_finish_callback(ai_edge_finish_speaking);
    ui_display_update_boot_progress(70, "Trợ lý giọng nói Edge AI sẵn sàng");
    vTaskDelay(pdMS_TO_TICKS(150));

    // -------------------------------------------------------------
    // GIAI ĐOẠN 4: KẾT NỐI WI-FI & MẠNG IP (70% -> 90%)
    // (Khởi động Wi-Fi STA, mDNS Border Router, đồng bộ giờ SNTP)
    // -------------------------------------------------------------
    ui_display_update_boot_progress(75, "Đang kết nối mạng Wi-Fi...");
    sys_core_init();
    if (sys_core_is_wifi_connected()) {
        char ip_str[24];
        sys_core_get_ip_str(ip_str, sizeof(ip_str));
        char status_buf[64];
        snprintf(status_buf, sizeof(status_buf), "Wi-Fi kết nối (IP: %s)", ip_str);
        ui_display_update_boot_progress(90, status_buf);
    } else {
        ui_display_update_boot_progress(90, "Chế độ Mạng Cục bộ (Offline)");
    }
    vTaskDelay(pdMS_TO_TICKS(150));

    // -------------------------------------------------------------
    // GIAI ĐOẠN 5: DỊCH VỤ ĐÁM MÂY & TRỢ LÝ THÔNG MINH (90% -> 100%)
    // -------------------------------------------------------------
    ui_display_update_boot_progress(95, "Khởi tạo ThingsBoard & Gemini Live...");
    iot_cloud_init();
    ai_cloud_init();

    ui_display_update_boot_progress(100, "Khởi động hoàn tất! Sẵn sàng.");
    vTaskDelay(pdMS_TO_TICKS(500)); // Dừng 500ms để người dùng thấy thanh 100% hoàn hảo

    // -------------------------------------------------------------
    // HOÀN TẤT: CHUYỂN SANG MÀN HÌNH CHỜ (IDLE AMBIENT CLOCK)
    // -------------------------------------------------------------
    ui_display_finish_splash();

    ESP_LOGI(TAG, "Initialization complete. Gateway running in Standby Mode.");
    sys_core_print_memory_stats("BOOT_COMPLETE");
    sys_core_print_memory_stats("AFTER_OPTIMIZATION");

    // Xóa task main vì các subsystem đều chạy trên các FreeRTOS worker tasks
    vTaskDelete(NULL);
}
