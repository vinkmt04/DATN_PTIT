#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo nền tảng phần cứng & mạng sớm (NVS, Event Loop, Netif, mDNS)
// Đảm bảo chạy trước khi OpenThread hoặc Wi-Fi cần đính kèm netif
void sys_core_platform_init(void);

// Khởi tạo các hệ thống cốt lõi: Wi-Fi STA / SoftAP Provisioning
void sys_core_init(void);

// Kiểm tra trạng thái Wi-Fi
bool sys_core_is_wifi_connected(void);

// Đăng ký hàm kiểm tra trạng thái bận của voice AI để hoãn kết nối lại khi người dùng đang nói
typedef bool (*sys_core_is_busy_cb_t)(void);
void sys_core_set_voice_busy_check(sys_core_is_busy_cb_t cb);

// Xóa thông tin Wi-Fi và khởi động lại vào chế độ SoftAP cấu hình
void sys_core_reset_wifi(void);

// Lấy địa chỉ IP dạng chuỗi (VD: "192.168.1.50")
void sys_core_get_ip_str(char *out_str, size_t max_len);

// Lấy độ mạnh sóng Wi-Fi (RSSI dBm)
int8_t sys_core_get_wifi_rssi(void);

// Lấy tên Wi-Fi (SSID) đang kết nối
void sys_core_get_wifi_ssid(char *out_str, size_t max_len);

// In thông số phân bổ DRAM và Octal PSRAM chi tiết
void sys_core_print_memory_stats(const char *phase_tag);

#ifdef __cplusplus
}
#endif
