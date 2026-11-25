/*
 * DEVICE REGISTRY MODULE - thread_gateway
 *
 * Quản lý danh mục thiết bị tập trung (Device Registry) & Theo dõi hiện diện trực tuyến (Presence Tracking).
 * Bộ nhớ RAM cache tốc độ cao O(1), phân giải địa chỉ SRP Server động và lưu trữ NVS bền vững.
 */

#include "device_registry.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "esp_memory_utils.h"

#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/srp_server.h"

#define TAG "DEV_REGISTRY"

#define NVS_NS_UNIFIED   "mesh_registry"
#define NVS_NS_LEGACY_1 "gw_dev_reg"
#define NVS_NS_LEGACY_2 "tg_devs"
#define NVS_NS_LEGACY_3 "dev_ip_cache"

typedef struct {
    char key[16];
    otIp6Address addr;
} deferred_nvs_ip_t;

static void nvs_persist_ip(const char *key, const otIp6Address *addr) {
    if (!key || !addr) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_UNIFIED, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, addr, sizeof(otIp6Address));
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open(NVS_NS_LEGACY_3, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, key, addr, sizeof(otIp6Address));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void deferred_nvs_save_ip(void *pvParameter1, uint32_t ulParameter2) {
    (void)ulParameter2;
    deferred_nvs_ip_t *ctx = (deferred_nvs_ip_t *)pvParameter1;
    if (!ctx) return;
    nvs_persist_ip(ctx->key, &ctx->addr);
    free(ctx);
}

static void schedule_or_run_nvs_ip_save(const char *key, const otIp6Address *addr, bool on_psram_stack) {
    if (on_psram_stack) {
        deferred_nvs_ip_t *ctx = (deferred_nvs_ip_t *)malloc(sizeof(deferred_nvs_ip_t));
        if (ctx) {
            strncpy(ctx->key, key, sizeof(ctx->key) - 1);
            ctx->key[sizeof(ctx->key) - 1] = '\0';
            ctx->addr = *addr;
            if (xTimerPendFunctionCall(deferred_nvs_save_ip, ctx, 0, pdMS_TO_TICKS(100)) != pdPASS) {
                ESP_LOGW(TAG, "xTimerPendFunctionCall failed to defer NVS IP save");
                free(ctx);
            }
        }
    } else {
        nvs_persist_ip(key, addr);
    }
}

typedef struct {
    thread_dev_type_t type;
    const char       *name;
    const char       *srp_label;
    const char       *srp_service;
    const char       *nvs_reg_key;
    const char       *nvs_ip_key;
    uint16_t          default_port;
} thread_dev_desc_t;

static const thread_dev_desc_t s_device_descs[THREAD_DEV_COUNT] = {
    [THREAD_DEV_RELAY] = {
        .type         = THREAD_DEV_RELAY,
        .name         = "relay",
        .srp_label    = "relay-01",
        .srp_service  = "_relay._udp",
        .nvs_reg_key  = "reg_relay",
        .nvs_ip_key   = "ip_relay",
        .default_port = 5683,
    },
    [THREAD_DEV_DIMMER] = {
        .type         = THREAD_DEV_DIMMER,
        .name         = "dimmer",
        .srp_label    = "dimmer-01",
        .srp_service  = "_dimmer._udp",
        .nvs_reg_key  = "reg_dimmer",
        .nvs_ip_key   = "ip_dimmer",
        .default_port = 5683,
    },
    [THREAD_DEV_SENSOR] = {
        .type         = THREAD_DEV_SENSOR,
        .name         = "sensor",
        .srp_label    = "sensor-01",
        .srp_service  = "_sensor._udp",
        .nvs_reg_key  = "reg_sensor",
        .nvs_ip_key   = "ip_sensor",
        .default_port = 5683,
    },
};

typedef struct {
    bool         registered;
    int64_t      last_seen_ms;
    bool         has_cached_ip;
    otIp6Address cached_ip;
    bool         deleted_runtime;
} thread_dev_entry_t;

static thread_dev_entry_t s_entries[THREAD_DEV_COUNT] = {0};
static SemaphoreHandle_t  s_reg_mutex = NULL;
static bool               s_inited = false;

