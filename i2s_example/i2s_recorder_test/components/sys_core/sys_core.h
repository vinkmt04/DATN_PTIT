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

// Xóa thông tin Wi-Fi và khởi động lại vào chế độ SoftAP cấu hình
void sys_core_reset_wifi(void);

#ifdef __cplusplus
}
#endif
