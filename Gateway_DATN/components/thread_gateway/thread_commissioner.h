#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMMISSIONER_DEV_RELAY,
    COMMISSIONER_DEV_DIMMER,
    COMMISSIONER_DEV_SENSOR,
    COMMISSIONER_DEV_UNKNOWN
} commissioner_dev_type_t;

typedef void (*commissioner_discovery_cb_t)(commissioner_dev_type_t type, const char *name, const char *id_str, int8_t rssi);

esp_err_t thread_commissioner_init(void);
esp_err_t thread_commissioner_start_scan(uint32_t timeout_sec);
esp_err_t thread_commissioner_stop(void);
bool      thread_commissioner_is_scanning(void);
uint32_t  thread_commissioner_get_remaining_sec(void);

void      thread_commissioner_set_discovery_callback(commissioner_discovery_cb_t cb);
esp_err_t thread_commissioner_approve_device(const char *dev_id_str);
esp_err_t thread_commissioner_remove_device(const char *dev_id_str);
bool      thread_commissioner_is_device_approved(const char *dev_id_str);

#ifdef __cplusplus
}
#endif
