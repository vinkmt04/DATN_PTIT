#pragma once

#include "lvgl.h"
#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo màn hình Chờ (Apple Standby Clock Mode)
lv_obj_t* ui_screen_idle_init(void);

// Cập nhật Giờ : Phút và Ngày tháng trên màn hình Idle
void ui_screen_idle_update_time(const struct tm *timeinfo);

// Cập nhật thẻ môi trường thu nhỏ (Nhiệt độ & Độ ẩm)
void ui_screen_idle_update_sensors(float temp, float hum, bool valid);

// Cập nhật các icon trạng thái mạng thu nhỏ
void ui_screen_idle_update_status(bool wifi_ok, bool cloud_ok, uint8_t mesh_nodes);

#ifdef __cplusplus
}
#endif
