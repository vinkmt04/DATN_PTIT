/*
 * THREAD GATEWAY - OpenThread Border Router Host Stack (ESP32-S3)
 *
 * Quan ly mang Thread Mesh qua Spinel UART (TX:13, RX:14 @ 460800bps) toi ESP32-H2 RCP.
 * Cung cap cac dich vu:
 *   - OpenThread Border Router Host stack
 *   - Tu dong khoi phuc hoac sinh mang Thread bao mat (Unique Network Key)
 *   - SRP Server & Dynamic Service Registry (relay-01, dimmer-test-01, sensor-01)
 *   - CoAP Server & CoAP Clients (Actuator & Sensor)
 *   - OpenThread Commissioner ho tro quy trinh phe duyet (Approval Flow)
 */

#include "thread_gateway.h"
#include "esp_ot_config.h"
#include "device_registry.h"
#include "thread_commissioner.h"
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_vfs_eventfd.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_spinel.h"

#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/srp_server.h"
#include "openthread/border_agent.h"
#include "openthread/coap.h"
#include "openthread/logging.h"

#define TAG "THREAD_GW"

static bool s_initialized = false;
static bool s_rcp_connected = false;
static SemaphoreHandle_t s_ot_init_sem = NULL;

bool thread_gateway_is_rcp_connected(void)
{
    return s_rcp_connected;
}

static void rcp_reset_failure_cb(void)
{
    ESP_LOGE(TAG, "======================================================================");
    ESP_LOGE(TAG, "[THREAD_GW] OpenThread RCP (ESP32-H2) NOT RESPONDING on UART1!");
    ESP_LOGE(TAG, "[THREAD_GW] Baud Rate: 460800 | S3 TX: GPIO 18 -> H2 RX: GPIO 4");
    ESP_LOGE(TAG, "[THREAD_GW]                     S3 RX: GPIO 14 <- H2 TX: GPIO 5");
    ESP_LOGE(TAG, "[THREAD_GW]                     GND: Common Ground");
    ESP_LOGE(TAG, "[THREAD_GW] Gateway switching to STANDALONE MODE (Wi-Fi, UI, Audio, AI Active).");
    ESP_LOGE(TAG, "======================================================================");

    s_rcp_connected = false;

    if (s_ot_init_sem) {
        xSemaphoreGive(s_ot_init_sem);
    }

    // Kết thúc task ot_task_worker an toàn để OpenThread KHÔNG gọi DieNow(OT_EXIT_FAILURE) / abort()!
    vTaskDelete(NULL);
}

static otCoapResource s_report_resource;
static thread_report_callback_t s_report_cb = NULL;

void thread_gateway_set_report_callback(thread_report_callback_t cb)
{
    s_report_cb = cb;
}

