/*
 * THREAD COMMISSIONER MODULE - thread_gateway
 *
 * Quản lý cửa sổ ghép nối 60 giây (OpenThread Commissioner),
 * tiếp nhận Joiner từ các Node và thực hiện quy trình phê duyệt (Approval Flow).
 * Chỉ báo kết quả thành công sau khi thiết bị đã xác thực kết nối hoàn toàn qua SRP.
 */

#include "thread_commissioner.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "openthread/instance.h"
#include "openthread/commissioner.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/ip6.h"

#include "device_registry.h"
#include "actuator_client.h"
#include "sensor_client.h"

#define TAG "THREAD_COMM"
#define COMMISSIONER_UNIVERSAL_PSKD "DATN2026"

static bool                          s_scanning = false;
static uint32_t                      s_remaining_sec = 0;
static TimerHandle_t                 s_countdown_timer = NULL;
static commissioner_discovery_cb_t   s_discovery_cb = NULL;
static commissioner_pair_result_cb_t s_pair_result_cb = NULL;
static char                          s_approving_id_str[32] = {0};
static volatile bool                 s_joiner_finalized = false;
static volatile bool                 s_pairing_active = false;
static int                           s_pairing_ticks = 0;
static volatile commissioner_dev_type_t s_reported_pairing_type = COMMISSIONER_DEV_UNKNOWN;

static bool                          s_coap_probe_sent = false;
static int64_t                       s_probe_start_ms = 0;
static int64_t                       s_probe_send_time_ms = 0;
static thread_dev_type_t             s_probing_type = THREAD_DEV_UNKNOWN;

void thread_commissioner_set_discovery_callback(commissioner_discovery_cb_t cb)
{
    s_discovery_cb = cb;
}

void thread_commissioner_set_pair_result_callback(commissioner_pair_result_cb_t cb)
{
    s_pair_result_cb = cb;
}

bool thread_commissioner_is_scanning(void)
{
    return s_scanning;
}

uint32_t thread_commissioner_get_remaining_sec(void)
{
    return s_remaining_sec;
}

static void countdown_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    static int s_half_sec_tick = 0;
    s_half_sec_tick++;

    // Cứ 2 lần tick (1000ms) thì đếm lùi scan window 60s
    if (s_half_sec_tick >= 2) {
        s_half_sec_tick = 0;
        if (s_remaining_sec > 0) {
            s_remaining_sec--;
        }
        if (s_remaining_sec == 0 && s_scanning && !s_pairing_active) {
            ESP_LOGI(TAG, "Pairing window expired (60s)");
            thread_commissioner_stop();
        }
    }

    // Nếu người dùng đã nhấn Duyệt và đang chờ xác thực hoàn tất kết nối:
    if (s_pairing_active) {
        s_pairing_ticks++;

        commissioner_dev_type_t dev_type = thread_commissioner_poll_joined_device();
        if (dev_type != COMMISSIONER_DEV_UNKNOWN) {
            s_pairing_active = false;
            const char *type_str = (dev_type == COMMISSIONER_DEV_RELAY) ? "relay" :
                                   (dev_type == COMMISSIONER_DEV_DIMMER) ? "dimmer" : "sensor";
            ESP_LOGI(TAG, ">>> Background Pairing SUCCESS: Node verified as '%s' (after %d ms) <<<",
                     type_str, s_pairing_ticks * 500);
            if (s_pair_result_cb != NULL) {
                s_pair_result_cb(true, type_str);
            }
            return;
        }

        // Hết thời gian chờ (30 giây = 60 ticks)
        if (s_pairing_ticks >= 60) {
            s_pairing_active = false;
            s_joiner_finalized = false;
            ESP_LOGW(TAG, ">>> Background Pairing TIMEOUT after 30s! Node did not register via SRP <<<");
            if (s_pair_result_cb != NULL) {
                s_pair_result_cb(false, NULL);
            }
        }
    }
}

