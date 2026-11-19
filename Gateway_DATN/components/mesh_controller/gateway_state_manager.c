#include "gateway_state_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "gateway_state";

static gateway_device_state_t s_device_state = {
    .relay_node_online = false,
    .dimmer_node_online = false,
    .sensor_node_online = false,
    .mesh_node_count = 0,
    .dimmer_on = false,
    .dimmer_brightness = 0,
    .dimmer_last_brightness = 80,
    .relay_on = false,
    .power_watts = 0.0f,
    .temperature = 0.0f,
    .humidity = 0.0f,
    .light_lux = 0,
    .sensor_valid = false,
};

static SemaphoreHandle_t s_state_mutex = NULL;

#define MAX_STATE_CALLBACKS 4
static gateway_state_change_cb_t s_change_cbs[MAX_STATE_CALLBACKS] = {0};
static uint8_t s_num_change_cbs = 0;

static void notify_state_changed(const gateway_device_state_t *state) {
    for (int i = 0; i < s_num_change_cbs; i++) {
        if (s_change_cbs[i] != NULL) {
            s_change_cbs[i](state);
        }
    }
}

void gateway_state_manager_init(void) {
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "Gateway State Manager initialized (Dimmer, Relay 1CH, DHT11)");
}

uint8_t gateway_state_get_active_node_count(void) {
    uint8_t count = 0;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_device_state.relay_node_online) count++;
        if (s_device_state.dimmer_node_online) count++;
        if (s_device_state.sensor_node_online) count++;
        xSemaphoreGive(s_state_mutex);
    }
    return count;
}

bool gateway_state_is_relay_online(void) {
    bool online = false;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        online = s_device_state.relay_node_online;
        xSemaphoreGive(s_state_mutex);
    }
    return online;
}

bool gateway_state_is_dimmer_online(void) {
    bool online = false;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        online = s_device_state.dimmer_node_online;
        xSemaphoreGive(s_state_mutex);
    }
    return online;
}

bool gateway_state_is_sensor_online(void) {
    bool online = false;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        online = s_device_state.sensor_node_online;
        xSemaphoreGive(s_state_mutex);
    }
    return online;
}

void gateway_state_get(gateway_device_state_t *out_state) {
    if (!out_state) return;
    if (s_state_mutex == NULL) {
        gateway_state_manager_init();
    }
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(out_state, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    } else {
        memcpy(out_state, &s_device_state, sizeof(gateway_device_state_t));
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
        notify_state_changed(&copy);
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
        notify_state_changed(&copy);
    }
}

void gateway_state_set_sensor(float temperature, float humidity) {
    bool changed = false;
    gateway_device_state_t copy;

    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (!s_device_state.sensor_valid ||
            s_device_state.temperature != temperature ||
            s_device_state.humidity != humidity) {
            changed = true;
            s_device_state.temperature = temperature;
            s_device_state.humidity = humidity;
            s_device_state.sensor_valid = true;
        }
        memcpy(&copy, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    }

    if (changed) {
        ESP_LOGI(TAG, "Sensor DHT11 Updated: Temp=%.1f°C, Humidity=%.1f%%", temperature, humidity);
        notify_state_changed(&copy);
    }
}

void gateway_state_set_node_presence(bool relay_online, bool dimmer_online, bool sensor_online) {
    bool changed = false;
    gateway_device_state_t copy;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_device_state.relay_node_online != relay_online ||
            s_device_state.dimmer_node_online != dimmer_online ||
            s_device_state.sensor_node_online != sensor_online) {
            changed = true;
            s_device_state.relay_node_online = relay_online;
            s_device_state.dimmer_node_online = dimmer_online;
            s_device_state.sensor_node_online = sensor_online;
        }
        memcpy(&copy, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    }
    if (changed) {
        ESP_LOGI(TAG, "Mesh Node Presence Changed: Relay=%d, Dimmer=%d, Sensor=%d",
                 relay_online, dimmer_online, sensor_online);
        notify_state_changed(&copy);
    }
}

void gateway_state_set_mesh_count(uint8_t count) {
    bool changed = false;
    gateway_device_state_t copy;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_device_state.mesh_node_count != count) {
            changed = true;
            s_device_state.mesh_node_count = count;
        }
        memcpy(&copy, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    }
    if (changed) {
        notify_state_changed(&copy);
    }
}

void gateway_state_set_power(float watts) {
    gateway_device_state_t copy;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_device_state.power_watts = watts;
        memcpy(&copy, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    }
    notify_state_changed(&copy);
}

void gateway_state_set_light(uint16_t lux) {
    gateway_device_state_t copy;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_device_state.light_lux = lux;
        memcpy(&copy, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    }
    notify_state_changed(&copy);
}

void gateway_state_register_callback(gateway_state_change_cb_t cb) {
    if (cb == NULL) return;
    for (int i = 0; i < s_num_change_cbs; i++) {
        if (s_change_cbs[i] == cb) {
            return; // Đã đăng ký rồi
        }
    }
    if (s_num_change_cbs < MAX_STATE_CALLBACKS) {
        s_change_cbs[s_num_change_cbs++] = cb;
        ESP_LOGI(TAG, "Registered state callback subscriber #%d", s_num_change_cbs);
    } else {
        ESP_LOGW(TAG, "Max state callbacks reached (%d)", MAX_STATE_CALLBACKS);
    }
}