static void ensure_inited_locked(void)
{
    if (s_inited) return;

    nvs_handle_t h;
    bool has_unified = (nvs_open(NVS_NS_UNIFIED, NVS_READWRITE, &h) == ESP_OK);

    for (int i = 0; i < THREAD_DEV_COUNT; i++) {
        const thread_dev_desc_t *desc = &s_device_descs[i];
        uint8_t reg_val = 0;
        bool loaded = false;

        // 1. Kiểm tra namespace mới (mesh_registry)
        if (has_unified) {
            if (nvs_get_u8(h, desc->nvs_reg_key, &reg_val) == ESP_OK) {
                s_entries[i].registered = (reg_val != 0);
                loaded = true;
            }
            size_t ip_len = sizeof(otIp6Address);
            if (nvs_get_blob(h, desc->nvs_ip_key, &s_entries[i].cached_ip, &ip_len) == ESP_OK &&
                ip_len == sizeof(otIp6Address)) {
                s_entries[i].has_cached_ip = true;
            }
        }

        // 2. Di trú dữ liệu từ namespace cũ nếu chưa có trong mesh_registry
        if (!loaded) {
            nvs_handle_t h_leg;
            // Kiểm tra gw_dev_reg
            if (nvs_open(NVS_NS_LEGACY_1, NVS_READONLY, &h_leg) == ESP_OK) {
                if (nvs_get_u8(h_leg, desc->nvs_reg_key, &reg_val) == ESP_OK) {
                    s_entries[i].registered = (reg_val != 0);
                    loaded = true;
                }
                nvs_close(h_leg);
            }
            // Kiểm tra tg_devs nếu chưa có
            if (!loaded && nvs_open(NVS_NS_LEGACY_2, NVS_READONLY, &h_leg) == ESP_OK) {
                if (nvs_get_u8(h_leg, desc->nvs_reg_key, &reg_val) == ESP_OK) {
                    s_entries[i].registered = (reg_val != 0);
                    loaded = true;
                }
                nvs_close(h_leg);
            }
            // Nạp IP cache cũ từ dev_ip_cache
            if (!s_entries[i].has_cached_ip && nvs_open(NVS_NS_LEGACY_3, NVS_READONLY, &h_leg) == ESP_OK) {
                size_t ip_len = sizeof(otIp6Address);
                if (nvs_get_blob(h_leg, desc->nvs_ip_key, &s_entries[i].cached_ip, &ip_len) == ESP_OK &&
                    ip_len == sizeof(otIp6Address)) {
                    s_entries[i].has_cached_ip = true;
                    if (!loaded) {
                        s_entries[i].registered = true;
                    }
                }
                nvs_close(h_leg);
            }

            // Ghi cấu hình đã di trú vào mesh_registry
            if (has_unified && s_entries[i].registered) {
                nvs_set_u8(h, desc->nvs_reg_key, 1);
                if (s_entries[i].has_cached_ip) {
                    nvs_set_blob(h, desc->nvs_ip_key, &s_entries[i].cached_ip, sizeof(otIp6Address));
                }
            }
        }

        s_entries[i].last_seen_ms = 0;
        s_entries[i].deleted_runtime = false;

        ESP_LOGI(TAG, "Device [%s]: Registered=%d, CachedIP=%d",
                 desc->name, s_entries[i].registered, s_entries[i].has_cached_ip);
    }

    if (has_unified) {
        nvs_commit(h);
        nvs_close(h);
    }

    s_inited = true;
}

void device_registry_init(void)
{
    if (s_reg_mutex == NULL) {
        s_reg_mutex = xSemaphoreCreateMutex();
    }
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        ensure_inited_locked();
        xSemaphoreGive(s_reg_mutex);
    }
}

thread_dev_type_t device_registry_str_to_type(const char *dev_name)
{
    if (dev_name == NULL) return THREAD_DEV_UNKNOWN;
    if (strstr(dev_name, "relay") != NULL)  return THREAD_DEV_RELAY;
    if (strstr(dev_name, "dimmer") != NULL) return THREAD_DEV_DIMMER;
    if (strstr(dev_name, "sensor") != NULL) return THREAD_DEV_SENSOR;
    return THREAD_DEV_UNKNOWN;
}

