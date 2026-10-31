#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo UART kết nối với ESP32-H2/C6 (TX=43, RX=44, 115200bps)
void mesh_controller_init(void);

// Gửi một lệnh điều khiển dưới dạng chuỗi (VD: "<CMD:LIGHT:LIVING:ON>")
void mesh_controller_send_command(const char *cmd_str);

#ifdef __cplusplus
}
#endif