static void commissioner_state_changed_cb(otCommissionerState aState, void *aContext)
{
    (void)aContext;
    const char *state_str = (aState == OT_COMMISSIONER_STATE_ACTIVE) ? "ACTIVE" :
                            (aState == OT_COMMISSIONER_STATE_PETITION) ? "PETITIONING" : "DISABLED";
    ESP_LOGI(TAG, "Commissioner state changed: %d (%s)", (int)aState, state_str);

    // BẮT BUỘC: otCommissionerAddJoiner CHỈ ĐƯỢC PHÉP GỌI KHI Commissioner đã ACTIVE (Đã hoàn tất Petitioning)
    if (aState == OT_COMMISSIONER_STATE_ACTIVE) {
        otInstance *instance = esp_openthread_get_instance();
        if (instance != NULL) {
            uint32_t timeout = (s_scanning && s_remaining_sec > 0) ? s_remaining_sec : 86400;
            otError add_err = otCommissionerAddJoiner(instance, NULL, COMMISSIONER_UNIVERSAL_PSKD, timeout);
            if (add_err == OT_ERROR_NONE) {
                ESP_LOGI(TAG, ">>> COMMISSIONER ACTIVE: Đã đăng ký Wildcard Joiner (PSKd='%s', timeout=%lu s) <<<",
                         COMMISSIONER_UNIVERSAL_PSKD, (unsigned long)timeout);
            } else {
                ESP_LOGE(TAG, "otCommissionerAddJoiner thất bại trong callback: %d", (int)add_err);
            }
        }
    }
}

void thread_commissioner_notify_report_received(commissioner_dev_type_t type)
{
    if (s_pairing_active || s_scanning) {
        s_reported_pairing_type = type;
        ESP_LOGI(TAG, "Notified report received during pairing/scan: type=%d", (int)type);
    }
}