const char *device_registry_type_to_str(thread_dev_type_t type)
{
    if (type >= 0 && type < THREAD_DEV_COUNT) {
        return s_device_descs[type].name;
    }
    return "unknown";
}

bool device_registry_is_type_registered(thread_dev_type_t type)
{
    if (type < 0 || type >= THREAD_DEV_COUNT) return false;
    device_registry_init();

    bool reg = false;
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        reg = s_entries[type].registered && !s_entries[type].deleted_runtime;
        xSemaphoreGive(s_reg_mutex);
    }
    return reg;
}

bool device_registry_is_registered(const char *dev_name)
{
    thread_dev_type_t type = device_registry_str_to_type(dev_name);
    return (type != THREAD_DEV_UNKNOWN) ? device_registry_is_type_registered(type) : false;
}

static void nvs_persist_reg(thread_dev_type_t type, bool registered) {
    if (type < 0 || type >= THREAD_DEV_COUNT) return;
    const thread_dev_desc_t *desc = &s_device_descs[type];
    nvs_handle_t h;

    // 1. Lưu vào unified namespace
    if (nvs_open(NVS_NS_UNIFIED, NVS_READWRITE, &h) == ESP_OK) {
        if (registered) {
            nvs_set_u8(h, desc->nvs_reg_key, 1);
        } else {
            nvs_erase_key(h, desc->nvs_reg_key);
            nvs_erase_key(h, desc->nvs_ip_key);
        }
        nvs_commit(h);
        nvs_close(h);
    }

    // 2. Đồng bộ legacy namespaces để đảm bảo tương thích ngược tuyệt đối
    if (nvs_open(NVS_NS_LEGACY_1, NVS_READWRITE, &h) == ESP_OK) {
        if (registered) nvs_set_u8(h, desc->nvs_reg_key, 1);
        else nvs_erase_key(h, desc->nvs_reg_key);
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open(NVS_NS_LEGACY_2, NVS_READWRITE, &h) == ESP_OK) {
        if (registered) nvs_set_u8(h, desc->nvs_reg_key, 1);
        else nvs_erase_key(h, desc->nvs_reg_key);
        nvs_commit(h);
        nvs_close(h);
    }
    if (!registered && nvs_open(NVS_NS_LEGACY_3, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, desc->nvs_ip_key);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Device [%s] registration NVS persisted: %d", desc->name, registered);
}

static void deferred_nvs_save_reg(void *pvParameter1, uint32_t ulParameter2) {
    (void)ulParameter2;
    uint32_t packed = (uint32_t)(uintptr_t)pvParameter1;
    thread_dev_type_t type = (thread_dev_type_t)(packed & 0xFFFF);
    bool registered = (bool)((packed >> 16) & 0x01);
    nvs_persist_reg(type, registered);
}

void device_registry_set_type_registered(thread_dev_type_t type, bool registered)
{
    if (type < 0 || type >= THREAD_DEV_COUNT) return;
    device_registry_init();

    // Cập nhật RAM ngay lập tức dưới mutex để UI / module khác đọc được trạng thái tức thì
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        s_entries[type].registered = registered;
        s_entries[type].deleted_runtime = !registered;
        if (!registered) {
            s_entries[type].last_seen_ms = 0;
            s_entries[type].has_cached_ip = false;
        }
        xSemaphoreGive(s_reg_mutex);
    }

    // Chỉ hoãn phần ghi/xóa NVS sang Timer Service task nếu stack caller nằm trong PSRAM
    if (esp_ptr_external_ram(&type)) {
        uint32_t packed = ((uint32_t)type & 0xFFFF) | ((registered ? 1 : 0) << 16);
        if (xTimerPendFunctionCall(deferred_nvs_save_reg, (void *)(uintptr_t)packed, 0, pdMS_TO_TICKS(100)) != pdPASS) {
            ESP_LOGW(TAG, "xTimerPendFunctionCall failed to defer NVS reg save");
        }
    } else {
        nvs_persist_reg(type, registered);
    }
}

