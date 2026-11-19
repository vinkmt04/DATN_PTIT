#pragma once

#include "lvgl.h"
#include "gateway_state_manager.h"
#include "thread_commissioner.h"
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

// Thông báo thiết bị Thread mới được phát hiện chờ phê duyệt
void ui_screen_dashboard_on_device_discovered(commissioner_dev_type_t type, const char *name, const char *id_str, int8_t rssi);

#ifdef __cplusplus
}
#endif
