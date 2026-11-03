#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Dimmer Light (0 - 100%)
    bool dimmer_on;
    uint8_t dimmer_brightness;      // Độ sáng hiện tại (0 - 100%)
    uint8_t dimmer_last_brightness; // Độ sáng ghi nhớ trước khi tắt (mặc định 80%)

    // Công tắc Relay 1 kênh
    bool relay_on;

    // Cảm biến DHT11
    float temperature;
    float humidity;
    bool sensor_valid;
} gateway_device_state_t;

// Kiểu callback thông báo thay đổi trạng thái (dùng cho ThingsBoard Telemetry & Gemini Prompt)
typedef void (*gateway_state_change_cb_t)(const gateway_device_state_t *new_state);

// Khởi tạo State Manager
void gateway_state_manager_init(void);

// Lấy bản sao trạng thái an toàn (Thread-Safe)
void gateway_state_get(gateway_device_state_t *out_state);

// Cập nhật trạng thái Đèn Dimmer
void gateway_state_set_dimmer(bool on, uint8_t brightness);

// Cập nhật trạng thái Relay
void gateway_state_set_relay(bool on);

// Cập nhật số liệu cảm biến DHT11 từ H2
void gateway_state_set_sensor(float temperature, float humidity);

// Đăng ký callback theo dõi sự kiện thay đổi trạng thái
void gateway_state_register_callback(gateway_state_change_cb_t cb);

#ifdef __cplusplus
}
#endif

