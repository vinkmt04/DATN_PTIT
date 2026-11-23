#include "iot_cloud.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "mesh_controller.h"
#include "gateway_state_manager.h"
#include "ai_cloud.h"
#include "sys_core.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "iot_cloud";

static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static volatile bool s_is_connected = false;

static void on_gateway_state_changed(const gateway_device_state_t *new_state) {
    if (!s_is_connected || !s_mqtt_client) return;
    iot_cloud_publish_telemetry_now();
}

void iot_cloud_publish_telemetry_now(void) {
    if (!s_is_connected || !s_mqtt_client) return;

    gateway_device_state_t st;
    gateway_state_get(&st);

    static int s_last_dimmer_status = -1;
    static int s_last_dimmer_bri = -1;
    static int s_last_relay_status = -1;
    static float s_last_temp = -999.0f;
    static float s_last_hum = -999.0f;
    static int64_t s_last_pub_ms = 0;

    int64_t now_ms = esp_timer_get_time() / 1000;
    int cur_dimmer_status = st.dimmer_on ? 1 : 0;
    int cur_dimmer_bri = st.dimmer_brightness;
    int cur_relay_status = st.relay_on ? 1 : 0;

    bool state_identical = (cur_dimmer_status == s_last_dimmer_status &&
                            cur_dimmer_bri == s_last_dimmer_bri &&
                            cur_relay_status == s_last_relay_status &&
                            (st.sensor_valid ? (fabsf(st.temperature - s_last_temp) < 0.15f &&
                                                fabsf(st.humidity - s_last_hum) < 0.8f) : true));

    // Khử bản tin trùng lặp: Nếu trạng thái hoàn toàn không đổi thì không cần đẩy lại lên ThingsBoard
    if (s_last_pub_ms > 0 && state_identical) {
        return;
    }

    s_last_dimmer_status = cur_dimmer_status;
    s_last_dimmer_bri = cur_dimmer_bri;
    s_last_relay_status = cur_relay_status;
    if (st.sensor_valid) {
        s_last_temp = st.temperature;
        s_last_hum = st.humidity;
    }
    s_last_pub_ms = now_ms;

    cJSON *root = cJSON_CreateObject();
    if (!root) return;

#if GATEWAY_ENABLE_DIMMER
    cJSON_AddNumberToObject(root, "dimmer_status", st.dimmer_on ? 1 : 0);
    cJSON_AddNumberToObject(root, "dimmer_brightness", st.dimmer_brightness);
#endif

#if GATEWAY_ENABLE_RELAY
    cJSON_AddNumberToObject(root, "relay_status", st.relay_on ? 1 : 0);
#endif

#if GATEWAY_ENABLE_DHT11
    if (st.sensor_valid) {
        cJSON_AddNumberToObject(root, "temperature", (double)st.temperature);
        cJSON_AddNumberToObject(root, "humidity", (double)st.humidity);
    }
#endif

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        esp_mqtt_client_publish(s_mqtt_client, "v1/devices/me/telemetry", json_str, 0, 0, 0);
        ESP_LOGI(TAG, "Published Telemetry to ThingsBoard: %s", json_str);
        free(json_str);
    }
    cJSON_Delete(root);
}

