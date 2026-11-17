#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo các hệ thống cốt lõi: NVS, Event Loop, Wi-Fi.
// Chặn (block) cho đến khi lấy được IP nếu wait_for_ip = true.
void sys_core_init(void);

// Kiểm tra trạng thái Wi-Fi
bool sys_core_is_wifi_connected(void);

// Đăng ký hàm kiểm tra trạng thái bận của voice AI để hoãn kết nối lại khi người dùng đang nói
typedef bool (*sys_core_is_busy_cb_t)(void);
void sys_core_set_voice_busy_check(sys_core_is_busy_cb_t cb);

// Xóa thông tin Wi-Fi và khởi động lại vào chế độ SoftAP cấu hình
void sys_core_reset_wifi(void);

#ifdef __cplusplus
}
#endif
