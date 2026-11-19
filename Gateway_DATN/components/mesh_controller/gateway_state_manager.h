#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// CẤU HÌNH THIẾT BỊ HIỆN DIỆN TRONG HỆ THỐNG
// (1: Thiết bị có mặt / Bật điều khiển, 0: Chưa có / Tắt điều khiển)
// =============================================================================
#define GATEWAY_ENABLE_DIMMER   1   // 1: Đèn Dimming (0-100%), 0: Chưa kết nối/Tắt
#define GATEWAY_ENABLE_RELAY    1   // 1: Công tắc Relay 1 kênh, 0: Chưa kết nối/Tắt
#define GATEWAY_ENABLE_DHT11    1   // 1: Cảm biến DHT11 (Nhiệt độ/Độ ẩm), 0: Chưa kết nối/Tắt

typedef struct {
    // Trạng thái trực tuyến của từng Node Thread Mesh (Presence Awareness)
    bool relay_node_online;         // true: Node Relay đang có mặt trong Mesh, false: Mất kết nối/Offline
    bool dimmer_node_online;        // true: Node Đèn Dimmer đang có mặt trong Mesh, false: Mất kết nối/Offline
    bool sensor_node_online;        // true: Node Cảm biến đang có mặt trong Mesh, false: Mất kết nối/Offline
    uint8_t mesh_node_count;        // Tổng số lượng Node đang kết nối trong mạng Thread Mesh

    // Dimmer Light (0 - 100%)
    bool dimmer_on;
    uint8_t dimmer_brightness;      // Độ sáng hiện tại (0 - 100%)
    uint8_t dimmer_last_brightness; // Độ sáng ghi nhớ trước khi tắt (mặc định 80%)

    // Công tắc Relay 1 kênh & Đo công suất
    bool relay_on;
    float power_watts;              // Công suất tiêu thụ tức thời (W) đo từ INA219/ACS712

    // Cảm biến môi trường (DHT22 & BH1750)
    float temperature;              // Nhiệt độ (°C)
    float humidity;                 // Độ ẩm (%RH)
    uint16_t light_lux;             // Cường độ ánh sáng (Lux)
    bool sensor_valid;
} gateway_device_state_t;

// Kiểu callback thông báo thay đổi trạng thái (dùng cho ThingsBoard Telemetry & Gemini Prompt & UI Display)
typedef void (*gateway_state_change_cb_t)(const gateway_device_state_t *new_state);

// Khởi tạo State Manager
void gateway_state_manager_init(void);

// Lấy số lượng Node Thread Mesh đang thực tế hoạt động
uint8_t gateway_state_get_active_node_count(void);

// Kiểm tra trạng thái trực tuyến của từng thiết bị
bool gateway_state_is_relay_online(void);
bool gateway_state_is_dimmer_online(void);
bool gateway_state_is_sensor_online(void);

// Lấy bản sao trạng thái an toàn (Thread-Safe)
void gateway_state_get(gateway_device_state_t *out_state);

// Cập nhật trạng thái trực tuyến của các Node Thread Mesh
void gateway_state_set_node_presence(bool relay_online, bool dimmer_online, bool sensor_online);

// Cập nhật tổng số Node trong mạng Thread
void gateway_state_set_mesh_count(uint8_t count);

// Cập nhật trạng thái Đèn Dimmer
void gateway_state_set_dimmer(bool on, uint8_t brightness);

// Cập nhật trạng thái Relay & Công suất
void gateway_state_set_relay(bool on);
void gateway_state_set_power(float watts);

// Cập nhật số liệu cảm biến môi trường từ Node Thread
void gateway_state_set_sensor(float temperature, float humidity);
void gateway_state_set_light(uint16_t lux);

// Đăng ký callback theo dõi sự kiện thay đổi trạng thái
void gateway_state_register_callback(gateway_state_change_cb_t cb);

#ifdef __cplusplus
}
#endif

