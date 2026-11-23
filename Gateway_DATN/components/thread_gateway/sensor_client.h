#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SENSOR_STATE_NO_DATA,       /* Chua co du lieu */
    SENSOR_STATE_GOOD,          /* Du lieu tot */
    SENSOR_STATE_READ_ERROR,    /* Loi doc cam bien */
    SENSOR_STATE_STALE,         /* Du lieu cu */
    SENSOR_STATE_RESP_ERROR,    /* Loi phan hoi CoAP */
    SENSOR_STATE_TIMEOUT,       /* Mat lien lac */
} sensor_client_state_t;

typedef struct {
    bool                  has_valid_sample;
    char                  node_id[32];
    float                 temperature_c;
    float                 humidity_pct;
    uint32_t              sample_seq;
    int64_t               age_ms;
    int64_t               last_rx_time_ms;
    bool                  last_read_ok;
    bool                  stale;
    sensor_client_state_t state;
} sensor_client_data_t;

typedef void (*sensor_callback_t)(const sensor_client_data_t *data);

esp_err_t sensor_client_start(void);
esp_err_t sensor_client_poll_now(void);
esp_err_t sensor_client_get_snapshot(sensor_client_data_t *out);
esp_err_t sensor_send_unpair(void);
void      sensor_client_set_callback(sensor_callback_t cb);
void      sensor_client_notify_report_rx(float temp, float hum, bool is_data);

#ifdef __cplusplus
}
#endif
