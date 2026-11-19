/*
 * DEVICE REGISTRY MODULE - thread_gateway
 *
 * Tra cuu thiet bi va phan giai IPv6 dong qua bang SRP Server tren Border Router.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/srp_server.h"

#include "device_registry.h"

#define TAG "DEV_REGISTRY"

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

            /* So khop nhan instance: khop chinh xac hoac chua tu khoa nhan */
            if (instance_label != NULL && label != NULL) {
                if (strcasecmp(label, instance_label) != 0 && strstr(label, instance_label) == NULL) {
                    continue;
                }
            }

            uint16_t port = otSrpServerServiceGetPort(service);
            if (port == 0) {
                continue;
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
    if (device_registry_find_locked(instance, "relay-01", "_relay._udp", out_addr, out_port)) {
        return ESP_OK;
    }
    if (device_registry_find_locked(instance, "relay", "_relay._udp", out_addr, out_port)) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t device_registry_find_dimmer_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port)
{
    if (device_registry_find_locked(instance, "dimmer-01", "_dimmer._udp", out_addr, out_port)) {
        return ESP_OK;
    }
    if (device_registry_find_locked(instance, "dimmer-test-01", "_dimmer._udp", out_addr, out_port)) {
        return ESP_OK;
    }
    if (device_registry_find_locked(instance, "dimmer", "_dimmer._udp", out_addr, out_port)) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t device_registry_find_sensor_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port)
{
    if (device_registry_find_locked(instance, "sensor-01", "_sensor._udp", out_addr, out_port)) {
        return ESP_OK;
    }
    if (device_registry_find_locked(instance, "sensor", "_sensor._udp", out_addr, out_port)) {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

uint8_t device_registry_count_active_locked(otInstance *instance)
{
    if (instance == NULL) return 0;
    uint8_t count = 0;
    const otSrpServerHost *host = NULL;
    while ((host = otSrpServerGetNextHost(instance, host)) != NULL) {
        if (!otSrpServerHostIsDeleted(host)) {
            count++;
        }
    }
    return count;
}