static void report_request_handler(void *aContext, otMessage *aMessage,
                                   const otMessageInfo *aMessageInfo)
{
    (void)aContext;
    char payload[64] = {0};
    uint16_t offset = otMessageGetOffset(aMessage);
    uint16_t length = otMessageGetLength(aMessage) - offset;
    if (length > 0 && length < sizeof(payload)) {
        otMessageRead(aMessage, offset, payload, length);
        payload[length] = '\0';
    }
    ESP_LOGI(TAG, "Gateway CoAP received /report: '%s'", payload);

    thread_dev_type_t dev_type = THREAD_DEV_UNKNOWN;
    if (strncmp(payload, "relay:", 6) == 0) {
        dev_type = THREAD_DEV_RELAY;
    } else if (strncmp(payload, "dimmer:", 7) == 0) {
        dev_type = THREAD_DEV_DIMMER;
    } else if (strncmp(payload, "sensor", 6) == 0 || strncmp(payload, "temp:", 5) == 0) {
        dev_type = THREAD_DEV_SENSOR;
    }

    // Kiểm tra tính hợp lệ của thiết bị gửi report
    if (dev_type != THREAD_DEV_UNKNOWN) {
        if (!device_registry_is_type_registered(dev_type)) {
            bool in_pairing_mode = thread_commissioner_is_scanning() || thread_commissioner_is_pairing_active();
            if (in_pairing_mode) {
                // Đang trong cửa sổ quét tìm hoặc đang duyệt ghép nối -> Đây là node ứng viên mới vừa join Thread!
                // TUYỆT ĐỐI KHÔNG GỬI /unpair! Cập nhật IP peer và chuyển tiếp report để Commissioner / UI nhận diện!
                ESP_LOGI(TAG, "Gateway received /report from candidate node (type=%d) during pairing scan. Updating cache & forwarding.", dev_type);
                if (aMessageInfo != NULL) {
                    device_registry_update_ip(dev_type, &aMessageInfo->mPeerAddr);
                }
                if (s_report_cb != NULL) {
                    s_report_cb(payload);
                }
                return;
            } else {
                // Thiết bị này KHÔNG được đăng ký trong Gateway VÀ Gateway KHÔNG ở chế độ ghép nối
                // (Node mồ côi hoặc vừa bị xóa trên Gateway trong lúc node offline)!
                ESP_LOGW(TAG, "Gateway received /report from UNREGISTERED node (type=%d) while not in pairing mode! Ejecting via CoAP POST /unpair...", dev_type);
                if (aMessageInfo != NULL) {
                    otInstance *instance = esp_openthread_get_instance();
                    if (instance != NULL) {
                        otMessage *unpair_msg = otCoapNewMessage(instance, NULL);
                        if (unpair_msg != NULL) {
                            otCoapMessageInit(unpair_msg, OT_COAP_TYPE_CONFIRMABLE, OT_COAP_CODE_POST);
                            otCoapMessageAppendUriPathOptions(unpair_msg, "unpair");
                            otError err = otCoapSendRequest(instance, unpair_msg, aMessageInfo, NULL, NULL);
                            if (err != OT_ERROR_NONE) {
                                otMessageFree(unpair_msg);
                            } else {
                                ESP_LOGW(TAG, "Ejection CoAP /unpair sent successfully to unregistered node");
                            }
                        }
                    }
                }
                return; // Chặn hoàn toàn: Không cập nhật IP cache, không chuyển tiếp cho controller!
            }
        }
    }

    if (aMessageInfo != NULL && dev_type != THREAD_DEV_UNKNOWN) {
        device_registry_update_ip(dev_type, &aMessageInfo->mPeerAddr);
    }

    if (s_report_cb != NULL) {
        s_report_cb(payload);
    }
}

static esp_err_t thread_coap_server_start(void)
{
    if (!esp_openthread_lock_acquire(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }
    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }
    otError err = otCoapStart(instance, 5683);
    if (err == OT_ERROR_NONE || err == OT_ERROR_ALREADY) {
        memset(&s_report_resource, 0, sizeof(s_report_resource));
        s_report_resource.mUriPath = "report";
        s_report_resource.mHandler = report_request_handler;
        otCoapAddResource(instance, &s_report_resource);
    }
    esp_openthread_lock_release();
    if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
        ESP_LOGE(TAG, "otCoapStart failed: %d", (int)err);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Gateway CoAP server ready on port 5683 (URI: /report)");
    return ESP_OK;
}

static void gateway_ot_state_changed_cb(otChangedFlags flags, void *context)
{
    otInstance *instance = (otInstance *)context;
    if (flags & OT_CHANGED_THREAD_ROLE) {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        ESP_LOGI(TAG, "OpenThread Gateway role changed to: %d (%s)", (int)role, otThreadDeviceRoleToString(role));
        if (role == OT_DEVICE_ROLE_CHILD) {
            ESP_LOGI(TAG, "Gateway attached as Child -> upgrading to Router & Leader...");
            otThreadBecomeRouter(instance);
            otThreadBecomeLeader(instance);
        } else if (role == OT_DEVICE_ROLE_ROUTER) {
            ESP_LOGI(TAG, "Gateway is Router -> claiming Leader role (weight 255)...");
            otThreadBecomeLeader(instance);
            thread_commissioner_on_role_changed(role);
        } else if (role == OT_DEVICE_ROLE_LEADER) {
            thread_commissioner_on_role_changed(role);
        }
    }
}

