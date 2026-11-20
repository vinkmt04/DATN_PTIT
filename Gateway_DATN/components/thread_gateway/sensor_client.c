/*
 * SENSOR CLIENT MODULE - thread_gateway
 *
 * Dinh ky gui CoAP GET den Sensor Node qua SRP ("sensor-01"):
 *   _sensor._udp /sensor
 * Nhan, phan tich JSON va cap nhat du lieu nhiet do / do am cho gateway_state_manager.
 */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#include "openthread/coap.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/thread.h"

#include "cJSON.h"
#include "sensor_client.h"
#include "device_registry.h"

#define TAG                 "SENSOR_CLIENT"
#define SENSOR_URI          "sensor"
#define SENSOR_POLL_MS      10000
#define SENSOR_PAYLOAD_MAX  512
#define SENSOR_NODE_ID      "sensor_01"

typedef enum {
    SENSOR_EVENT_RESPONSE,
    SENSOR_EVENT_TIMEOUT,
    SENSOR_EVENT_ERROR,
} sensor_event_type_t;

typedef struct {
    sensor_event_type_t type;
    otCoapCode          code;
    otError             error;
    int64_t             rx_time_ms;
    uint16_t            payload_len;
    char                payload[SENSOR_PAYLOAD_MAX];
} sensor_event_t;

static QueueHandle_t     s_sensor_queue = NULL;
static QueueHandle_t     s_snapshot_queue = NULL;
static sensor_callback_t s_sensor_callback = NULL;

static sensor_client_data_t s_sensor_data = {
    .has_valid_sample = false,
    .temperature_c    = 0.0f,
    .humidity_pct     = 0.0f,
    .sample_seq       = 0,
    .age_ms           = 0,
    .last_rx_time_ms  = 0,
    .last_read_ok     = false,
    .stale            = false,
    .state            = SENSOR_STATE_NO_DATA,
};

void sensor_client_set_callback(sensor_callback_t cb)
{
    s_sensor_callback = cb;
}

static void sensor_publish_event(const sensor_event_t *event)
{
    if (s_sensor_queue != NULL) {
        xQueueOverwrite(s_sensor_queue, event);
    }
}

static void sensor_response_handler(void *context,
                                    otMessage *message,
                                    const otMessageInfo *message_info,
                                    otError result)
{
    (void)context;
    (void)message_info;

    sensor_event_t event;
    memset(&event, 0, sizeof(event));
    event.rx_time_ms = esp_timer_get_time() / 1000;
    event.error = result;

    if (result != OT_ERROR_NONE) {
        event.type = (result == OT_ERROR_RESPONSE_TIMEOUT) ? SENSOR_EVENT_TIMEOUT : SENSOR_EVENT_ERROR;
        sensor_publish_event(&event);
        return;
    }

    if (message == NULL) {
        event.type = SENSOR_EVENT_ERROR;
        event.error = OT_ERROR_FAILED;
        sensor_publish_event(&event);
        return;
    }

    event.type = SENSOR_EVENT_RESPONSE;
    event.code = otCoapMessageGetCode(message);

    uint16_t offset = otMessageGetOffset(message);
    uint16_t total = otMessageGetLength(message);
    if (offset <= total) {
        uint16_t length = total - offset;
        if (length < sizeof(event.payload)) {
            uint16_t bytes_read = 0;
            if (length > 0) {
                bytes_read = otMessageRead(message, offset, event.payload, length);
            }
            if (bytes_read == length) {
                event.payload[length] = '\0';
                event.payload_len = length;
            }
        }
    }

    sensor_publish_event(&event);
}

