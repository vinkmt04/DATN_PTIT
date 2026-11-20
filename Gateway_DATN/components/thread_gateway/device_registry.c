/*
 * DEVICE REGISTRY MODULE - thread_gateway
 *
 * Tra cuu thiet bi va phan giai IPv6 dong qua bang SRP Server tren Border Router.
 * Tich hop NVS Cache giup Gateway ket noi tuc thi toi cac Node ngay khi khoi dong lai
 * ma khong can cho SRP Server hoc lai toan bo tu dau.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/srp_server.h"

#include "device_registry.h"

#define TAG "DEV_REGISTRY"
#define NVS_NS_IP_CACHE "dev_ip_cache"

static bool s_relay_deleted_runtime  = false;
static bool s_dimmer_deleted_runtime = false;
static bool s_sensor_deleted_runtime = false;

static bool load_cached_ip(const char *key, otIp6Address *out_addr)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_IP_CACHE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*out_addr);
    esp_err_t err = nvs_get_blob(h, key, out_addr, &len);
    nvs_close(h);
    return (err == ESP_OK && len == sizeof(*out_addr));
}

static void save_cached_ip(const char *key, const otIp6Address *in_addr)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_IP_CACHE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, in_addr, sizeof(*in_addr));
        nvs_commit(h);
        nvs_close(h);
    }
}

bool device_registry_has_cached_device(const char *dev_name)
{
    if (dev_name == NULL) return false;
    otIp6Address dummy;
    if (strstr(dev_name, "relay") != NULL) {
        return !s_relay_deleted_runtime && load_cached_ip("ip_relay", &dummy);
    }
    if (strstr(dev_name, "dimmer") != NULL) {
        return !s_dimmer_deleted_runtime && load_cached_ip("ip_dimmer", &dummy);
    }
    if (strstr(dev_name, "sensor") != NULL) {
        return !s_sensor_deleted_runtime && load_cached_ip("ip_sensor", &dummy);
    }
    return false;
}

void device_registry_clear_cache(const char *dev_name)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_IP_CACHE, NVS_READWRITE, &h) == ESP_OK) {
        if (dev_name == NULL || strcasecmp(dev_name, "all") == 0) {
            nvs_erase_all(h);
            s_relay_deleted_runtime  = true;
            s_dimmer_deleted_runtime = true;
            s_sensor_deleted_runtime = true;
            ESP_LOGI(TAG, "Cleared ALL device IPv6 caches");
        } else {
            if (strstr(dev_name, "relay") != NULL) {
                nvs_erase_key(h, "ip_relay");
                s_relay_deleted_runtime = true;
                ESP_LOGI(TAG, "Cleared Relay IPv6 cache");
            }
            if (strstr(dev_name, "dimmer") != NULL) {
                nvs_erase_key(h, "ip_dimmer");
                s_dimmer_deleted_runtime = true;
                ESP_LOGI(TAG, "Cleared Dimmer IPv6 cache");
            }
            if (strstr(dev_name, "sensor") != NULL) {
                nvs_erase_key(h, "ip_sensor");
                s_sensor_deleted_runtime = true;
                ESP_LOGI(TAG, "Cleared Sensor IPv6 cache");
            }
        }
        nvs_commit(h);
        nvs_close(h);
    }
}

bool device_registry_find_locked(
    otInstance   *instance,
    const char   *instance_label,
    const char   *service_type,
    otIp6Address *out_addr,
    uint16_t     *out_port)
{
    if (instance == NULL || service_type == NULL || out_addr == NULL || out_port == NULL) {
        return false;
    }

    const otSrpServerHost *host = NULL;
    while ((host = otSrpServerGetNextHost(instance, host)) != NULL) {
        if (otSrpServerHostIsDeleted(host)) {
            continue;
        }

        const otSrpServerService *service = NULL;
        while ((service = otSrpServerHostGetNextService(host, service)) != NULL) {
            if (otSrpServerServiceIsDeleted(service)) {
                continue;
            }

            const char *label = otSrpServerServiceGetInstanceLabel(service);
            const char *service_name = otSrpServerServiceGetServiceName(service);

            /* So khop loai dich vu SRP */
            if (service_name == NULL || strstr(service_name, service_type) == NULL) {
                continue;
            }

            /* So khop nhan instance: khop chinh xac hoac chua tu khoa nhan (neu instance_label != NULL) */
            if (instance_label != NULL && label != NULL) {
                if (strcasecmp(label, instance_label) != 0 && strstr(label, instance_label) == NULL) {
                    continue;
                }
            }

            uint16_t port = otSrpServerServiceGetPort(service);
            if (port == 0) {
                port = 5683; // Default CoAP port
            }

            uint8_t addr_num = 0;
            const otIp6Address *addrs = otSrpServerHostGetAddresses(host, &addr_num);
            if (addrs == NULL || addr_num < 1) {
                continue;
            }

            *out_addr = addrs[0];
            *out_port = port;

            char ip_str[OT_IP6_ADDRESS_STRING_SIZE];
            otIp6AddressToString(&addrs[0], ip_str, sizeof(ip_str));
            ESP_LOGD(TAG, "Found SRP: instance='%s' type='%s' addr=[%s]:%u",
                     label ? label : "n/a", service_name, ip_str, (unsigned)port);

            return true;
        }
    }

    return false;
}