static void ot_task_worker(void *aContext)
{
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config  = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *openthread_netif = esp_netif_new(&cfg);
    assert(openthread_netif != NULL);

    // Đăng ký callback an toàn: Bắt lỗi khi co-processor H2 không phản hồi
    esp_openthread_set_coprocessor_reset_failure_callback(rcp_reset_failure_cb);

    esp_err_t ot_err = esp_openthread_init(&config);
    if (ot_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_openthread_init failed: %s", esp_err_to_name(ot_err));
        s_rcp_connected = false;
        if (s_ot_init_sem) {
            xSemaphoreGive(s_ot_init_sem);
        }
        esp_netif_destroy(openthread_netif);
        vTaskDelete(NULL);
        return;
    }
    s_rcp_connected = true;

    ESP_ERROR_CHECK(esp_netif_attach(openthread_netif, esp_openthread_netif_glue_init(&config)));

    esp_openthread_lock_acquire(portMAX_DELAY);
    otInstance *instance = esp_openthread_get_instance();
    if (instance != NULL) {
        otLoggingSetLevel(OT_LOG_LEVEL_WARN);
        ESP_LOGI(TAG, "OpenThread logging level set to WARN (Production clean log)");

        otSrpServerSetEnabled(instance, true);
        otSrpServerLeaseConfig lease_cfg = {
            .mMinLease    = 30,
            .mMaxLease    = 7200,
            .mMinKeyLease = 30,
            .mMaxKeyLease = 120, // 2 phut max key lease phong ngua Name Conflict Error 6
        };
        otSrpServerSetLeaseConfig(instance, &lease_cfg);
        ESP_LOGI(TAG, "OpenThread SRP Server enabled on port %u (Key-Lease: 30-120s)", (unsigned)otSrpServerGetPort(instance));

        otBorderAgentSetEnabled(instance, true);
        ESP_LOGI(TAG, "OpenThread Border Agent enabled (UDP port %u)", (unsigned)otBorderAgentGetUdpPort(instance));

        // Thiết lập trọng số Leader tuyệt đối (255 = mức cao nhất của OpenThread) để Gateway luôn luôn là Leader
        otThreadSetLocalLeaderWeight(instance, 255);
        otSetStateChangedCallback(instance, gateway_ot_state_changed_cb, instance);

        if (otDatasetIsCommissioned(instance)) {
            ESP_LOGI(TAG, "Restoring existing Thread network dataset from NVS...");
            otError th_err = otIp6SetEnabled(instance, true);
            if (th_err == OT_ERROR_NONE) {
                th_err = otThreadSetEnabled(instance, true);
            }
            if (th_err == OT_ERROR_NONE) {
                otError lead_err = otThreadBecomeLeader(instance);
                ESP_LOGI(TAG, "Thread network restored and running as Leader! (lead_res=%d)", (int)lead_err);
            } else {
                ESP_LOGE(TAG, "Failed to restore Thread network: %d", (int)th_err);
            }
        } else {
            ESP_LOGI(TAG, "No dataset in NVS -> Generating Thread dataset (Channel 19, PAN 0xa05e)...");
            otOperationalDataset dataset;
            if (otDatasetCreateNewNetwork(instance, &dataset) == OT_ERROR_NONE) {
                static const uint8_t s_fixed_network_key[OT_NETWORK_KEY_SIZE] = {
                    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
                };
                static const otExtendedPanId s_fixed_ext_panid = {
                    .m8 = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}
                };
                static const otMeshLocalPrefix s_fixed_mesh_local_prefix = {
                    .m8 = {0xfd, 0x00, 0x0d, 0xad, 0x00, 0x00, 0x00, 0x00}
                };

                dataset.mChannel = 19;
                dataset.mPanId = 0xa05e;
                dataset.mComponents.mIsChannelPresent = true;
                dataset.mComponents.mIsPanIdPresent = true;

                memcpy(dataset.mNetworkKey.m8, s_fixed_network_key, sizeof(s_fixed_network_key));
                dataset.mComponents.mIsNetworkKeyPresent = true;

                memcpy(dataset.mExtendedPanId.m8, s_fixed_ext_panid.m8, sizeof(s_fixed_ext_panid.m8));
                dataset.mComponents.mIsExtendedPanIdPresent = true;

                memcpy(dataset.mMeshLocalPrefix.m8, s_fixed_mesh_local_prefix.m8, sizeof(s_fixed_mesh_local_prefix.m8));
                dataset.mComponents.mIsMeshLocalPrefixPresent = true;

                const char *net_name = "OpenThread-PTIT";
                size_t name_len = strlen(net_name);
                memcpy(dataset.mNetworkName.m8, net_name, name_len);
                dataset.mNetworkName.m8[name_len] = '\0';
                dataset.mComponents.mIsNetworkNamePresent = true;

                otDatasetSetActive(instance, &dataset);
                otIp6SetEnabled(instance, true);
                otThreadSetEnabled(instance, true);
                otError lead_err = otThreadBecomeLeader(instance);
                ESP_LOGI(TAG, "New Thread network '%s' created and active as Leader! (lead_res=%d)", net_name, (int)lead_err);
            } else {
                ESP_LOGE(TAG, "Failed to create random Thread network dataset");
            }
        }
    }
    esp_openthread_lock_release();

    if (s_ot_init_sem) {
        xSemaphoreGive(s_ot_init_sem);
    }

    // Run the OpenThread main loop
    esp_openthread_launch_mainloop();

    // Clean up
    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(openthread_netif);
    vTaskDelete(NULL);
}

