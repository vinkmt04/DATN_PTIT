/*
 * ACTUATOR CLIENT MODULE - thread_gateway
 *
 * Quan ly dieu khien Relay va Dimmer qua CoAP Confirmable (OpenThread).
 * - Relay:  PUT /relay ("ON"/"OFF") hoac GET /relay
 * - Dimmer: PUT /light ("0".."100") hoac GET /light
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#include "openthread/coap.h"
#include "openthread/ip6.h"
#include "openthread/message.h"
#include "openthread/thread.h"

#include "actuator_client.h"
#include "device_registry.h"

#define TAG                     "ACTUATOR_CLIENT"
#define RELAY_URI               "relay"
#define DIMMER_URI              "light"
#define ACTUATOR_PAYLOAD_MAX    64
#define ACTUATOR_CMD_QUEUE_LEN  8
#define ACTUATOR_TIMEOUT_WAIT_MS 12000

typedef struct {
    actuator_device_t device;
    bool              is_get;
    int               value;
} actuator_command_t;

typedef enum {
    ACTUATOR_EVT_RESPONSE,
    ACTUATOR_EVT_TIMEOUT,
    ACTUATOR_EVT_ERROR,
} actuator_event_type_t;

typedef struct {
    actuator_event_type_t type;
    otCoapCode            code;
    otError               error;
    int64_t               rx_time_ms;
    uint16_t              payload_len;
    char                  payload[ACTUATOR_PAYLOAD_MAX];
} actuator_event_t;

static QueueHandle_t       s_cmd_queue = NULL;
static QueueHandle_t       s_evt_queue = NULL;
static SemaphoreHandle_t   s_snapshot_mutex = NULL;
static actuator_callback_t s_callback = NULL;

static actuator_snapshot_t s_relay_snapshot = {
    .has_confirmed_value = false,
    .confirmed_value     = 0,
    .requested_value     = 0,
    .status              = ACTUATOR_UNKNOWN,
    .last_confirmed_ms   = 0,
};

static actuator_snapshot_t s_dimmer_snapshot = {
    .has_confirmed_value = false,
    .confirmed_value     = 0,
    .requested_value     = 0,
    .status              = ACTUATOR_UNKNOWN,
    .last_confirmed_ms   = 0,
};

void actuator_client_set_callback(actuator_callback_t cb)
{
    s_callback = cb;
}

static void actuator_publish_event(const actuator_event_t *event)
{
    if (s_evt_queue != NULL) {
        xQueueOverwrite(s_evt_queue, event);
    }
}

static void actuator_response_handler(void *context,
                                      otMessage *message,
                                      const otMessageInfo *message_info,
                                      otError result)
{
    (void)context;
    (void)message_info;

    actuator_event_t event;
    memset(&event, 0, sizeof(event));
    event.rx_time_ms = esp_timer_get_time() / 1000;
    event.error = result;

    if (result != OT_ERROR_NONE) {
        event.type = (result == OT_ERROR_RESPONSE_TIMEOUT)
                         ? ACTUATOR_EVT_TIMEOUT
                         : ACTUATOR_EVT_ERROR;
        actuator_publish_event(&event);
        return;
    }

    if (message == NULL) {
        event.type = ACTUATOR_EVT_ERROR;
        event.error = OT_ERROR_FAILED;
        actuator_publish_event(&event);
        return;
    }

    event.type = ACTUATOR_EVT_RESPONSE;
    event.code = otCoapMessageGetCode(message);

    uint16_t offset = otMessageGetOffset(message);
    uint16_t total  = otMessageGetLength(message);
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

    actuator_publish_event(&event);
}

static otError actuator_send_request_locked(otInstance *instance, const actuator_command_t *cmd)
{
    const char   *uri       = (cmd->device == ACTUATOR_RELAY) ? RELAY_URI : DIMMER_URI;
    otCoapCode    coap_code = cmd->is_get ? OT_COAP_CODE_GET : OT_COAP_CODE_PUT;
    otMessageInfo message_info = {0};

    if (cmd->device == ACTUATOR_RELAY) {
        if (device_registry_find_relay_locked(instance, &message_info.mPeerAddr, &message_info.mPeerPort) != ESP_OK) {
            ESP_LOGW(TAG, "RELAY_NOT_DISCOVERED: relay not in SRP Server or Cache");
            return OT_ERROR_NOT_FOUND;
        }
    } else if (cmd->device == ACTUATOR_DIMMER) {
        if (device_registry_find_dimmer_locked(instance, &message_info.mPeerAddr, &message_info.mPeerPort) != ESP_OK) {
            ESP_LOGW(TAG, "DIMMER_NOT_DISCOVERED: dimmer not in SRP Server or Cache");
            return OT_ERROR_NOT_FOUND;
        }
    } else {
        return OT_ERROR_INVALID_ARGS;
    }

    otMessage *msg = otCoapNewMessage(instance, NULL);
    if (msg == NULL) {
        return OT_ERROR_NO_BUFS;
    }

    otCoapMessageInit(msg, OT_COAP_TYPE_CONFIRMABLE, coap_code);
    otCoapMessageGenerateToken(msg, OT_COAP_DEFAULT_TOKEN_LENGTH);
    otCoapMessageAppendUriPathOptions(msg, uri);

    if (!cmd->is_get) {
        char payload[16];
        if (cmd->device == ACTUATOR_RELAY) {
            snprintf(payload, sizeof(payload), "%s", (cmd->value != 0) ? "ON" : "OFF");
        } else {
            snprintf(payload, sizeof(payload), "%d", cmd->value);
        }
        otCoapMessageSetPayloadMarker(msg);
        otMessageAppend(msg, payload, (uint16_t)strlen(payload));
    }

    otError err = otCoapSendRequest(instance, msg, &message_info, actuator_response_handler, NULL);
    if (err != OT_ERROR_NONE) {
        otMessageFree(msg);
        ESP_LOGE(TAG, "Failed to send CoAP request: %d", (int)err);
    }
    return err;
}

static void actuator_verify_and_update(const actuator_command_t *cmd, const actuator_event_t *evt)
{
    if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    actuator_snapshot_t *snap = (cmd->device == ACTUATOR_RELAY) ? &s_relay_snapshot : &s_dimmer_snapshot;
    int confirmed_val = snap->confirmed_value;

    if (evt->type == ACTUATOR_EVT_TIMEOUT) {
        snap->status = ACTUATOR_TIMEOUT;
        ESP_LOGW(TAG, "Actuator %s TIMEOUT after 12s", (cmd->device == ACTUATOR_RELAY) ? "relay" : "dimmer");
    } else if (evt->type == ACTUATOR_EVT_ERROR) {
        snap->status = ACTUATOR_ERROR;
        ESP_LOGE(TAG, "Actuator %s ERROR", (cmd->device == ACTUATOR_RELAY) ? "relay" : "dimmer");
    } else {
        bool match = false;
        if (cmd->device == ACTUATOR_RELAY) {
            if (evt->code == OT_COAP_CODE_CHANGED || evt->code == OT_COAP_CODE_CONTENT) {
                if (evt->payload_len > 0) {
                    if (strcasecmp(evt->payload, "ON") == 0 || strcmp(evt->payload, "1") == 0) {
                        confirmed_val = 1; match = true;
                    } else if (strcasecmp(evt->payload, "OFF") == 0 || strcmp(evt->payload, "0") == 0) {
                        confirmed_val = 0; match = true;
                    }
                } else if (!cmd->is_get) {
                    confirmed_val = cmd->value ? 1 : 0; match = true;
                }
            }
        } else {
            if (evt->code == OT_COAP_CODE_CHANGED || evt->code == OT_COAP_CODE_CONTENT) {
                if (evt->payload_len > 0) {
                    int val = atoi(evt->payload);
                    if (val >= 0 && val <= 100) {
                        confirmed_val = val; match = true;
                    }
                } else if (!cmd->is_get) {
                    confirmed_val = cmd->value; match = true;
                }
            }
        }

        if (match) {
            snap->status = ACTUATOR_CONFIRMED;
            snap->has_confirmed_value = true;
            snap->confirmed_value = confirmed_val;
            snap->last_confirmed_ms = evt->rx_time_ms;
            ESP_LOGI(TAG, "Actuator %s CONFIRMED: value=%d",
                     (cmd->device == ACTUATOR_RELAY) ? "relay" : "dimmer", confirmed_val);
        } else {
            snap->status = ACTUATOR_ERROR;
        }
    }

    actuator_status_t final_status = snap->status;
    xSemaphoreGive(s_snapshot_mutex);

    if (s_callback != NULL) {
        s_callback(cmd->device, final_status, confirmed_val);
    }
}

static void actuator_task(void *pvParameters)
{
    (void)pvParameters;

    otInstance *instance = NULL;
    while ((instance = esp_openthread_get_instance()) == NULL) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    while (1) {
        actuator_command_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
            actuator_snapshot_t *snap = (cmd.device == ACTUATOR_RELAY) ? &s_relay_snapshot : &s_dimmer_snapshot;
            snap->status = ACTUATOR_PENDING;
            if (!cmd.is_get) {
                snap->requested_value = cmd.value;
            }
            xSemaphoreGive(s_snapshot_mutex);
        }

        bool sent = false;
        if (esp_openthread_lock_acquire(portMAX_DELAY)) {
            otDeviceRole role = otThreadGetDeviceRole(instance);
            bool in_network = (role == OT_DEVICE_ROLE_CHILD ||
                               role == OT_DEVICE_ROLE_ROUTER ||
                               role == OT_DEVICE_ROLE_LEADER);
            if (in_network) {
                if (actuator_send_request_locked(instance, &cmd) == OT_ERROR_NONE) {
                    sent = true;
                }
            } else {
                ESP_LOGW(TAG, "Host not attached to Thread network (role=%d)", role);
            }
            esp_openthread_lock_release();
        }

        if (!sent) {
            if (xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY) == pdTRUE) {
                actuator_snapshot_t *snap = (cmd.device == ACTUATOR_RELAY) ? &s_relay_snapshot : &s_dimmer_snapshot;
                snap->status = ACTUATOR_ERROR;
                xSemaphoreGive(s_snapshot_mutex);
            }
            if (s_callback != NULL) {
                s_callback(cmd.device, ACTUATOR_ERROR, cmd.value);
            }
            continue;
        }

        actuator_event_t evt;
        if (xQueueReceive(s_evt_queue, &evt, pdMS_TO_TICKS(ACTUATOR_TIMEOUT_WAIT_MS)) == pdTRUE) {
            actuator_verify_and_update(&cmd, &evt);
        } else {
            actuator_event_t timeout_evt = {
                .type  = ACTUATOR_EVT_TIMEOUT,
                .error = OT_ERROR_RESPONSE_TIMEOUT,
            };
            actuator_verify_and_update(&cmd, &timeout_evt);
        }
    }
}

esp_err_t actuator_client_start(void)
{
    if (s_cmd_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_snapshot_mutex = xSemaphoreCreateMutex();
    if (s_snapshot_mutex == NULL) return ESP_ERR_NO_MEM;

    s_cmd_queue = xQueueCreate(ACTUATOR_CMD_QUEUE_LEN, sizeof(actuator_command_t));
    if (s_cmd_queue == NULL) {
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_evt_queue = xQueueCreate(1, sizeof(actuator_event_t));
    if (s_evt_queue == NULL) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        vSemaphoreDelete(s_snapshot_mutex);
        s_snapshot_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(actuator_task, "actuator_task", 4096, NULL, 5, NULL);
    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t actuator_set_relay(bool on)
{
    if (s_cmd_queue == NULL) return ESP_ERR_INVALID_STATE;
    actuator_command_t cmd = {
        .device = ACTUATOR_RELAY,
        .is_get = false,
        .value  = on ? 1 : 0,
    };
    return (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t actuator_set_brightness(int percent)
{
    if (s_cmd_queue == NULL) return ESP_ERR_INVALID_STATE;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    actuator_command_t cmd = {
        .device = ACTUATOR_DIMMER,
        .is_get = false,
        .value  = percent,
    };
    return (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t actuator_read_relay(void)
{
    if (s_cmd_queue == NULL) return ESP_ERR_INVALID_STATE;
    actuator_command_t cmd = {
        .device = ACTUATOR_RELAY,
        .is_get = true,
        .value  = 0,
    };
    return (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t actuator_read_brightness(void)
{
    if (s_cmd_queue == NULL) return ESP_ERR_INVALID_STATE;
    actuator_command_t cmd = {
        .device = ACTUATOR_DIMMER,
        .is_get = true,
        .value  = 0,
    };
    return (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t actuator_get_snapshot(actuator_device_t device, actuator_snapshot_t *out)
{
    if (out == NULL || s_snapshot_mutex == NULL) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_snapshot_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    *out = (device == ACTUATOR_RELAY) ? s_relay_snapshot : s_dimmer_snapshot;
    xSemaphoreGive(s_snapshot_mutex);
    return ESP_OK;
}

esp_err_t actuator_send_unpair(actuator_device_t device)
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
    esp_err_t find_err = ESP_ERR_NOT_FOUND;
    if (device == ACTUATOR_RELAY) {
        find_err = device_registry_find_relay_locked(instance, &message_info.mPeerAddr, &message_info.mPeerPort);
    } else if (device == ACTUATOR_DIMMER) {
        find_err = device_registry_find_dimmer_locked(instance, &message_info.mPeerAddr, &message_info.mPeerPort);
    }

    if (find_err != ESP_OK) {
        esp_openthread_lock_release();
        return find_err;
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
    ESP_LOGI(TAG, "Sent CoAP POST /unpair to actuator device %d (err=%d)", device, (int)err);
    return (err == OT_ERROR_NONE) ? ESP_OK : ESP_FAIL;
}
