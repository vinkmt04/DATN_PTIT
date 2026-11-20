#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Tra cuu thong tin dia chi IPv6 va port cua mot thiet bi tu bang SRP Server noi bo.
 * @note Ham nay phai duoc goi khi DA GIU OpenThread lock (caller quan ly lock).
 */
bool device_registry_find_locked(
    otInstance   *instance,
    const char   *instance_label,
    const char   *service_type,
    otIp6Address *out_addr,
    uint16_t     *out_port
);

/**
 * @brief Tra cuu thong tin Node Relay ("relay-01", "_relay._udp").
 * Co fallback ve dia chi IP da luu trong NVS khi Gateway vua khoi dong lai.
 */
esp_err_t device_registry_find_relay_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port);

/**
 * @brief Tra cuu thong tin Node Dimmer ("dimmer-01", "dimmer-test-01", "_dimmer._udp").
 * Co fallback ve dia chi IP da luu trong NVS khi Gateway vua khoi dong lai.
 */
esp_err_t device_registry_find_dimmer_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port);

/**
 * @brief Tra cuu thong tin Node Sensor ("sensor-01", "_sensor._udp").
 * Co fallback ve dia chi IP da luu trong NVS khi Gateway vua khoi dong lai.
 */
esp_err_t device_registry_find_sensor_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port);

/**
 * @brief Dem so luong thiet bi Thread hop le dang online (tu SRP Server hoac NVS cache hop le).
 */
uint8_t device_registry_count_active_locked(otInstance *instance);

/**
 * @brief Xoa cache dia chi IPv6 cua mot thiet bi khoi NVS khi nguoi dung xoa thiet bi khoi mang.
 * @param dev_name: "relay", "dimmer", "sensor", hoac NULL/"all" de xoa tat ca.
 */
void device_registry_clear_cache(const char *dev_name);

/**
 * @brief Kiem tra thiet bi co san trong NVS cache hay khong.
 */
bool device_registry_has_cached_device(const char *dev_name);

#ifdef __cplusplus
}
#endif
