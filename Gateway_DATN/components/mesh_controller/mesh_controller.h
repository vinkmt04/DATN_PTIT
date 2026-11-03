#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "gateway_state_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo UART kết nối với ESP32-H2 (TX=13, RX=14, 115200bps) & State Manager & RX Task
void mesh_controller_init(void);

// Gửi một chuỗi lệnh điều khiển chung (VD: "CMD:DIMMER:ON", "<CMD:DIMMER:SET:70>", "CMD:RELAY:ON")
void mesh_controller_send_command(const char *cmd_str);

// Điều khiển Đèn Dimmer có chỉ định độ sáng (0 - 100%)
void mesh_controller_set_dimmer(bool on, uint8_t brightness);

// Điều khiển Công tắc Relay 1 kênh (Bật / Tắt)
void mesh_controller_set_relay(bool on);

#ifdef __cplusplus
}
#endif