static void handle_rpc_request(const char *topic, const char *data, size_t data_len) {
    char rpc_response_topic[64];
    const char *last_slash = strrchr(topic, '/');
    if (!last_slash) return;

    snprintf(rpc_response_topic, sizeof(rpc_response_topic), "v1/devices/me/rpc/response/%s", last_slash + 1);

    cJSON *root = cJSON_ParseWithLength(data, data_len);
    if (!root) return;

    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *params = cJSON_GetObjectItem(root, "params");

    if (cJSON_IsString(method)) {
        ESP_LOGI(TAG, "ThingsBoard RPC Call: Method='%s'", method->valuestring);

#if GATEWAY_ENABLE_DIMMER
        // 1. Điều khiển Đèn Dimmer
        if (strcmp(method->valuestring, "setDimmerStatus") == 0) {
            int status = 1;
            int brightness = 80;

            if (cJSON_IsObject(params)) {
                cJSON *s = cJSON_GetObjectItem(params, "status");
                cJSON *b = cJSON_GetObjectItem(params, "brightness");
                if (cJSON_IsNumber(s)) status = s->valueint;
                if (cJSON_IsNumber(b)) brightness = b->valueint;
            } else if (cJSON_IsNumber(params)) {
                brightness = params->valueint;
                status = (brightness > 0) ? 1 : 0;
            }

            mesh_controller_set_dimmer(status == 1, (uint8_t)brightness);
            esp_mqtt_client_publish(s_mqtt_client, rpc_response_topic, "{\"result\":\"ok\"}", 0, 1, 0);
        }
#endif
#if GATEWAY_ENABLE_RELAY
        // 2. Điều khiển Relay 1 kênh
        else if (strcmp(method->valuestring, "setRelayStatus") == 0) {
            int status = 0;
            if (cJSON_IsObject(params)) {
                cJSON *s = cJSON_GetObjectItem(params, "status");
                if (cJSON_IsNumber(s)) status = s->valueint;
            } else if (cJSON_IsBool(params)) {
                status = cJSON_IsTrue(params) ? 1 : 0;
            } else if (cJSON_IsNumber(params)) {
                status = params->valueint;
            }

            mesh_controller_set_relay(status == 1);
            esp_mqtt_client_publish(s_mqtt_client, rpc_response_topic, "{\"result\":\"ok\"}", 0, 1, 0);
        }
#endif
        // 3. Lấy trạng thái hiện tại
        else if (strcmp(method->valuestring, "getDeviceStatus") == 0) {
            gateway_device_state_t st;
            gateway_state_get(&st);
            cJSON *resp = cJSON_CreateObject();
#if GATEWAY_ENABLE_DIMMER
            cJSON_AddNumberToObject(resp, "dimmer_status", st.dimmer_on ? 1 : 0);
            cJSON_AddNumberToObject(resp, "dimmer_brightness", st.dimmer_brightness);
#endif
#if GATEWAY_ENABLE_RELAY
            cJSON_AddNumberToObject(resp, "relay_status", st.relay_on ? 1 : 0);
#endif
#if GATEWAY_ENABLE_DHT11
            if (st.sensor_valid) {
                cJSON_AddNumberToObject(resp, "temperature", st.temperature);
                cJSON_AddNumberToObject(resp, "humidity", st.humidity);
            }
#endif

            char *resp_str = cJSON_PrintUnformatted(resp);
            if (resp_str) {
                esp_mqtt_client_publish(s_mqtt_client, rpc_response_topic, resp_str, 0, 1, 0);
                free(resp_str);
            }
            cJSON_Delete(resp);
        }
    }

    cJSON_Delete(root);
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ThingsBoard MQTT: Connected successfully!");
        s_is_connected = true;

        // Báo cáo Attributes (MAC, Firmware, Device Type)
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        cJSON *attr = cJSON_CreateObject();
        if (attr) {
            cJSON_AddStringToObject(attr, "mac_address", mac_str);
            cJSON_AddStringToObject(attr, "firmware_version", "1.0.0");
            cJSON_AddStringToObject(attr, "device_type", "OTBR_SmartHome_Gateway");
            char *attr_str = cJSON_PrintUnformatted(attr);
            if (attr_str) {
                esp_mqtt_client_publish(s_mqtt_client, "v1/devices/me/attributes", attr_str, 0, 1, 0);
                free(attr_str);
            }
            cJSON_Delete(attr);
        }

        // Đăng ký RPC điều khiển từ Dashboard ThingsBoard
        esp_mqtt_client_subscribe(s_mqtt_client, "v1/devices/me/rpc/request/+", 1);

        // Đẩy bản tin Telemetry đầu tiên
        iot_cloud_publish_telemetry_now();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "ThingsBoard MQTT: Disconnected!");
        s_is_connected = false;
        break;

    case MQTT_EVENT_DATA:
        if (event->topic && event->data) {
            char *topic_str = strndup(event->topic, event->topic_len);
            if (topic_str) {
                if (strstr(topic_str, "rpc/request")) {
                    handle_rpc_request(topic_str, event->data, event->data_len);
                }
                free(topic_str);
            }
        }
        break;

    default:
        break;
    }
}

static void on_wifi_ip_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data) {
    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi acquired IP -> Starting ThingsBoard MQTT client...");
        if (s_mqtt_client) {
            esp_mqtt_client_start(s_mqtt_client);
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected -> Pausing ThingsBoard MQTT client...");
        s_is_connected = false;
        if (s_mqtt_client) {
            esp_mqtt_client_stop(s_mqtt_client);
        }
    }
}

void iot_cloud_init(void) {
    ESP_LOGI(TAG, "Initializing ThingsBoard IoT Cloud Client (Event-Driven)...");

    // Đăng ký nhận sự kiện thay đổi trạng thái từ Gateway State Manager
    gateway_state_register_callback(on_gateway_state_changed);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = THINGSBOARD_BROKER_URI,
        .credentials.username = THINGSBOARD_ACCESS_TOKEN,
        .task.stack_size = 3072,
        .task.priority = 3,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    // Đăng ký nhận sự kiện IP để chỉ khởi động MQTT khi Wi-Fi đã sẵn sàng
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_ip_event, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_ip_event, NULL, NULL);

    if (sys_core_is_wifi_connected()) {
        ESP_LOGI(TAG, "Wi-Fi already connected -> Starting ThingsBoard MQTT immediately");
        esp_mqtt_client_start(s_mqtt_client);
    } else {
        ESP_LOGI(TAG, "Wi-Fi not connected yet. ThingsBoard MQTT will start automatically when IP is assigned.");
    }
}

bool iot_cloud_is_connected(void) {
    return s_is_connected;
}