void device_registry_set_registered(const char *dev_name, bool registered)
{
    thread_dev_type_t type = device_registry_str_to_type(dev_name);
    if (type != THREAD_DEV_UNKNOWN) {
        device_registry_set_type_registered(type, registered);
    }
}

void device_registry_get_registered(bool *out_relay, bool *out_dimmer, bool *out_sensor)
{
    if (out_relay)  *out_relay  = device_registry_is_type_registered(THREAD_DEV_RELAY);
    if (out_dimmer) *out_dimmer = device_registry_is_type_registered(THREAD_DEV_DIMMER);
    if (out_sensor) *out_sensor = device_registry_is_type_registered(THREAD_DEV_SENSOR);
}

void device_registry_record_type_activity(thread_dev_type_t type)
{
    if (type < 0 || type >= THREAD_DEV_COUNT) return;
    device_registry_init();

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        s_entries[type].last_seen_ms = now_ms;
        s_entries[type].deleted_runtime = false;
        xSemaphoreGive(s_reg_mutex);
    }
}

void device_registry_record_activity(const char *dev_name)
{
    thread_dev_type_t type = device_registry_str_to_type(dev_name);
    if (type != THREAD_DEV_UNKNOWN) {
        device_registry_record_type_activity(type);
    }
}

void device_registry_mark_type_offline(thread_dev_type_t type)
{
    if (type < 0 || type >= THREAD_DEV_COUNT) return;
    device_registry_init();
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        s_entries[type].last_seen_ms = 0;
        xSemaphoreGive(s_reg_mutex);
        ESP_LOGI(TAG, "Device [%s] explicitly marked OFFLINE (last_seen reset)", device_registry_type_to_str(type));
    }
}

void device_registry_mark_offline(const char *dev_name)
{
    thread_dev_type_t type = device_registry_str_to_type(dev_name);
    if (type != THREAD_DEV_UNKNOWN) {
        device_registry_mark_type_offline(type);
    }
}

int64_t device_registry_get_last_seen_ms(thread_dev_type_t type)
{
    if (type < 0 || type >= THREAD_DEV_COUNT) return 0;
    device_registry_init();
    int64_t val = 0;
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        val = s_entries[type].last_seen_ms;
        xSemaphoreGive(s_reg_mutex);
    }
    return val;
}

void device_registry_update_ip(thread_dev_type_t type, const otIp6Address *addr)
{
    if (type < 0 || type >= THREAD_DEV_COUNT || addr == NULL) return;
    device_registry_init();

    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        bool ip_changed = (!s_entries[type].has_cached_ip ||
                           memcmp(&s_entries[type].cached_ip, addr, sizeof(otIp6Address)) != 0);
        s_entries[type].cached_ip = *addr;
        s_entries[type].has_cached_ip = true;
        s_entries[type].last_seen_ms = esp_timer_get_time() / 1000;
        s_entries[type].deleted_runtime = false;

        const thread_dev_desc_t *desc = &s_device_descs[type];
        xSemaphoreGive(s_reg_mutex);

        if (ip_changed) {
            schedule_or_run_nvs_ip_save(desc->nvs_ip_key, addr, esp_ptr_external_ram(&type));
            ESP_LOGI(TAG, "Device [%s] IP cache updated from live packet", desc->name);
        }
    }
}

