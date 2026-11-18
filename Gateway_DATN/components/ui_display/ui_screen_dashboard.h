#pragma once

#include "lvgl.h"
#include "gateway_state_manager.h"
#include <time.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo màn hình Dashboard chính (3 Tabs theo chuẩn Apple Light Theme)
lv_obj_t* ui_screen_dashboard_init(void);

// Cập nhật toàn bộ trạng thái thiết bị và cảm biến lên Dashboard
void ui_screen_dashboard_update_state(const gateway_device_state_t *state);

// Cập nhật đồng hồ thời gian thực và ngày tháng trên Header Bar
void ui_screen_dashboard_update_time(const struct tm *timeinfo);

// Cập nhật thông tin hệ thống mạng trên Header Bar và Tab Hệ thống
void ui_screen_dashboard_update_network(bool wifi_ok, const char *ip_str, int8_t rssi, bool cloud_ok);

// Chọn Tab hiển thị (0: Thiết bị, 1: Môi trường, 2: Hệ thống)
void ui_screen_dashboard_select_tab(uint16_t tab_idx);

#ifdef __cplusplus
}
#endif
