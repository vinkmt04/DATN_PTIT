#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACTUATOR_PRESENCE_TIMEOUT_MS 75000  // 75s khong co phan hoi -> Coi nhu Offline cho Relay/Dimmer (2 chu ky hb 30s + 15s grace)
#define SENSOR_PRESENCE_TIMEOUT_MS   135000 // 135s khong co phan hoi -> Coi nhu Offline cho Sensor (2 chu ky gui 60s + 15s grace)
#define PRESENCE_ACTIVITY_TIMEOUT_MS ACTUATOR_PRESENCE_TIMEOUT_MS // Tương thích ngược

/**
 * @brief Định danh kiểu thiết bị chuẩn hoá trong mạng Thread Mesh
 */
typedef enum {
    THREAD_DEV_RELAY = 0,
    THREAD_DEV_DIMMER,
    THREAD_DEV_SENSOR,
    THREAD_DEV_COUNT,
    THREAD_DEV_UNKNOWN = -1
} thread_dev_type_t;

/**
 * @brief Khởi tạo module Device Registry, nạp trạng thái từ NVS và di trú namespace cũ nếu cần.
 */
void device_registry_init(void);

/**
 * @brief Chuyển đổi chuỗi tên thiết bị thành enum thread_dev_type_t
 */
thread_dev_type_t device_registry_str_to_type(const char *dev_name);

/**
 * @brief Lấy chuỗi định danh chuẩn ("relay", "dimmer", "sensor") từ enum
 */
const char *device_registry_type_to_str(thread_dev_type_t type);

/**
 * @brief Tra cứu thông tin địa chỉ IPv6 và port của thiết bị từ SRP Server nội bộ.
 * @note Phải được gọi khi ĐÃ GIỮ OpenThread lock (caller quản lý lock).
 */
bool device_registry_find_locked(
    otInstance   *instance,
    const char   *instance_label,
    const char   *service_type,
    otIp6Address *out_addr,
    uint16_t     *out_port
);

/**
 * @brief Tra cứu địa chỉ IPv6 và port kèm bộ lọc tuổi bản ghi (max_age_ms) để loại bỏ thiết bị cũ.
 */
bool device_registry_find_fresh_locked(
    otInstance   *instance,
    const char   *instance_label,
    const char   *service_type,
    uint32_t      max_age_ms,
    otIp6Address *out_addr,
    uint16_t     *out_port
);

/**
 * @brief Phân giải địa chỉ IPv6 thống nhất cho từng loại thiết bị (SRP trước, fallback RAM/NVS cache).
 */
esp_err_t device_registry_resolve_address_locked(
    otInstance        *instance,
    thread_dev_type_t  type,
    otIp6Address      *out_addr,
    uint16_t          *out_port
);

/**
 * @brief Wrapper phân giải địa chỉ cho Node Relay, Dimmer, Sensor
 */
esp_err_t device_registry_find_relay_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port);
esp_err_t device_registry_find_dimmer_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port);
esp_err_t device_registry_find_sensor_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port);

/**
 * @brief Cập nhật trực tiếp địa chỉ IPv6 thực tế của thiết bị khi nhận được gói tin (ví dụ /report).
 */
void device_registry_update_ip(thread_dev_type_t type, const otIp6Address *addr);

/**
 * @brief Đếm số lượng thiết bị Thread thực tế đang ONLINE trong mạng.
 */
uint8_t device_registry_count_active_locked(otInstance *instance);

/**
 * @brief Ghi nhận hoạt động truyền thông hợp lệ từ node (Heartbeat, CoAP ACK, /report, Telemetry).
 * Cập nhật mốc thời gian last_seen_ms để duy trì trạng thái Online.
 * TUYỆT ĐỐI KHÔNG tự ý đăng ký (enroll) thiết bị chưa được cấp phép.
 */
void device_registry_record_type_activity(thread_dev_type_t type);
void device_registry_record_activity(const char *dev_name);

/**
 * @brief Đánh dấu thiết bị Offline ngay lập tức (xóa mốc last_seen_ms).
 * Dùng khi lệnh điều khiển CoAP bị timeout hoặc báo lỗi kết nối.
 */
void device_registry_mark_type_offline(thread_dev_type_t type);
void device_registry_mark_offline(const char *dev_name);

/**
 * @brief Lấy mốc thời gian hoạt động gần nhất (epoch ms) của thiết bị.
 */
int64_t device_registry_get_last_seen_ms(thread_dev_type_t type);

/**
 * @brief Kiểm tra thiết bị có thực sự ONLINE hay không (Strict Presence Check < 10s).
 */
bool device_registry_is_type_online(otInstance *instance, thread_dev_type_t type);
bool device_registry_is_node_online(otInstance *instance, const char *dev_name);

/**
 * @brief Quản lý trạng thái đăng ký thiết bị (Enrollment / Registration Persistence).
 */
void device_registry_set_type_registered(thread_dev_type_t type, bool registered);
void device_registry_set_registered(const char *dev_name, bool registered);
bool device_registry_is_type_registered(thread_dev_type_t type);
bool device_registry_is_registered(const char *dev_name);
void device_registry_get_registered(bool *out_relay, bool *out_dimmer, bool *out_sensor);

/**
 * @brief Xóa cache IP của một thiết bị khi người dùng xóa khỏi mạng.
 */
void device_registry_clear_cache(const char *dev_name);

/**
 * @brief Kiểm tra thiết bị có địa chỉ IP cache hợp lệ hay không.
 */
bool device_registry_has_cached_device(const char *dev_name);

/**
 * @brief Xóa toàn bộ thiết bị và cache trong hệ thống.
 */
void device_registry_clear_all(void);

#ifdef __cplusplus
}
#endif