esp_err_t thread_gateway_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Thread Gateway (Border Router on S3 via H2 RCP UART1 TX:18 RX:14 @ 460800)...");

    esp_vfs_eventfd_config_t eventfd_config = {
        .max_fds = 4,
    };
    esp_err_t err = esp_vfs_eventfd_register(&eventfd_config);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "eventfd register returned: %d", err);
    }

    s_ot_init_sem = xSemaphoreCreateBinary();
    if (!s_ot_init_sem) {
        ESP_LOGE(TAG, "Failed to create initialization semaphore");
        return ESP_ERR_NO_MEM;
    }

    // ot_br_main BẮT BUỘC có stack trong Internal DRAM vì OpenThread ghi NVS dataset khi cache PSRAM bị freeze
    BaseType_t ret = xTaskCreate(ot_task_worker, "ot_br_main", 8192, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ot_br_main task");
        vSemaphoreDelete(s_ot_init_sem);
        s_ot_init_sem = NULL;
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_ot_init_sem, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGW(TAG, "Timeout waiting for OpenThread initialization");
        s_rcp_connected = false;
    }
    vSemaphoreDelete(s_ot_init_sem);
    s_ot_init_sem = NULL;

    if (!s_rcp_connected) {
        ESP_LOGW(TAG, ">>> [THREAD_GW] RCP Not Connected. Gateway continuing in STANDALONE MODE (Wi-Fi, UI, Audio, AI Active). <<<");
        return ESP_OK;
    }

    ESP_ERROR_CHECK(thread_coap_server_start());
    ESP_ERROR_CHECK(sensor_client_start());
    ESP_ERROR_CHECK(actuator_client_start());
    ESP_ERROR_CHECK(thread_commissioner_init());

    s_initialized = true;
    ESP_LOGI(TAG, "Thread Gateway initialized successfully with RCP connected!");
    return ESP_OK;
}

esp_err_t thread_gateway_set_relay(bool on)
{
    if (!s_rcp_connected) {
        ESP_LOGW(TAG, "Cannot control relay: Thread RCP not connected");
        return ESP_ERR_INVALID_STATE;
    }
    return actuator_set_relay(on);
}

