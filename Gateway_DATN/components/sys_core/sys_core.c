#include "sys_core.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_sntp.h"
#include <time.h>
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#include <string.h>

static const char *TAG = "sys_core";

#define PROV_SECURITY WIFI_PROV_SECURITY_1
#define PROV_POP "12345678" // Mật khẩu cấu hình (Proof of Possession)
#if __has_include("app_secrets.h")
#include "app_secrets.h"
#endif

#define MAXIMUM_RETRY 5
#define BOOT_BUTTON_GPIO 0

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID "YOUR_WIFI_SSID"
#endif
#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS "YOUR_WIFI_PASS"
#endif

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static bool s_is_connected = false;
static bool s_is_provisioned = false;
static TimerHandle_t s_reconnect_timer = NULL;
static sys_core_is_busy_cb_t s_voice_busy_cb = NULL;

#define WIFI_RECONNECT_INTERVAL_MS 15000 // Thử kết nối lại mỗi 15 giây ở chế độ nền

void sys_core_set_voice_busy_check(sys_core_is_busy_cb_t cb) {
    s_voice_busy_cb = cb;
}

static void wifi_reconnect_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    if (s_is_connected) {
        if (s_reconnect_timer) xTimerStop(s_reconnect_timer, 0);
        return;
    }

    // Ưu tiên giọng nói: Nếu người dùng đang ra lệnh Offline (thu âm / suy luận) thì hoãn lượt quét
    if (s_voice_busy_cb && s_voice_busy_cb()) {
        ESP_LOGD(TAG, "[WIFI_RECONNECT] Hoãn quét Wi-Fi vì hệ thống giọng nói đang bận...");
        return;
    }

    ESP_LOGI(TAG, ">>> [WIFI_RECONNECT] Định kỳ 15s: Đang thử kết nối lại Wi-Fi trong nền... <<<");
    esp_wifi_connect();
}

