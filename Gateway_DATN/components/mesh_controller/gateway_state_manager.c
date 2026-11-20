#include "gateway_state_manager.h"
#include "device_registry.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "gateway_state";

#define NVS_NS_STATE        "gw_dev_reg"
#define NVS_KEY_REG_RELAY   "reg_relay"
#define NVS_KEY_REG_DIMMER  "reg_dimmer"
#define NVS_KEY_REG_SENSOR  "reg_sensor"
#define NVS_KEY_LAST_BRI    "last_bri"

static gateway_device_state_t s_device_state = {
    .relay_registered = false,
    .dimmer_registered = false,
    .sensor_registered = false,
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

static void save_last_brightness_nvs(uint8_t bri) {
    if (bri == 0) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_STATE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_LAST_BRI, bri);
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint8_t load_last_brightness_nvs(void) {
    nvs_handle_t h;
    uint8_t bri = 80;
    if (nvs_open(NVS_NS_STATE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_LAST_BRI, &bri);
        nvs_close(h);
    }
    if (bri == 0 || bri > 100) bri = 80;
    return bri;
}

void gateway_state_manager_init(void) {
    if (s_state_mutex == NULL) {
        s_state_mutex = xSemaphoreCreateMutex();
    }

    // Đọc trạng thái đã đăng ký (enrolled) từ NVS
    nvs_handle_t h;
    uint8_t reg_r = 0, reg_d = 0, reg_s = 0;
    if (nvs_open(NVS_NS_STATE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_KEY_REG_RELAY, &reg_r);
        nvs_get_u8(h, NVS_KEY_REG_DIMMER, &reg_d);
        nvs_get_u8(h, NVS_KEY_REG_SENSOR, &reg_s);
        nvs_close(h);
    }

    // Tương thích ngược: nếu chưa có cờ trong gw_dev_reg, kiểm tra dev_registry cache
    if (!reg_r && device_registry_has_cached_device("relay")) reg_r = 1;
    if (!reg_d && device_registry_has_cached_device("dimmer")) reg_d = 1;
    if (!reg_s && device_registry_has_cached_device("sensor")) reg_s = 1;

    uint8_t last_bri = load_last_brightness_nvs();

    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_device_state.relay_registered = (reg_r != 0);
        s_device_state.dimmer_registered = (reg_d != 0);
        s_device_state.sensor_registered = (reg_s != 0);

        // Ban đầu sau khi khởi động: Các node tạm thời Offline cho đến khi mesh_monitor xác nhận
        s_device_state.relay_node_online = false;
        s_device_state.dimmer_node_online = false;
        s_device_state.sensor_node_online = false;
        s_device_state.mesh_node_count = 0;

        s_device_state.dimmer_last_brightness = last_bri;
        xSemaphoreGive(s_state_mutex);
    }

    ESP_LOGI(TAG, "Gateway State Manager initialized (Registered Nodes: Relay=%d, Dimmer=%d, Sensor=%d, LastBri=%u%%)",
             reg_r != 0, reg_d != 0, reg_s != 0, last_bri);
}

uint8_t gateway_state_get_registered_node_count(void) {
    uint8_t count = 0;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_device_state.relay_registered) count++;
        if (s_device_state.dimmer_registered) count++;
        if (s_device_state.sensor_registered) count++;
        xSemaphoreGive(s_state_mutex);
    }
    return count;
}

bool gateway_state_is_relay_registered(void) {
    bool reg = false;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        reg = s_device_state.relay_registered;
        xSemaphoreGive(s_state_mutex);
    }
    return reg;
}

bool gateway_state_is_dimmer_registered(void) {
    bool reg = false;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        reg = s_device_state.dimmer_registered;
        xSemaphoreGive(s_state_mutex);
    }
    return reg;
}

bool gateway_state_is_sensor_registered(void) {
    bool reg = false;
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        reg = s_device_state.sensor_registered;
        xSemaphoreGive(s_state_mutex);
    }
    return reg;
}

void gateway_state_set_device_registered(const char *dev_type, bool registered) {
    if (dev_type == NULL) return;
    bool changed = false;
    gateway_device_state_t copy;

    nvs_handle_t h;
    bool nvs_ok = (nvs_open(NVS_NS_STATE, NVS_READWRITE, &h) == ESP_OK);

    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strstr(dev_type, "relay") != NULL) {
            if (s_device_state.relay_registered != registered) {
                s_device_state.relay_registered = registered;
                changed = true;
            }
            if (!registered) s_device_state.relay_node_online = false;
            if (nvs_ok) {
                if (registered) nvs_set_u8(h, NVS_KEY_REG_RELAY, 1);
                else nvs_erase_key(h, NVS_KEY_REG_RELAY);
            }
        } else if (strstr(dev_type, "dimmer") != NULL) {
            if (s_device_state.dimmer_registered != registered) {
                s_device_state.dimmer_registered = registered;
                changed = true;
            }
            if (!registered) s_device_state.dimmer_node_online = false;
            if (nvs_ok) {
                if (registered) nvs_set_u8(h, NVS_KEY_REG_DIMMER, 1);
                else nvs_erase_key(h, NVS_KEY_REG_DIMMER);
            }
        } else if (strstr(dev_type, "sensor") != NULL) {
            if (s_device_state.sensor_registered != registered) {
                s_device_state.sensor_registered = registered;
                changed = true;
            }
            if (!registered) s_device_state.sensor_node_online = false;
            if (nvs_ok) {
                if (registered) nvs_set_u8(h, NVS_KEY_REG_SENSOR, 1);
                else nvs_erase_key(h, NVS_KEY_REG_SENSOR);
            }
        }
        memcpy(&copy, &s_device_state, sizeof(gateway_device_state_t));
        xSemaphoreGive(s_state_mutex);
    }

    if (nvs_ok) {
        nvs_commit(h);
        nvs_close(h);
    }

    if (changed) {
        ESP_LOGI(TAG, "Device registration changed: dev='%s', registered=%d", dev_type, registered);
        notify_state_changed(&copy);
    }
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
                save_last_brightness_nvs(brightness);
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