bool device_registry_find_fresh_locked(
    otInstance   *instance,
    const char   *instance_label,
    const char   *service_type,
    uint32_t      max_age_ms,
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

        // Kiểm tra tuổi đăng ký của host nếu max_age_ms > 0
        if (max_age_ms > 0) {
            otSrpServerLeaseInfo lease_info;
            otSrpServerHostGetLeaseInfo(host, &lease_info);
            // mLease và mRemainingLease trong OpenThread tính theo GIÂY -> quy đổi ra ms
            uint32_t age_sec = (lease_info.mLease >= lease_info.mRemainingLease)
                                ? (lease_info.mLease - lease_info.mRemainingLease)
                                : 0;
            uint32_t age_ms = age_sec * 1000;
            if (age_ms > max_age_ms) {
                ESP_LOGD(TAG, "Bỏ qua SRP Host vì bản ghi cũ (age=%lu ms > max=%lu ms)",
                         (unsigned long)age_ms, (unsigned long)max_age_ms);
                continue;
            }
        }

        const otSrpServerService *service = NULL;
        while ((service = otSrpServerHostGetNextService(host, service)) != NULL) {
            if (otSrpServerServiceIsDeleted(service)) {
                continue;
            }

            const char *service_name = otSrpServerServiceGetServiceName(service);
            if (service_name == NULL || strstr(service_name, service_type) == NULL) {
                continue;
            }

            const char *label = otSrpServerServiceGetInstanceLabel(service);
            if (instance_label != NULL && label != NULL) {
                if (strcasecmp(label, instance_label) != 0 && strstr(label, instance_label) == NULL) {
                    continue;
                }
            }

            uint16_t port = otSrpServerServiceGetPort(service);
            if (port == 0) {
                port = 5683;
            }

            uint8_t addr_num = 0;
            const otIp6Address *addrs = otSrpServerHostGetAddresses(host, &addr_num);
            if (addrs == NULL || addr_num < 1) {
                continue;
            }

            *out_addr = addrs[0];
            *out_port = port;
            return true;
        }
    }
    return false;
}

bool device_registry_find_locked(
    otInstance   *instance,
    const char   *instance_label,
    const char   *service_type,
    otIp6Address *out_addr,
    uint16_t     *out_port)
{
    return device_registry_find_fresh_locked(instance, instance_label, service_type, 0, out_addr, out_port);
}

esp_err_t device_registry_resolve_address_locked(
    otInstance        *instance,
    thread_dev_type_t  type,
    otIp6Address      *out_addr,
    uint16_t          *out_port)
{
    if (type < 0 || type >= THREAD_DEV_COUNT || out_addr == NULL || out_port == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    device_registry_init();

    if (s_entries[type].deleted_runtime) {
        return ESP_ERR_NOT_FOUND;
    }

    const thread_dev_desc_t *desc = &s_device_descs[type];
    int64_t now_ms = esp_timer_get_time() / 1000;

    // 1. UU TIEN SO 1: Kiem tra cached_ip vua nhan tu ban tin /report trong 45s qua
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_entries[type].has_cached_ip && (now_ms - s_entries[type].last_seen_ms < 45000)) {
            *out_addr = s_entries[type].cached_ip;
            *out_port = desc->default_port;
            xSemaphoreGive(s_reg_mutex);
            return ESP_OK;
        }
        xSemaphoreGive(s_reg_mutex);
    }

    // 2. UU TIEN SO 2: Tim kiem trong bang SRP Server hien tai
    if (instance != NULL &&
        (device_registry_find_locked(instance, desc->srp_label, desc->srp_service, out_addr, out_port) ||
         device_registry_find_locked(instance, desc->name, desc->srp_service, out_addr, out_port) ||
         device_registry_find_locked(instance, NULL, desc->srp_service, out_addr, out_port))) {

        // Cập nhật RAM IP cache
        if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
            bool ip_changed = (!s_entries[type].has_cached_ip ||
                               memcmp(&s_entries[type].cached_ip, out_addr, sizeof(otIp6Address)) != 0);
            s_entries[type].cached_ip = *out_addr;
            s_entries[type].has_cached_ip = true;

            if (ip_changed) {
                schedule_or_run_nvs_ip_save(desc->nvs_ip_key, out_addr, esp_ptr_external_ram(&type));
            }
            xSemaphoreGive(s_reg_mutex);
        }
        return ESP_OK;
    }

    // 2. Fallback sử dụng IP Cache đã lưu trong RAM/NVS
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_entries[type].has_cached_ip) {
            *out_addr = s_entries[type].cached_ip;
            *out_port = desc->default_port;
            xSemaphoreGive(s_reg_mutex);
            return ESP_OK;
        }
        xSemaphoreGive(s_reg_mutex);
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t device_registry_find_relay_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port)
{
    return device_registry_resolve_address_locked(instance, THREAD_DEV_RELAY, out_addr, out_port);
}