commissioner_dev_type_t thread_commissioner_poll_joined_device(void)
{
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
        return COMMISSIONER_DEV_UNKNOWN;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        esp_openthread_lock_release();
        return COMMISSIONER_DEV_UNKNOWN;
    }

    thread_dev_type_t matched_type = THREAD_DEV_UNKNOWN;
    otIp6Address addr;
    uint16_t port;
    int64_t now_ms = esp_timer_get_time() / 1000;
    uint32_t max_fresh_age_ms = (s_probe_start_ms > 0 && now_ms >= s_probe_start_ms)
                                ? (uint32_t)(now_ms - s_probe_start_ms + 6000)
                                : 12000;

    // Chỉ tìm thiết bị khi Joiner đã hoàn tất bắt tay DTLS
    if (s_joiner_finalized) {
        // Ưu tiên 0: Nếu đã nhận trực tiếp bản tin CoAP /report từ node vừa gắn kết
        if (s_reported_pairing_type != COMMISSIONER_DEV_UNKNOWN) {
            thread_dev_type_t t = (thread_dev_type_t)s_reported_pairing_type;
            const char *service = (t == THREAD_DEV_RELAY) ? "_relay._udp" :
                                  (t == THREAD_DEV_DIMMER) ? "_dimmer._udp" : "_sensor._udp";
            const char *label   = (t == THREAD_DEV_RELAY) ? "relay-01" :
                                  (t == THREAD_DEV_DIMMER) ? "dimmer-01" : "sensor-01";

            if (device_registry_find_fresh_locked(instance, label, service, max_fresh_age_ms, &addr, &port) ||
                device_registry_find_fresh_locked(instance, device_registry_type_to_str(t), service, max_fresh_age_ms, &addr, &port) ||
                device_registry_find_locked(instance, label, service, &addr, &port) ||
                device_registry_find_locked(instance, device_registry_type_to_str(t), service, &addr, &port) ||
                device_registry_find_locked(instance, NULL, service, &addr, &port) ||
                device_registry_resolve_address_locked(instance, t, &addr, &port) == ESP_OK) {
                matched_type = t;
                ESP_LOGI(TAG, "Matched candidate from direct /report event: %s", device_registry_type_to_str(t));
            }
        }

        // Ưu tiên 1: Quét các thiết bị CHƯA đăng ký trong mạng có bản ghi SRP tươi mới
        if (matched_type == THREAD_DEV_UNKNOWN) {
            for (int i = 0; i < THREAD_DEV_COUNT; i++) {
                thread_dev_type_t t = (thread_dev_type_t)i;
                if (!device_registry_is_type_registered(t)) {
                    const char *service = (t == THREAD_DEV_RELAY) ? "_relay._udp" :
                                          (t == THREAD_DEV_DIMMER) ? "_dimmer._udp" : "_sensor._udp";
                    const char *label   = (t == THREAD_DEV_RELAY) ? "relay-01" :
                                          (t == THREAD_DEV_DIMMER) ? "dimmer-01" : "sensor-01";

                    if (device_registry_find_fresh_locked(instance, label, service, max_fresh_age_ms, &addr, &port) ||
                        device_registry_find_fresh_locked(instance, device_registry_type_to_str(t), service, max_fresh_age_ms, &addr, &port) ||
                        device_registry_find_fresh_locked(instance, NULL, service, max_fresh_age_ms, &addr, &port)) {
                        matched_type = t;
                        ESP_LOGI(TAG, "Matched fresh unregistered candidate: %s (age <= %lu ms)",
                                 device_registry_type_to_str(t), (unsigned long)max_fresh_age_ms);
                        break;
                    }
                }
            }
        }

        // Ưu tiên 2: Nếu chưa tìm thấy và tất cả các node đều đã đăng ký, tìm node vừa re-join fresh
        if (matched_type == THREAD_DEV_UNKNOWN) {
            for (int i = 0; i < THREAD_DEV_COUNT; i++) {
                thread_dev_type_t t = (thread_dev_type_t)i;
                const char *service = (t == THREAD_DEV_RELAY) ? "_relay._udp" :
                                      (t == THREAD_DEV_DIMMER) ? "_dimmer._udp" : "_sensor._udp";
                const char *label   = (t == THREAD_DEV_RELAY) ? "relay-01" :
                                      (t == THREAD_DEV_DIMMER) ? "dimmer-01" : "sensor-01";

                if (device_registry_find_fresh_locked(instance, label, service, max_fresh_age_ms, &addr, &port) ||
                    device_registry_find_fresh_locked(instance, device_registry_type_to_str(t), service, max_fresh_age_ms, &addr, &port) ||
                    device_registry_find_fresh_locked(instance, NULL, service, max_fresh_age_ms, &addr, &port)) {
                    matched_type = t;
                    ESP_LOGI(TAG, "Matched re-joining candidate: %s", device_registry_type_to_str(t));
                    break;
                }
            }
        }
    }

    esp_openthread_lock_release();

    if (matched_type == THREAD_DEV_UNKNOWN) {
        return COMMISSIONER_DEV_UNKNOWN;
    }

    // ĐÃ TÌM THẤY TRONG BẢNG SRP -> BƯỚC TIẾP THEO: ACTIVE COAP TWO-WAY PROBE!
    // Bắt buộc kiểm tra gói tin CoAP round-trip thực tế để đảm bảo định tuyến Thread Mesh đã sẵn sàng
    now_ms = esp_timer_get_time() / 1000;

    if (!s_coap_probe_sent || s_probing_type != matched_type) {
        s_probing_type = matched_type;
        s_coap_probe_sent = true;
        s_probe_send_time_ms = now_ms;

        ESP_LOGI(TAG, "Candidate node '%s' found in SRP! Sending Active CoAP Verification Probe...",
                 device_registry_type_to_str(matched_type));

        if (matched_type == THREAD_DEV_RELAY) {
            actuator_read_relay();
        } else if (matched_type == THREAD_DEV_DIMMER) {
            actuator_read_brightness();
        } else if (matched_type == THREAD_DEV_SENSOR) {
            sensor_client_poll_now();
        }
        return COMMISSIONER_DEV_UNKNOWN;
    }

    bool coap_confirmed = false;
    if (matched_type == THREAD_DEV_RELAY) {
        actuator_snapshot_t snap = {0};
        actuator_get_snapshot(ACTUATOR_RELAY, &snap);
        if (snap.status == ACTUATOR_CONFIRMED && snap.last_confirmed_ms >= s_probe_start_ms) {
            coap_confirmed = true;
            ESP_LOGI(TAG, ">>> Relay CoAP Round-Trip CONFIRMED (val=%d, RTT=%lld ms)! <<<",
                     snap.confirmed_value, (long long)(snap.last_confirmed_ms - s_probe_start_ms));
        } else if (now_ms - s_probe_send_time_ms >= 1500) {
            ESP_LOGD(TAG, "Retrying CoAP GET /relay probe...");
            actuator_read_relay();
            s_probe_send_time_ms = now_ms;
        }
    } else if (matched_type == THREAD_DEV_DIMMER) {
        actuator_snapshot_t snap = {0};
        actuator_get_snapshot(ACTUATOR_DIMMER, &snap);
        if (snap.status == ACTUATOR_CONFIRMED && snap.last_confirmed_ms >= s_probe_start_ms) {
            coap_confirmed = true;
            ESP_LOGI(TAG, ">>> Dimmer CoAP Round-Trip CONFIRMED (bri=%d%%, RTT=%lld ms)! <<<",
                     snap.confirmed_value, (long long)(snap.last_confirmed_ms - s_probe_start_ms));
        } else if (now_ms - s_probe_send_time_ms >= 1500) {
            ESP_LOGD(TAG, "Retrying CoAP GET /light probe...");
            actuator_read_brightness();
            s_probe_send_time_ms = now_ms;
        }
    } else if (matched_type == THREAD_DEV_SENSOR) {
        sensor_client_data_t snap = {0};
        sensor_client_get_snapshot(&snap);
        int64_t last_sensor_seen = device_registry_get_last_seen_ms(THREAD_DEV_SENSOR);
        if ((snap.has_valid_sample && snap.last_rx_time_ms >= s_probe_start_ms) ||
            (last_sensor_seen >= s_probe_start_ms)) {
            coap_confirmed = true;
            ESP_LOGI(TAG, ">>> Sensor Telemetry CONFIRMED from Thread Mesh (Temp=%.1f C, Hum=%.1f %%)! <<<",
                     snap.temperature_c, snap.humidity_pct);
        } else if (now_ms - s_probe_send_time_ms >= 1500) {
            ESP_LOGD(TAG, "Retrying Sensor CoAP poll probe...");
            sensor_client_poll_now();
            s_probe_send_time_ms = now_ms;
        }
    }

    if (!coap_confirmed) {
        return COMMISSIONER_DEV_UNKNOWN;
    }

    // CoAP Round-trip verified! Đăng ký chính thức vào mạng
    const char *type_str = device_registry_type_to_str(matched_type);
    ESP_LOGI(TAG, ">>> Device [%s] VERIFIED 100%% via SRP + CoAP Round-Trip! Saving to Registry... <<<", type_str);

    device_registry_record_type_activity(matched_type);
    device_registry_set_type_registered(matched_type, true);

    s_joiner_finalized = false;
    s_coap_probe_sent = false;
    s_probing_type = THREAD_DEV_UNKNOWN;
    s_reported_pairing_type = COMMISSIONER_DEV_UNKNOWN;

    return (commissioner_dev_type_t)matched_type;
}