static void time_sync_notification_cb(struct timeval *tv) {
    (void)tv;
    ESP_LOGI(TAG, ">>> [SNTP SYNC SUCCESS] Thời gian hệ thống đã được cập nhật tức thì GMT+7! <<<");
}

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
            case WIFI_PROV_START:
                ESP_LOGI(TAG, "Provisioning started");
                break;
            case WIFI_PROV_CRED_RECV:
                ESP_LOGI(TAG, "Received Wi-Fi credentials");
                break;
            case WIFI_PROV_CRED_FAIL: {
                wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
                ESP_LOGE(TAG, "Provisioning failed! Reason: %s",
                         (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "AUTH_FAILED" : "NETWORK_NOT_FOUND");
                break;
            }
            case WIFI_PROV_CRED_SUCCESS:
                ESP_LOGI(TAG, "Provisioning successful");
                s_is_provisioned = true;
                break;
            case WIFI_PROV_END:
                ESP_LOGI(TAG, "Provisioning end");
                wifi_prov_mgr_deinit();
                break;
            default:
                break;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_set_ps(WIFI_PS_NONE);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason: %d). Sẽ tự động kết nối lại mỗi 15s trong nền.", disc ? disc->reason : 0);

        // Khởi động timer định kỳ 15s trong nền (Non-blocking, KHÔNG gọi vTaskDelay trong event loop!)
        if (s_reconnect_timer) {
            xTimerStart(s_reconnect_timer, 0);
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, ">>> [WIFI CONNECTED] Đã nhận địa chỉ IP: " IPSTR " <<<", IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_is_connected = true;
        // Tắt hoàn toàn Modem Sleep của Wi-Fi để đảm bảo độ trễ thấp nhất và không bao giờ rớt gói tin WebSocket audio
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (s_reconnect_timer) {
            xTimerStop(s_reconnect_timer, 0);
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);

        // Khởi động đồng bộ giờ SNTP tức thì (Múi giờ GMT+7 Việt Nam)
        if (!esp_sntp_enabled()) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
            esp_sntp_setservername(0, "vn.pool.ntp.org");
            esp_sntp_setservername(1, "asia.pool.ntp.org");
            esp_sntp_setservername(2, "time.google.com");
            esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
            esp_sntp_init();
            setenv("TZ", "ICT-7", 1);
            tzset();
            ESP_LOGI(TAG, "SNTP Started (IMMED mode): Syncing network time to GMT+7 via VN/Asia/Google NTP...");
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void sys_core_reset_wifi(void) {
    ESP_LOGW(TAG, "Resetting Wi-Fi credentials and restarting to Provisioning mode...");
    wifi_prov_mgr_reset_provisioning();
    nvs_flash_erase();
    esp_restart();
}

void sys_core_init(void) {
    ESP_LOGI(TAG, "sys_core initialized");
    
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    s_wifi_event_group = xEventGroupCreate();

    esp_event_handler_instance_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
    };
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));

    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

    // Nếu chưa provisioned HOẶC SSID hiện tại khác DEFAULT_WIFI_SSID ("Vinh") -> Tự động nạp Wi-Fi mặc định
    if (!provisioned || strcmp((char *)wifi_cfg.sta.ssid, DEFAULT_WIFI_SSID) != 0) {
        ESP_LOGI(TAG, "Thiết lập Wi-Fi mặc định: SSID='%s'", DEFAULT_WIFI_SSID);
        memset(&wifi_cfg, 0, sizeof(wifi_cfg));
        strncpy((char *)wifi_cfg.sta.ssid, DEFAULT_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
        strncpy((char *)wifi_cfg.sta.password, DEFAULT_WIFI_PASS, sizeof(wifi_cfg.sta.password));
        wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
        provisioned = true;
    }
    s_is_provisioned = provisioned;

    if (!provisioned) {
        ESP_LOGI(TAG, "Starting Wi-Fi provisioning (SoftAP)...");
        char service_name[] = "PROV_Voice_Gateway";
        wifi_prov_security_t security = PROV_SECURITY;
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, PROV_POP, service_name, NULL));
        
        ESP_LOGI(TAG, "=================================================");
        ESP_LOGI(TAG, "1. Tải app 'ESP SoftAP Provisioning' trên iOS/Android");
        ESP_LOGI(TAG, "2. Chọn 'Provision Device' -> 'SoftAP'");
        ESP_LOGI(TAG, "3. Nhập mật khẩu Pop: %s", PROV_POP);
        ESP_LOGI(TAG, "4. Kết nối tới Wi-Fi: %s", service_name);
        ESP_LOGI(TAG, "=================================================");
    } else {
        ESP_LOGI(TAG, "Already provisioned. Connecting to Wi-Fi...");
        wifi_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    }

    // Khởi tạo Software Timer kết nối lại Wi-Fi trong nền (chu kỳ 15s, auto-reload)
    if (!s_reconnect_timer) {
        s_reconnect_timer = xTimerCreate("wifi_reconn_tmr",
                                          pdMS_TO_TICKS(WIFI_RECONNECT_INTERVAL_MS),
                                          pdTRUE, // Auto-reload
                                          NULL,
                                          wifi_reconnect_timer_cb);
    }

    // Chờ tối đa 3.5 giây để kết nối Wi-Fi lúc khởi động (Sweet-Spot Boot Timeout):
    // - Nếu có mạng (bình thường mất 1.5s - 2.0s): Trả về ngay lập tức để kịp phiên Online.
    // - Nếu mất mạng / router tắt: Không bị treo kẹt, unblock sau 3.5s để khởi động toàn bộ Mic, Loa, Mesh, Edge AI chạy Offline trơn tru!
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(3500));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, ">>> [BOOT ONLINE] Kết nối Wi-Fi thành công lúc khởi động! <<<");
        ESP_LOGI(TAG, "==================================================");
    } else {
        ESP_LOGW(TAG, "==================================================");
        ESP_LOGW(TAG, ">>> [BOOT OFFLINE] Không có Wi-Fi sau 3.5s -> Khởi động chế độ OFFLINE! <<<");
        ESP_LOGW(TAG, ">>> Mic, Loa, Mesh Controller và Edge AI sẵn sàng 100%% không cần Internet <<<");
        ESP_LOGW(TAG, ">>> Luồng kết nối Wi-Fi sẽ tự động thử lại mỗi 15s ở chế độ nền <<<");
        ESP_LOGW(TAG, "==================================================");
        if (s_reconnect_timer) {
            xTimerStart(s_reconnect_timer, 0);
        }
    }
}

bool sys_core_is_wifi_connected(void) {
    return s_is_connected;
}

void sys_core_get_ip_str(char *out_str, size_t max_len) {
    if (!out_str || max_len == 0) return;
    if (!s_is_connected) {
        snprintf(out_str, max_len, "Offline");
        return;
    }
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(out_str, max_len, IPSTR, IP2STR(&ip_info.ip));
            return;
        }
    }
    snprintf(out_str, max_len, "192.168.?.?");
}

int8_t sys_core_get_wifi_rssi(void) {
    if (!s_is_connected) return -100;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return -100;
}

void sys_core_get_wifi_ssid(char *out_str, size_t max_len) {
    if (!out_str || max_len == 0) return;
    if (!s_is_connected) {
        snprintf(out_str, max_len, "Disconnected");
        return;
    }
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        snprintf(out_str, max_len, "%s", (char*)ap_info.ssid);
        return;
    }
    snprintf(out_str, max_len, "Connected");
}