esp_err_t thread_gateway_set_dimmer(uint8_t brightness)
{
    if (!s_rcp_connected) {
        ESP_LOGW(TAG, "Cannot control dimmer: Thread RCP not connected");
        return ESP_ERR_INVALID_STATE;
    }
    return actuator_set_brightness((int)brightness);
}

uint8_t thread_gateway_get_node_count(void)
{
    if (!s_rcp_connected) {
        return 0;
    }
    uint8_t count = 0;
    if (esp_openthread_lock_acquire(portMAX_DELAY)) {
        otInstance *instance = esp_openthread_get_instance();
        if (instance != NULL) {
            count = device_registry_count_active_locked(instance);
        }
        esp_openthread_lock_release();
    }
    return count;
}

bool thread_gateway_is_relay_online(void)
{
    if (!s_rcp_connected) {
        return false;
    }
    bool online = false;
    if (esp_openthread_lock_acquire(portMAX_DELAY)) {
        otInstance *instance = esp_openthread_get_instance();
        online = device_registry_is_node_online(instance, "relay");
        esp_openthread_lock_release();
    }
    return online;
}

bool thread_gateway_is_dimmer_online(void)
{
    if (!s_rcp_connected) {
        return false;
    }
    bool online = false;
    if (esp_openthread_lock_acquire(portMAX_DELAY)) {
        otInstance *instance = esp_openthread_get_instance();
        online = device_registry_is_node_online(instance, "dimmer");
        esp_openthread_lock_release();
    }
    return online;
}

bool thread_gateway_is_sensor_online(void)
{
    if (!s_rcp_connected) {
        return false;
    }
    bool online = false;
    if (esp_openthread_lock_acquire(portMAX_DELAY)) {
        otInstance *instance = esp_openthread_get_instance();
        online = device_registry_is_node_online(instance, "sensor");
        esp_openthread_lock_release();
    }
    return online;
}

void thread_gateway_set_sensor_callback(sensor_callback_t cb)
{
    sensor_client_set_callback(cb);
}

void thread_gateway_set_actuator_callback(actuator_callback_t cb)
{
    actuator_client_set_callback(cb);
}

esp_err_t thread_gateway_remove_device(const char *dev_id_str)
{
    if (dev_id_str == NULL) return ESP_ERR_INVALID_ARG;
    ESP_LOGW(TAG, ">>> Removing device [%s] from Thread Gateway <<<", dev_id_str);

    // 1. Gui lenh CoAP POST /unpair de yeu cau Node tu xoa dataset va restart
    if (strstr(dev_id_str, "relay") != NULL) {
        actuator_send_unpair(ACTUATOR_RELAY);
    } else if (strstr(dev_id_str, "dimmer") != NULL) {
        actuator_send_unpair(ACTUATOR_DIMMER);
    } else if (strstr(dev_id_str, "sensor") != NULL) {
        sensor_send_unpair();
    }

    // 2. Xoa khoi NVS IP Cache
    device_registry_clear_cache(dev_id_str);

    // 3. Danh dau offline ngay lap tuc trong registry
    device_registry_mark_offline(dev_id_str);

    // 4. Purge SRP Server bang cach reset SRP Server
    thread_gateway_purge_srp_server();

    // 5. Xoa khoi Commissioner approved registry
    return thread_commissioner_remove_device(dev_id_str);
}

void thread_gateway_purge_srp_server(void)
{
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        return;
    }
    otInstance *instance = esp_openthread_get_instance();
    if (instance != NULL) {
        ESP_LOGI(TAG, "Purging SRP Server entries to clear stale keys...");
        otSrpServerSetEnabled(instance, false);
        otSrpServerSetEnabled(instance, true);
        otSrpServerLeaseConfig lease_cfg = {
            .mMinLease    = 30,
            .mMaxLease    = 7200,
            .mMinKeyLease = 30,
            .mMaxKeyLease = 120,
        };
        otSrpServerSetLeaseConfig(instance, &lease_cfg);
    }
    esp_openthread_lock_release();
}
