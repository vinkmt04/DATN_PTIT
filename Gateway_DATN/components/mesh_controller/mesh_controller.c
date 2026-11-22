#include "mesh_controller.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "thread_gateway.h"
#include "thread_commissioner.h"
#include "device_registry.h"
#include "sensor_client.h"
#include "sys_core.h"

static const char *TAG = "mesh_controller";

// Bộ đệm bảo vệ chống nhảy trạng thái (In-flight command guard):
// Bỏ qua các bản tin heartbeat phản ánh trạng thái cũ đến trong vòng 2.5s sau khi Gateway vừa gửi lệnh điều khiển
static int64_t s_relay_cmd_in_flight_until_ms = 0;
static bool    s_relay_target_state = false;

static int64_t s_dimmer_cmd_in_flight_until_ms = 0;
static uint8_t s_dimmer_target_bri = 0;

// Callback nhận dữ liệu DHT11 từ sensor_client (Thread CoAP)
static void on_thread_sensor_snapshot(const sensor_client_data_t *data) {
    if (!data) return;

    if (data->has_valid_sample && (data->state == SENSOR_STATE_GOOD)) {
        device_registry_record_activity("sensor");
        thread_commissioner_notify_report_received(COMMISSIONER_DEV_SENSOR);
        if (gateway_state_is_sensor_registered()) {
            gateway_state_set_sensor(data->temperature_c, data->humidity_pct);
            gateway_state_set_node_presence(
                gateway_state_is_relay_online(),
                gateway_state_is_dimmer_online(),
                true
            );
        }
        ESP_LOGI(TAG, "Sensor DHT11 updated from Thread Mesh: Temp=%.1f C, Hum=%.1f %%",
                 data->temperature_c, data->humidity_pct);
    } else if (data->state == SENSOR_STATE_TIMEOUT || data->state == SENSOR_STATE_RESP_ERROR) {
        if (gateway_state_is_sensor_registered()) {
            gateway_state_set_node_presence(
                gateway_state_is_relay_online(),
                gateway_state_is_dimmer_online(),
                false
            );
        }
        ESP_LOGW(TAG, "Sensor node unreachable or timed out");
    }
}

// Callback nhận phản hồi từ actuator_client (Thread CoAP ACK / Response)
static void on_thread_actuator_event(actuator_device_t device, actuator_status_t status, int confirmed_value) {
    if (status != ACTUATOR_CONFIRMED) {
        ESP_LOGW(TAG, "Actuator event error/timeout: dev=%d status=%d", device, status);
        return;
    }

    if (device == ACTUATOR_RELAY) {
        bool on = (confirmed_value == 1);
        s_relay_cmd_in_flight_until_ms = 0; // Đã nhận xác nhận từ node, kết thúc guard
        device_registry_record_activity("relay");
        if (gateway_state_is_relay_registered()) {
            gateway_state_set_relay(on);
            gateway_state_set_node_presence(true, gateway_state_is_dimmer_online(), gateway_state_is_sensor_online());
        }
        ESP_LOGI(TAG, "Relay state confirmed from Thread Mesh: %s", on ? "ON" : "OFF");
    } else if (device == ACTUATOR_DIMMER) {
        bool on = (confirmed_value > 0);
        s_dimmer_cmd_in_flight_until_ms = 0; // Đã nhận xác nhận từ node, kết thúc guard
        device_registry_record_activity("dimmer");
        if (gateway_state_is_dimmer_registered()) {
            gateway_state_set_dimmer(on, (uint8_t)confirmed_value);
            gateway_state_set_node_presence(gateway_state_is_relay_online(), true, gateway_state_is_sensor_online());
        }
        ESP_LOGI(TAG, "Dimmer brightness confirmed from Thread Mesh: %d%%", confirmed_value);
    }
}