static void commissioner_joiner_event_cb(otCommissionerJoinerEvent aEvent,
                                         const otJoinerInfo       *aJoinerInfo,
                                         const otExtAddress       *aJoinerId,
                                         void                     *aContext)
{
    (void)aJoinerInfo;
    (void)aContext;

    char id_str[24] = "unknown";
    if (aJoinerId != NULL) {
        snprintf(id_str, sizeof(id_str), "%02x%02x%02x%02x%02x%02x%02x%02x",
                 aJoinerId->m8[0], aJoinerId->m8[1], aJoinerId->m8[2], aJoinerId->m8[3],
                 aJoinerId->m8[4], aJoinerId->m8[5], aJoinerId->m8[6], aJoinerId->m8[7]);
    }

    ESP_LOGI(TAG, "Joiner Event %d from [%s]", (int)aEvent, id_str);

    // Khi Joiner bắt đầu bắt tay DTLS hoặc đã kết nối, thông báo lên UI để hiển thị Thẻ Phê Duyệt
    if (aEvent == OT_COMMISSIONER_JOINER_START || aEvent == OT_COMMISSIONER_JOINER_CONNECTED) {
        if (s_discovery_cb != NULL) {
            s_discovery_cb(COMMISSIONER_DEV_UNKNOWN, "Thiết bị Thread mới", id_str, -45);
        }
    }

    // Khi một Joiner hoàn tất bắt tay DTLS
    if (aEvent == OT_COMMISSIONER_JOINER_FINALIZE || aEvent == OT_COMMISSIONER_JOINER_END || aEvent == OT_COMMISSIONER_JOINER_REMOVED) {
        if (aEvent == OT_COMMISSIONER_JOINER_FINALIZE || aEvent == OT_COMMISSIONER_JOINER_END) {
            s_joiner_finalized = true;
            ESP_LOGI(TAG, ">>> Joiner finalized for [%s]! Waiting for SRP registration... <<<", id_str);
        }

        otInstance *instance = esp_openthread_get_instance();
        if (instance != NULL && otCommissionerGetState(instance) == OT_COMMISSIONER_STATE_ACTIVE) {
            otError re_err = otCommissionerAddJoiner(instance, NULL, COMMISSIONER_UNIVERSAL_PSKD, 86400);
            if (re_err == OT_ERROR_NONE) {
                ESP_LOGI(TAG, ">>> Đã tự động tái kích hoạt Wildcard Joiner (PSKd='%s', timeout=86400s) cho các node kế tiếp <<<",
                         COMMISSIONER_UNIVERSAL_PSKD);
            }
        }
    }
}

