#include "gateway_state_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "gateway_state";

static gateway_device_state_t s_device_state = {
    .dimmer_on = false,
    .dimmer_brightness = 0,
    .dimmer_last_brightness = 80,
    .relay_on = false,
    .temperature = 0.0f,
    .humidity = 0.0f,
    .sensor_valid = false,
};

static SemaphoreHandle_t s_state_mutex = NULL;
static gateway_state_change_cb_t s_change_cb = NULL;

void gateway_state_manager_init(void) {
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "Gateway State Manager initialized (Dimmer, Relay 1CH, DHT11)");
}

void gateway_state_get(gateway_device_state_t *out_state) {
    if (!out_state) return;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(out_state, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    }
}

void gateway_state_set_dimmer(bool on, uint8_t brightness) {
    bool changed = false;
    gateway_device_state_t copy;

    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_device_state.dimmer_on != on || s_device_state.dimmer_brightness != brightness) {
            changed = true;
            s_device_state.dimmer_on = on;
            s_device_state.dimmer_brightness = brightness;
            if (on && brightness > 0) {
                s_device_state.dimmer_last_brightness = brightness;
            }
        }
        memcpy(&copy, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    }

    if (changed) {
        ESP_LOGI(TAG, "Dimmer State Updated: ON=%d, Brightness=%u%% (Last=%u%%)",
                 copy.dimmer_on, copy.dimmer_brightness, copy.dimmer_last_brightness);
        if (s_change_cb) {
            s_change_cb(&copy);
        }
    }
}

void gateway_state_set_relay(bool on) {
    bool changed = false;
    gateway_device_state_t copy;

    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_device_state.relay_on != on) {
            changed = true;
            s_device_state.relay_on = on;
        }
        memcpy(&copy, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    }

    if (changed) {
        ESP_LOGI(TAG, "Relay State Updated: ON=%d", copy.relay_on);
        if (s_change_cb) {
            s_change_cb(&copy);
        }
    }
}

void gateway_state_set_sensor(float temperature, float humidity) {
    gateway_device_state_t copy;

    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_device_state.temperature = temperature;
        s_device_state.humidity = humidity;
        s_device_state.sensor_valid = true;
        memcpy(&copy, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    }

    ESP_LOGI(TAG, "Sensor DHT11 Updated: Temp=%.1f°C, Humidity=%.1f%%", temperature, humidity);
    if (s_change_cb) {
        s_change_cb(&copy);
    }
}

void gateway_state_register_callback(gateway_state_change_cb_t cb) {
    s_change_cb = cb;
}