static otError sensor_send_get_locked(otInstance *instance)
{
    otMessageInfo message_info = {0};
    if (device_registry_find_sensor_locked(instance, &message_info.mPeerAddr, &message_info.mPeerPort) != ESP_OK) {
        return OT_ERROR_NOT_FOUND;
    }

    otMessage *message = otCoapNewMessage(instance, NULL);
    if (message == NULL) {
        return OT_ERROR_NO_BUFS;
    }

    otCoapMessageInit(message, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_GET);
    otCoapMessageGenerateToken(message, OT_COAP_DEFAULT_TOKEN_LENGTH);

    otError err = otCoapMessageAppendUriPathOptions(message, SENSOR_URI);
    if (err != OT_ERROR_NONE) {
        otMessageFree(message);
        return err;
    }

    err = otCoapSendRequest(instance, message, &message_info, sensor_response_handler, NULL);
    if (err != OT_ERROR_NONE) {
        otMessageFree(message);
    }
    return err;
}

static void sensor_process_response(const sensor_event_t *event)
{
    if (event->type == SENSOR_EVENT_TIMEOUT) {
        s_sensor_data.state = SENSOR_STATE_TIMEOUT;
        ESP_LOGW(TAG, "SENSOR_TIMEOUT: %s", SENSOR_NODE_ID);
        if (s_sensor_callback != NULL) {
            s_sensor_callback(&s_sensor_data);
        }
        return;
    }

    if (event->type == SENSOR_EVENT_ERROR) {
        s_sensor_data.state = SENSOR_STATE_RESP_ERROR;
        ESP_LOGE(TAG, "SENSOR_ERROR: %s (code=%d)", SENSOR_NODE_ID, event->error);
        if (s_sensor_callback != NULL) {
            s_sensor_callback(&s_sensor_data);
        }
        return;
    }

    if (event->code != OT_COAP_CODE_CONTENT) {
        s_sensor_data.state = SENSOR_STATE_RESP_ERROR;
        return;
    }

    cJSON *root = cJSON_Parse(event->payload);
    if (root == NULL) {
        s_sensor_data.state = SENSOR_STATE_RESP_ERROR;
        return;
    }

    cJSON *j_temp = cJSON_GetObjectItem(root, "temperature_c");
    cJSON *j_hum  = cJSON_GetObjectItem(root, "humidity_pct");
    cJSON *j_seq  = cJSON_GetObjectItem(root, "sample_seq");
    cJSON *j_ok   = cJSON_GetObjectItem(root, "last_read_ok");
    cJSON *j_stale = cJSON_GetObjectItem(root, "stale");

    if (j_temp && cJSON_IsNumber(j_temp) && j_hum && cJSON_IsNumber(j_hum)) {
        s_sensor_data.has_valid_sample = true;
        s_sensor_data.temperature_c = (float)j_temp->valuedouble;
        s_sensor_data.humidity_pct = (float)j_hum->valuedouble;
        s_sensor_data.sample_seq = (j_seq && cJSON_IsNumber(j_seq)) ? (uint32_t)j_seq->valuedouble : 0;
        s_sensor_data.last_rx_time_ms = event->rx_time_ms;
        s_sensor_data.last_read_ok = (j_ok && cJSON_IsBool(j_ok)) ? cJSON_IsTrue(j_ok) : true;
        s_sensor_data.stale = (j_stale && cJSON_IsBool(j_stale)) ? cJSON_IsTrue(j_stale) : false;
        s_sensor_data.state = s_sensor_data.stale ? SENSOR_STATE_STALE : SENSOR_STATE_GOOD;
        strncpy(s_sensor_data.node_id, SENSOR_NODE_ID, sizeof(s_sensor_data.node_id) - 1);

        ESP_LOGI(TAG, "SENSOR_RX: temp=%.1f C, hum=%.1f %%, seq=%" PRIu32,
                 s_sensor_data.temperature_c, s_sensor_data.humidity_pct, s_sensor_data.sample_seq);

        if (s_sensor_callback != NULL) {
            s_sensor_callback(&s_sensor_data);
        }
    }

    cJSON_Delete(root);
}