// Callback nhận bản tin báo cáo trạng thái tự động từ các Node (Heartbeat và State Report)
static void on_thread_report(const char *payload) {
    if (!payload) return;
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (strncmp(payload, "relay:", 6) == 0) {
        bool on = (payload[6] == '1' || strcasecmp(payload + 6, "ON") == 0);
        device_registry_record_activity("relay");
        thread_commissioner_notify_report_received(COMMISSIONER_DEV_RELAY);

        if (gateway_state_is_relay_registered()) {
            gateway_state_set_node_presence(true, gateway_state_is_dimmer_online(), gateway_state_is_sensor_online());

            // Nếu vừa gửi lệnh điều khiển mà bản tin heartbeat cũ đang bay trên mạng mang giá trị đối nghịch:
            if (now_ms < s_relay_cmd_in_flight_until_ms && on != s_relay_target_state) {
                ESP_LOGD(TAG, "Bỏ qua bản tin relay cũ đang bay (reported=%d, target=%d)", on, s_relay_target_state);
            } else {
                s_relay_cmd_in_flight_until_ms = 0;
                gateway_state_set_relay(on);
                ESP_LOGD(TAG, "Node Relay reported state: %s", on ? "ON" : "OFF");
            }
        }
    } else if (strncmp(payload, "dimmer:", 7) == 0) {
        int bri = atoi(payload + 7);
        if (bri < 0) bri = 0;
        if (bri > 100) bri = 100;
        device_registry_record_activity("dimmer");
        thread_commissioner_notify_report_received(COMMISSIONER_DEV_DIMMER);

        if (gateway_state_is_dimmer_registered()) {
            gateway_state_set_node_presence(gateway_state_is_relay_online(), true, gateway_state_is_sensor_online());

            // Nếu vừa kéo slider hoặc bật tắt đèn mà heartbeat cũ gửi giá trị khác:
            if (now_ms < s_dimmer_cmd_in_flight_until_ms && abs(bri - (int)s_dimmer_target_bri) > 5) {
                ESP_LOGD(TAG, "Bỏ qua bản tin dimmer cũ đang bay (reported=%d, target=%d)", bri, s_dimmer_target_bri);
            } else {
                s_dimmer_cmd_in_flight_until_ms = 0;
                gateway_state_set_dimmer(bri > 0, (uint8_t)bri);
                ESP_LOGD(TAG, "Node Dimmer reported state: %d%%", bri);
            }
        }
    } else if (strcmp(payload, "sensor:hb") == 0 || strcmp(payload, "sensor") == 0) {
        ESP_LOGD(TAG, "Node Sensor heartbeat received");
        device_registry_record_activity("sensor");
        thread_commissioner_notify_report_received(COMMISSIONER_DEV_SENSOR);
        if (gateway_state_is_sensor_registered()) {
            gateway_state_set_node_presence(gateway_state_is_relay_online(), gateway_state_is_dimmer_online(), true);
        }
    } else if (strncmp(payload, "sensor:data:", 12) == 0 || strncmp(payload, "temp:", 5) == 0) {
        float temp = 0.0f, hum = 0.0f;
        const char *data_str = (strncmp(payload, "sensor:data:", 12) == 0) ? payload + 12 : payload;
        if (sscanf(data_str, "%f,%f", &temp, &hum) == 2 ||
            sscanf(data_str, "temp:%f,hum:%f", &temp, &hum) == 2) {
            ESP_LOGI(TAG, "Node Sensor DHT11 Report received: Temp=%.1f°C, Hum=%.1f%% -> Syncing State & Telemetry", temp, hum);
            device_registry_record_activity("sensor");
            thread_commissioner_notify_report_received(COMMISSIONER_DEV_SENSOR);
            if (gateway_state_is_sensor_registered()) {
                gateway_state_set_sensor(temp, hum);
                gateway_state_set_node_presence(gateway_state_is_relay_online(), gateway_state_is_dimmer_online(), true);
            }
        }
    }
}