esp_err_t device_registry_find_relay_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port)
{
    if (s_relay_deleted_runtime) {
        return ESP_ERR_NOT_FOUND;
    }

    // 1. Tim trong SRP Server
    if (device_registry_find_locked(instance, "relay-01", "_relay._udp", out_addr, out_port) ||
        device_registry_find_locked(instance, "relay", "_relay._udp", out_addr, out_port) ||
        device_registry_find_locked(instance, NULL, "_relay._udp", out_addr, out_port)) {
        save_cached_ip("ip_relay", out_addr);
        return ESP_OK;
    }

    // 2. Fallback tim trong NVS cache
    if (load_cached_ip("ip_relay", out_addr)) {
        *out_port = 5683;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t device_registry_find_dimmer_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port)
{
    if (s_dimmer_deleted_runtime) {
        return ESP_ERR_NOT_FOUND;
    }

    // 1. Tim trong SRP Server (ho tro ca "dimmer-01", "dimmer-test-01", "dimmer" hoac bat ky service _dimmer._udp)
    if (device_registry_find_locked(instance, "dimmer-01", "_dimmer._udp", out_addr, out_port) ||
        device_registry_find_locked(instance, "dimmer-test-01", "_dimmer._udp", out_addr, out_port) ||
        device_registry_find_locked(instance, "dimmer", "_dimmer._udp", out_addr, out_port) ||
        device_registry_find_locked(instance, NULL, "_dimmer._udp", out_addr, out_port)) {
        save_cached_ip("ip_dimmer", out_addr);
        return ESP_OK;
    }

    // 2. Fallback tim trong NVS cache
    if (load_cached_ip("ip_dimmer", out_addr)) {
        *out_port = 5683;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t device_registry_find_sensor_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port)
{
    if (s_sensor_deleted_runtime) {
        return ESP_ERR_NOT_FOUND;
    }

    // 1. Tim trong SRP Server
    if (device_registry_find_locked(instance, "sensor-01", "_sensor._udp", out_addr, out_port) ||
        device_registry_find_locked(instance, "sensor", "_sensor._udp", out_addr, out_port) ||
        device_registry_find_locked(instance, NULL, "_sensor._udp", out_addr, out_port)) {
        save_cached_ip("ip_sensor", out_addr);
        return ESP_OK;
    }

    // 2. Fallback tim trong NVS cache
    if (load_cached_ip("ip_sensor", out_addr)) {
        *out_port = 5683;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
}

uint8_t device_registry_count_active_locked(otInstance *instance)
{
    uint8_t count = 0;
    otIp6Address addr;
    uint16_t port;

    if (device_registry_find_relay_locked(instance, &addr, &port) == ESP_OK) {
        count++;
    }
    if (device_registry_find_dimmer_locked(instance, &addr, &port) == ESP_OK) {
        count++;
    }
    if (device_registry_find_sensor_locked(instance, &addr, &port) == ESP_OK) {
        count++;
    }

    return count;
}