static void sensor_poll_task(void *pvParameters)
{
    (void)pvParameters;

    otInstance *instance = NULL;
    while ((instance = esp_openthread_get_instance()) == NULL) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    bool request_pending = false;
    TickType_t last_poll_tick = xTaskGetTickCount() - pdMS_TO_TICKS(SENSOR_POLL_MS);

    while (1) {
        TickType_t now_tick = xTaskGetTickCount();
        TickType_t elapsed_ticks = now_tick - last_poll_tick;
        TickType_t poll_period_ticks = pdMS_TO_TICKS(SENSOR_POLL_MS);

        if (elapsed_ticks >= poll_period_ticks) {
            last_poll_tick = now_tick;

            if (!request_pending) {
                if (esp_openthread_lock_acquire(portMAX_DELAY)) {
                    otDeviceRole role = otThreadGetDeviceRole(instance);
                    bool in_network = (role == OT_DEVICE_ROLE_CHILD ||
                                       role == OT_DEVICE_ROLE_ROUTER ||
                                       role == OT_DEVICE_ROLE_LEADER);

                    if (in_network) {
                        if (sensor_send_get_locked(instance) == OT_ERROR_NONE) {
                            request_pending = true;
                        }
                    }
                    esp_openthread_lock_release();
                }
            }
        }

        now_tick = xTaskGetTickCount();
        elapsed_ticks = now_tick - last_poll_tick;
        TickType_t wait_ticks = (elapsed_ticks < poll_period_ticks) ? (poll_period_ticks - elapsed_ticks) : 0;
        if (wait_ticks > pdMS_TO_TICKS(1000)) {
            wait_ticks = pdMS_TO_TICKS(1000);
        }

        sensor_event_t event;
        if (xQueueReceive(s_sensor_queue, &event, wait_ticks) == pdTRUE) {
            request_pending = false;
            sensor_process_response(&event);

            if (s_snapshot_queue != NULL) {
                xQueueOverwrite(s_snapshot_queue, &s_sensor_data);
            }
        }
    }
}

esp_err_t sensor_client_start(void)
{
    if (s_sensor_queue != NULL) return ESP_ERR_INVALID_STATE;

    s_sensor_queue = xQueueCreate(1, sizeof(sensor_event_t));
    if (s_sensor_queue == NULL) return ESP_ERR_NO_MEM;

    s_snapshot_queue = xQueueCreate(1, sizeof(sensor_client_data_t));
    if (s_snapshot_queue == NULL) {
        vQueueDelete(s_sensor_queue);
        s_sensor_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    xQueueOverwrite(s_snapshot_queue, &s_sensor_data);

    BaseType_t ret = xTaskCreate(sensor_poll_task, "sensor_poll", 4096, NULL, 4, NULL);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t sensor_client_get_snapshot(sensor_client_data_t *out)
{
    if (out == NULL || s_snapshot_queue == NULL) return ESP_ERR_INVALID_ARG;
    return (xQueuePeek(s_snapshot_queue, out, 0) == pdTRUE) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t sensor_send_unpair(void)
{
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        return ESP_ERR_TIMEOUT;
    }
    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }

    otMessageInfo message_info = {0};
    if (device_registry_find_sensor_locked(instance, &message_info.mPeerAddr, &message_info.mPeerPort) != ESP_OK) {
        esp_openthread_lock_release();
        return ESP_ERR_NOT_FOUND;
    }

    otMessage *msg = otCoapNewMessage(instance, NULL);
    if (msg == NULL) {
        esp_openthread_lock_release();
        return ESP_ERR_NO_MEM;
    }

    otCoapMessageInit(msg, OT_COAP_TYPE_NON_CONFIRMABLE, OT_COAP_CODE_POST);
    otCoapMessageAppendUriPathOptions(msg, "unpair");
    otError err = otCoapSendRequest(instance, msg, &message_info, NULL, NULL);
    if (err != OT_ERROR_NONE) {
        otMessageFree(msg);
    }
    esp_openthread_lock_release();
    ESP_LOGI(TAG, "Sent CoAP POST /unpair to Sensor node (err=%d)", (int)err);
    return (err == OT_ERROR_NONE) ? ESP_OK : ESP_FAIL;
}