// Task định kỳ giám sát sự hiện diện của các Node Thread Mesh qua SRP Server & Health Check
static void mesh_monitor_task(void *pvParameters) {
    ESP_LOGI(TAG, "Thread Mesh Presence Monitor Task started.");

    // Chờ 1.5s cho OpenThread stack và RCP khởi tạo ổn định, chủ động Probe ngay các Node đã lưu trong NVS
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (gateway_state_is_relay_registered()) {
        ESP_LOGI(TAG, ">>> Proactive Boot Probe: Relay registered -> Sending CoAP GET /relay <<<");
        actuator_read_relay();
    }
    if (gateway_state_is_dimmer_registered()) {
        ESP_LOGI(TAG, ">>> Proactive Boot Probe: Dimmer registered -> Sending CoAP GET /light <<<");
        actuator_read_brightness();
    }
    if (gateway_state_is_sensor_registered()) {
        ESP_LOGI(TAG, ">>> Proactive Boot Probe: Sensor registered -> Polling DHT11 telemetry <<<");
        sensor_client_poll_now();
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        int64_t now_ms = esp_timer_get_time() / 1000;

        // Chủ động Probe Actuator nếu mới khởi động (last_seen == 0) hoặc quá 15s không có tín hiệu
        extern bool ai_edge_is_busy(void);
        if (!ai_edge_is_busy()) {
            if (gateway_state_is_relay_registered()) {
                int64_t last_r = device_registry_get_last_seen_ms(THREAD_DEV_RELAY);
                if (last_r == 0 || (now_ms - last_r >= 15000)) {
                    actuator_read_relay();
                }
            }
            if (gateway_state_is_dimmer_registered()) {
                int64_t last_d = device_registry_get_last_seen_ms(THREAD_DEV_DIMMER);
                if (last_d == 0 || (now_ms - last_d >= 15000)) {
                    actuator_read_brightness();
                }
            }
            if (gateway_state_is_sensor_registered()) {
                int64_t last_s = device_registry_get_last_seen_ms(THREAD_DEV_SENSOR);
                if (last_s == 0 || (now_ms - last_s >= 20000)) {
                    sensor_client_poll_now();
                }
            }
        }

        bool relay_on  = thread_gateway_is_relay_online();
        bool dimmer_on = thread_gateway_is_dimmer_online();
        bool sensor_on = thread_gateway_is_sensor_online();
        uint8_t count  = thread_gateway_get_node_count();

        gateway_state_set_node_presence(relay_on, dimmer_on, sensor_on);
        gateway_state_set_mesh_count(count);

        static uint32_t s_mem_log_cycle = 0;
        if (++s_mem_log_cycle >= 30) { // Mỗi 60 giây in audit bộ nhớ
            s_mem_log_cycle = 0;
            sys_core_print_memory_stats("RUNTIME_HEALTH");
        }
    }
}

void mesh_controller_init(void) {
    ESP_LOGI(TAG, "Initializing Mesh Controller (OpenThread Host on ESP32-S3)...");

    // 1. Khởi tạo OpenThread Border Router Host stack (Spinel UART TX:18 RX:14 @ 460800bps tới H2 RCP)
    esp_err_t ret = thread_gateway_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "thread_gateway_init failed: %d", ret);
        return;
    }

    // 3. Đăng ký callback nhận dữ liệu cảm biến và thiết bị chấp hành
    thread_gateway_set_sensor_callback(on_thread_sensor_snapshot);
    thread_gateway_set_actuator_callback(on_thread_actuator_event);
    thread_gateway_set_report_callback(on_thread_report);

    // 4. Tạo Task giám sát định kỳ sự hiện diện của các node trong Mesh (Stack trong PSRAM để tiết kiệm DRAM)
    xTaskCreatePinnedToCoreWithCaps(mesh_monitor_task, "mesh_mon_task", 4096, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "Mesh Controller initialized successfully!");
}

void mesh_controller_send_command(const char *cmd_str) {
    if (cmd_str == NULL) return;

    char clean_cmd[128];
    const char *src = cmd_str;

    // Loại bỏ dấu ngoặc nhọn nếu có: <CMD:...> -> CMD:...
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
    if (!gateway_state_is_dimmer_registered()) {
        ESP_LOGW(TAG, "Cannot control Dimmer: not registered in NVS");
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (on) {
        if (brightness == 0) {
            gateway_device_state_t st;
            gateway_state_get(&st);
            brightness = st.dimmer_last_brightness > 0 ? st.dimmer_last_brightness : 80;
        }
        s_dimmer_cmd_in_flight_until_ms = now_ms + 2500;
        s_dimmer_target_bri = brightness;
        gateway_state_set_dimmer(true, brightness);
        thread_gateway_set_dimmer(brightness);
    } else {
        s_dimmer_cmd_in_flight_until_ms = now_ms + 2500;
        s_dimmer_target_bri = 0;
        gateway_state_set_dimmer(false, 0);
        thread_gateway_set_dimmer(0);
    }
}

void mesh_controller_set_relay(bool on) {
    if (!gateway_state_is_relay_registered()) {
        ESP_LOGW(TAG, "Cannot control Relay: not registered in NVS");
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    s_relay_cmd_in_flight_until_ms = now_ms + 2500;
    s_relay_target_state = on;
    gateway_state_set_relay(on);
    thread_gateway_set_relay(on);
}
