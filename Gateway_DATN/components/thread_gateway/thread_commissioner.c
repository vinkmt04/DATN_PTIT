/*
 * THREAD COMMISSIONER MODULE - thread_gateway
 *
 * Quan ly cua so ghep noi 60 giay (OpenThread Commissioner),
 * tiep nhan Joiner tu cac Node va thuc hien quy trinh phe duyet (Approval Flow).
 */

#include "thread_commissioner.h"
#include <string.h>
#include <stdio.h>

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

#define TAG "THREAD_COMM"
#define NVS_NAMESPACE_DEV "tg_devs"

/* Ma PIN Universal Commissioning dung chung cho toan bo he thong Matter/Thread */
#define COMMISSIONER_UNIVERSAL_PSKD "DATN2026"

static bool                        s_scanning = false;
static uint32_t                    s_remaining_sec = 0;
static TimerHandle_t               s_countdown_timer = NULL;
static commissioner_discovery_cb_t   s_discovery_cb = NULL;
static commissioner_pair_result_cb_t s_pair_result_cb = NULL;

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
    if (s_remaining_sec > 0) {
        s_remaining_sec--;
    }
    if (s_remaining_sec == 0) {
        ESP_LOGI(TAG, "Pairing window expired (60s)");
        thread_commissioner_stop();
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

    // Khi Joiner bat dau bat tay DTLS hoac da ket noi, thong bao len UI de hien thi The Phe Duyet
    if (aEvent == OT_COMMISSIONER_JOINER_START || aEvent == OT_COMMISSIONER_JOINER_CONNECTED) {
        if (s_discovery_cb != NULL) {
            s_discovery_cb(COMMISSIONER_DEV_UNKNOWN, "Thiết bị Thread mới", id_str, -45);
        }
    }

    // Khi một Joiner hoàn tất, kết thúc hoặc bị xoá khỏi danh sách (sau khi ghép nối thành công),
    // Tự động tái kích hoạt Wildcard Joiner để không bị khoá mạng đối với các Node tiếp theo!
    if (aEvent == OT_COMMISSIONER_JOINER_FINALIZE || aEvent == OT_COMMISSIONER_JOINER_END || aEvent == OT_COMMISSIONER_JOINER_REMOVED) {
        if (aEvent == OT_COMMISSIONER_JOINER_FINALIZE || aEvent == OT_COMMISSIONER_JOINER_END) {
            if (s_pair_result_cb != NULL) {
                s_pair_result_cb(true, id_str);
            }
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
        s_countdown_timer = xTimerCreate("comm_timer", pdMS_TO_TICKS(1000), pdTRUE, NULL, countdown_timer_cb);
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

esp_err_t thread_commissioner_start_scan(uint32_t timeout_sec)
{
    if (timeout_sec == 0) timeout_sec = 60;
    s_remaining_sec = timeout_sec;
    s_scanning = true;

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
    if (s_countdown_timer != NULL) {
        xTimerStop(s_countdown_timer, 0);
    }
    ESP_LOGI(TAG, "Pairing scan window closed");
    return ESP_OK;
}

esp_err_t thread_commissioner_approve_device(const char *dev_id_str)
{
    if (dev_id_str == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_DEV, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint8_t approved = 1;
    err = nvs_set_u8(h, dev_id_str, approved);
    if (err == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(TAG, "Device [%s] APPROVED and saved to NVS", dev_id_str);
    }
    nvs_close(h);
    return err;
}

esp_err_t thread_commissioner_remove_device(const char *dev_id_str)
{
    if (dev_id_str == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_DEV, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_erase_key(h, dev_id_str);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Device [%s] REMOVED from NVS registry", dev_id_str);
    return ESP_OK;
}

bool thread_commissioner_is_device_approved(const char *dev_id_str)
{
    if (dev_id_str == NULL) return false;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE_DEV, NVS_READONLY, &h);
    if (err != ESP_OK) return false;

    uint8_t approved = 0;
    err = nvs_get_u8(h, dev_id_str, &approved);
    nvs_close(h);

    return (err == ESP_OK && approved == 1);
}
