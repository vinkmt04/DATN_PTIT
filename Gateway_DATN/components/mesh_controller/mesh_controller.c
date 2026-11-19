#include "mesh_controller.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "thread_gateway.h"

static const char *TAG = "mesh_controller";

// Callback nhan du lieu DHT11 tu sensor_client (Thread CoAP)
static void on_thread_sensor_snapshot(const sensor_client_data_t *data) {
    if (!data) return;

    if (data->has_valid_sample && (data->state == SENSOR_STATE_GOOD)) {
        gateway_state_set_sensor(data->temperature_c, data->humidity_pct);
        gateway_state_set_node_presence(
            gateway_state_is_relay_online(),
            gateway_state_is_dimmer_online(),
            true
        );
        ESP_LOGI(TAG, "Sensor DHT11 updated from Thread Mesh: Temp=%.1f C, Hum=%.1f %%",
                 data->temperature_c, data->humidity_pct);
    } else if (data->state == SENSOR_STATE_TIMEOUT || data->state == SENSOR_STATE_RESP_ERROR) {
        gateway_state_set_node_presence(
            gateway_state_is_relay_online(),
            gateway_state_is_dimmer_online(),
            false
        );
        ESP_LOGW(TAG, "Sensor node unreachable or timed out");
    }
}

// Callback nhan phan hoi tu actuator_client (Thread CoAP)
static void on_thread_actuator_event(actuator_device_t device, actuator_status_t status, int confirmed_value) {
    if (status != ACTUATOR_CONFIRMED) {
        ESP_LOGW(TAG, "Actuator event error/timeout: dev=%d status=%d", device, status);
        return;
    }

    if (device == ACTUATOR_RELAY) {
        bool on = (confirmed_value == 1);
        gateway_state_set_relay(on);
        gateway_state_set_node_presence(true, gateway_state_is_dimmer_online(), gateway_state_is_sensor_online());
        ESP_LOGI(TAG, "Relay state confirmed from Thread Mesh: %s", on ? "ON" : "OFF");
    } else if (device == ACTUATOR_DIMMER) {
        bool on = (confirmed_value > 0);
        gateway_state_set_dimmer(on, (uint8_t)confirmed_value);
        gateway_state_set_node_presence(gateway_state_is_relay_online(), true, gateway_state_is_sensor_online());
        ESP_LOGI(TAG, "Dimmer brightness confirmed from Thread Mesh: %d%%", confirmed_value);
    }
}

// Task dinh ky giam sat su hien dien cua cac Node Thread Mesh qua SRP Server
static void mesh_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Thread Mesh Presence Monitor Task started.");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(4000));

        bool relay_on  = thread_gateway_is_relay_online();
        bool dimmer_on = thread_gateway_is_dimmer_online();
        bool sensor_on = thread_gateway_is_sensor_online();
        uint8_t count  = thread_gateway_get_node_count();

        gateway_state_set_node_presence(relay_on, dimmer_on, sensor_on);
        gateway_state_set_mesh_count(count);
    }
}

void mesh_controller_init(void) {
    ESP_LOGI(TAG, "Initializing Mesh Controller (OpenThread Host on ESP32-S3)...");

    // 1. Khoi tao Gateway State Manager trung tam
    gateway_state_manager_init();

    // 2. Khoi tao OpenThread Border Router Host stack (Spinel UART TX:18 RX:14 @ 460800bps toi H2 RCP)
    esp_err_t ret = thread_gateway_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "thread_gateway_init failed: %d", ret);
        return;
    }

    // 3. Dang ky callback nhan du lieu cam bien va thiet bi chap hanh
    thread_gateway_set_sensor_callback(on_thread_sensor_snapshot);
    thread_gateway_set_actuator_callback(on_thread_actuator_event);

    // 4. Tao Task giam sat dinh ky su hien dien cua cac node trong Mesh
    xTaskCreate(mesh_monitor_task, "mesh_mon_task", 3072, NULL, 3, NULL);

    ESP_LOGI(TAG, "Mesh Controller initialized successfully!");
}

void mesh_controller_send_command(const char *cmd_str) {
    if (cmd_str == NULL) return;

    char clean_cmd[128];
    const char *src = cmd_str;

    // Loai bo dau ngoac nhon neu co: <CMD:...> -> CMD:...
    if (src[0] == '<') src++;
    size_t len = strlen(src);
    if (len > 0 && src[len - 1] == '>') {
        len--;
    }
    if (len >= sizeof(clean_cmd)) len = sizeof(clean_cmd) - 1;
    memcpy(clean_cmd, src, len);
    clean_cmd[len] = '\0';

    ESP_LOGI(TAG, "Processing command from Voice/UI/Cloud: '%s'", clean_cmd);

    if (strcmp(clean_cmd, "CMD:LIGHT:ON") == 0 || strcmp(clean_cmd, "CMD:DIMMER:ON") == 0) {
        gateway_device_state_t st;
        gateway_state_get(&st);
        uint8_t target_bright = st.dimmer_last_brightness > 0 ? st.dimmer_last_brightness : 80;
        mesh_controller_set_dimmer(true, target_bright);
    } else if (strcmp(clean_cmd, "CMD:LIGHT:OFF") == 0 || strcmp(clean_cmd, "CMD:DIMMER:OFF") == 0) {
        mesh_controller_set_dimmer(false, 0);
    } else if (strncmp(clean_cmd, "CMD:DIMMER:SET:", 15) == 0) {
        int val = atoi(clean_cmd + 15);
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        mesh_controller_set_dimmer(val > 0, (uint8_t)val);
    } else if (strcmp(clean_cmd, "CMD:SWITCH:ON") == 0 || strcmp(clean_cmd, "CMD:RELAY:ON") == 0) {
        mesh_controller_set_relay(true);
    } else if (strcmp(clean_cmd, "CMD:SWITCH:OFF") == 0 || strcmp(clean_cmd, "CMD:RELAY:OFF") == 0) {
        mesh_controller_set_relay(false);
    } else {
        ESP_LOGW(TAG, "Unrecognized mesh command: %s", clean_cmd);
    }
}

void mesh_controller_set_dimmer(bool on, uint8_t brightness) {
    if (on) {
        if (brightness == 0) {
            gateway_device_state_t st;
            gateway_state_get(&st);
            brightness = st.dimmer_last_brightness > 0 ? st.dimmer_last_brightness : 80;
        }
        gateway_state_set_dimmer(true, brightness);
        thread_gateway_set_dimmer(brightness);
    } else {
        gateway_state_set_dimmer(false, 0);
        thread_gateway_set_dimmer(0);
    }
}

void mesh_controller_set_relay(bool on) {
    gateway_state_set_relay(on);
    thread_gateway_set_relay(on);
}