esp_err_t device_registry_find_dimmer_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port)
{
    return device_registry_resolve_address_locked(instance, THREAD_DEV_DIMMER, out_addr, out_port);
}

esp_err_t device_registry_find_sensor_locked(otInstance *instance, otIp6Address *out_addr, uint16_t *out_port)
{
    return device_registry_resolve_address_locked(instance, THREAD_DEV_SENSOR, out_addr, out_port);
}

bool device_registry_is_type_online(otInstance *instance, thread_dev_type_t type)
{
    if (type < 0 || type >= THREAD_DEV_COUNT) return false;
    device_registry_init();

    if (!device_registry_is_type_registered(type)) {
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    bool is_online = false;

    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_entries[type].deleted_runtime) {
            xSemaphoreGive(s_reg_mutex);
            return false;
        }

        // 1. Kiem tra hoat dong truyen thong gan nhat tu ban tin /report thuc te
        int64_t timeout_limit_ms = (type == THREAD_DEV_SENSOR) ? SENSOR_PRESENCE_TIMEOUT_MS : ACTUATOR_PRESENCE_TIMEOUT_MS;
        extern bool ai_edge_is_busy(void);
        bool edge_busy = ai_edge_is_busy();
        if (s_entries[type].last_seen_ms > 0 &&
            (edge_busy || (now_ms - s_entries[type].last_seen_ms < timeout_limit_ms))) {
            is_online = true;
        }
        xSemaphoreGive(s_reg_mutex);
    }

    return is_online;
}

bool device_registry_is_node_online(otInstance *instance, const char *dev_name)
{
    thread_dev_type_t type = device_registry_str_to_type(dev_name);
    return (type != THREAD_DEV_UNKNOWN) ? device_registry_is_type_online(instance, type) : false;
}

uint8_t device_registry_count_active_locked(otInstance *instance)
{
    uint8_t count = 0;
    for (int i = 0; i < THREAD_DEV_COUNT; i++) {
        if (device_registry_is_type_online(instance, (thread_dev_type_t)i)) {
            count++;
        }
    }
    return count;
}

bool device_registry_has_cached_device(const char *dev_name)
{
    thread_dev_type_t type = device_registry_str_to_type(dev_name);
    if (type == THREAD_DEV_UNKNOWN) return false;
    device_registry_init();

    bool has_ip = false;
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        has_ip = !s_entries[type].deleted_runtime && s_entries[type].has_cached_ip;
        xSemaphoreGive(s_reg_mutex);
    }
    return has_ip;
}

void device_registry_clear_cache(const char *dev_name)
{
    device_registry_init();
    if (dev_name == NULL || strcasecmp(dev_name, "all") == 0) {
        device_registry_clear_all();
        return;
    }

    thread_dev_type_t type = device_registry_str_to_type(dev_name);
    if (type != THREAD_DEV_UNKNOWN) {
        device_registry_set_type_registered(type, false);
    }
}

static void nvs_clear_all_namespaces(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS_UNIFIED, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open(NVS_NS_LEGACY_1, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open(NVS_NS_LEGACY_2, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open(NVS_NS_LEGACY_3, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Cleared ALL device registries & caches from NVS");
}

static void deferred_nvs_clear_all(void *pvParameter1, uint32_t ulParameter2) {
    (void)pvParameter1;
    (void)ulParameter2;
    nvs_clear_all_namespaces();
}

void device_registry_clear_all(void)
{
    device_registry_init();
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < THREAD_DEV_COUNT; i++) {
            s_entries[i].registered = false;
            s_entries[i].deleted_runtime = true;
            s_entries[i].last_seen_ms = 0;
            s_entries[i].has_cached_ip = false;
        }
        xSemaphoreGive(s_reg_mutex);
    }

    int stack_marker = 0;
    if (esp_ptr_external_ram(&stack_marker)) {
        if (xTimerPendFunctionCall(deferred_nvs_clear_all, NULL, 0, pdMS_TO_TICKS(100)) != pdPASS) {
            ESP_LOGW(TAG, "xTimerPendFunctionCall failed to defer NVS clear_all");
        }
    } else {
        nvs_clear_all_namespaces();
    }
}