esp_err_t thread_commissioner_init(void)
{
    if (s_countdown_timer == NULL) {
        s_countdown_timer = xTimerCreate("comm_timer", pdMS_TO_TICKS(500), pdTRUE, NULL, countdown_timer_cb);
        if (s_countdown_timer == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (esp_openthread_lock_acquire(portMAX_DELAY)) {
        otInstance *instance = esp_openthread_get_instance();
        if (instance != NULL) {
            otCommissionerState cur_state = otCommissionerGetState(instance);
            if (cur_state == OT_COMMISSIONER_STATE_DISABLED) {
                otError err = otCommissionerStart(instance, commissioner_state_changed_cb, commissioner_joiner_event_cb, NULL);
                ESP_LOGI(TAG, "Commissioner auto-started on boot (res=%d)", (int)err);
            }
        }
        esp_openthread_lock_release();
    }
    return ESP_OK;
}

void thread_commissioner_on_role_changed(otDeviceRole role)
{
    if (role == OT_DEVICE_ROLE_LEADER || role == OT_DEVICE_ROLE_ROUTER) {
        if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(500))) {
            return;
        }
        otInstance *instance = esp_openthread_get_instance();
        if (instance != NULL) {
            otCommissionerState cur_state = otCommissionerGetState(instance);
            if (cur_state == OT_COMMISSIONER_STATE_DISABLED) {
                otError err = otCommissionerStart(instance, commissioner_state_changed_cb, commissioner_joiner_event_cb, NULL);
                ESP_LOGI(TAG, "OpenThread role is %s -> Starting Commissioner (res=%d)...",
                         otThreadDeviceRoleToString(role), (int)err);
            }
        }
        esp_openthread_lock_release();
    }
}

