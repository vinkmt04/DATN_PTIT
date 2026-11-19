#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACTUATOR_RELAY,
    ACTUATOR_DIMMER,
} actuator_device_t;

typedef enum {
    ACTUATOR_UNKNOWN,
    ACTUATOR_PENDING,
    ACTUATOR_CONFIRMED,
    ACTUATOR_TIMEOUT,
    ACTUATOR_ERROR,
} actuator_status_t;

typedef struct {
    bool              has_confirmed_value;
    int               confirmed_value; // Relay: 0/1; dimmer: 0..100
    int               requested_value;
    actuator_status_t status;
    int64_t           last_confirmed_ms;
} actuator_snapshot_t;

typedef void (*actuator_callback_t)(actuator_device_t device, actuator_status_t status, int confirmed_value);

esp_err_t actuator_client_start(void);
esp_err_t actuator_set_relay(bool on);
esp_err_t actuator_set_brightness(int percent);
esp_err_t actuator_read_relay(void);
esp_err_t actuator_read_brightness(void);
esp_err_t actuator_get_snapshot(actuator_device_t device, actuator_snapshot_t *out);
void      actuator_client_set_callback(actuator_callback_t cb);

#ifdef __cplusplus
}
#endif
