#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if __has_include("app_secrets.h")
#include "app_secrets.h"
#endif

// =============================================================================
// CẤU HÌNH KẾT NỐI THINGSBOARD CLOUD
// =============================================================================
#define THINGSBOARD_BROKER_URI          "mqtt://mqtt.thingsboard.cloud"
#ifndef THINGSBOARD_ACCESS_TOKEN
#define THINGSBOARD_ACCESS_TOKEN        "YOUR_THINGSBOARD_TOKEN"
#endif
#define THINGSBOARD_TELEMETRY_PERIOD_S  60                    // Chu kỳ gửi Telemetry định kỳ (60s = 1 phút)

// Khởi tạo và khởi động MQTT Client kết nối ThingsBoard
void iot_cloud_init(void);

// Kiểm tra trạng thái kết nối MQTT
bool iot_cloud_is_connected(void);

// Bắn ngay 1 bản tin Telemetry khi có sự kiện thay đổi trạng thái từ Local / Mesh
void iot_cloud_publish_telemetry_now(void);

#ifdef __cplusplus
}
#endif