esp_err_t thread_commissioner_start_scan(uint32_t timeout_sec)
{
    if (timeout_sec == 0) timeout_sec = 60;
    s_remaining_sec = timeout_sec;
    s_scanning = true;
    s_joiner_finalized = false;
    s_reported_pairing_type = COMMISSIONER_DEV_UNKNOWN;

    if (!esp_openthread_lock_acquire(portMAX_DELAY)) {
        return ESP_ERR_TIMEOUT;
    }

    otInstance *instance = esp_openthread_get_instance();
    if (instance == NULL) {
        esp_openthread_lock_release();
        return ESP_ERR_INVALID_STATE;
    }

    otDeviceRole role = otThreadGetDeviceRole(instance);
    if (role != OT_DEVICE_ROLE_LEADER && role != OT_DEVICE_ROLE_ROUTER) {
        ESP_LOGW(TAG, "Gateway role is %s (not Leader/Router). Forcing become Leader...", otThreadDeviceRoleToString(role));
        otThreadBecomeLeader(instance);
    }

    otCommissionerState cur_state = otCommissionerGetState(instance);
    if (cur_state == OT_COMMISSIONER_STATE_ACTIVE) {
        otError add_err = otCommissionerAddJoiner(instance, NULL, COMMISSIONER_UNIVERSAL_PSKD, timeout_sec);
        if (add_err == OT_ERROR_NONE) {
            ESP_LOGI(TAG, "Commissioner đã ACTIVE: Tái kích hoạt Wildcard Joiner (PSKd='%s', timeout=%lu s)",
                     COMMISSIONER_UNIVERSAL_PSKD, (unsigned long)timeout_sec);
        } else {
            ESP_LOGE(TAG, "otCommissionerAddJoiner thất bại: %d", (int)add_err);
        }
    } else {
        otError err = otCommissionerStart(instance, commissioner_state_changed_cb, commissioner_joiner_event_cb, NULL);
        if (err != OT_ERROR_NONE && err != OT_ERROR_ALREADY) {
            ESP_LOGE(TAG, "otCommissionerStart thất bại: %d", (int)err);
            s_scanning = false;
            s_remaining_sec = 0;
            esp_openthread_lock_release();
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "otCommissionerStart đã kích hoạt (state=%d), đang chờ callback ACTIVE để đăng ký Joiner...", (int)cur_state);
    }

    esp_openthread_lock_release();

    if (s_countdown_timer != NULL) {
        xTimerStart(s_countdown_timer, 0);
    }

    return ESP_OK;
}

esp_err_t thread_commissioner_stop(void)
{
    s_scanning = false;
    s_remaining_sec = 0;
    s_pairing_active = false;
    s_joiner_finalized = false;
    s_coap_probe_sent = false;
    s_probing_type = THREAD_DEV_UNKNOWN;
    s_reported_pairing_type = COMMISSIONER_DEV_UNKNOWN;
    if (s_countdown_timer != NULL) {
        xTimerStop(s_countdown_timer, 0);
    }
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) {
        otInstance *instance = esp_openthread_get_instance();
        if (instance != NULL && otCommissionerGetState(instance) == OT_COMMISSIONER_STATE_ACTIVE) {
            otCommissionerAddJoiner(instance, NULL, COMMISSIONER_UNIVERSAL_PSKD, 86400);
        }
        esp_openthread_lock_release();
    }
    ESP_LOGI(TAG, "Pairing scan window closed (Wildcard Joiner maintained in background)");
    return ESP_OK;
}

esp_err_t thread_commissioner_approve_device(const char *dev_id_str)
{
    if (dev_id_str == NULL) return ESP_ERR_INVALID_ARG;
    strncpy(s_approving_id_str, dev_id_str, sizeof(s_approving_id_str) - 1);
    s_pairing_active = true;
    s_pairing_ticks = 0;
    s_coap_probe_sent = false;
    s_probing_type = THREAD_DEV_UNKNOWN;
    s_reported_pairing_type = COMMISSIONER_DEV_UNKNOWN;
    s_probe_start_ms = esp_timer_get_time() / 1000;
    s_probe_send_time_ms = 0;
    if (s_countdown_timer != NULL) {
        xTimerStart(s_countdown_timer, 0);
    }
    ESP_LOGI(TAG, "Device [%s] APPROVED by user. Starting background SRP + CoAP round-trip verification...", dev_id_str);
    return ESP_OK;
}

esp_err_t thread_commissioner_remove_device(const char *dev_id_str)
{
    if (dev_id_str == NULL) return ESP_ERR_INVALID_ARG;
    device_registry_clear_cache(dev_id_str);
    ESP_LOGI(TAG, "Device [%s] REMOVED from registry", dev_id_str);
    return ESP_OK;
}

bool thread_commissioner_is_device_approved(const char *dev_id_str)
{
    if (dev_id_str == NULL) return false;
    return device_registry_is_registered(dev_id_str);
}
