#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// CẤU HÌNH KẾT NỐI THINGSBOARD CLOUD
// =============================================================================
#define THINGSBOARD_BROKER_URI          "mqtt://mqtt.thingsboard.cloud"
#define THINGSBOARD_ACCESS_TOKEN        "b2t7eXNeOUcMpSQy4LWO" // Token ThingsBoard của bạn
#define THINGSBOARD_TELEMETRY_PERIOD_S  15                    // Chu kỳ gửi Telemetry định kỳ (15s)

// Khởi tạo và khởi động MQTT Client kết nối ThingsBoard
void iot_cloud_init(void);

// Kiểm tra trạng thái kết nối MQTT
bool iot_cloud_is_connected(void);

// Bắn ngay 1 bản tin Telemetry khi có sự kiện thay đổi trạng thái từ Local / Mesh
void iot_cloud_publish_telemetry_now(void);

#ifdef __cplusplus
}
#endif
