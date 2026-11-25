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
#include "mdns.h"
#include <time.h>
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_memory_utils.h"
#include "driver/gpio.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
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
static volatile bool s_wifi_connecting = false;
static bool s_is_provisioned = false;
static TimerHandle_t s_reconnect_timer = NULL;
static sys_core_is_busy_cb_t s_voice_busy_cb = NULL;

static uint32_t calculate_backoff_ms(int retry) {
    if (retry <= 0) return 1000;  // Lần 1: 1s
    if (retry == 1) return 2000;  // Lần 2: 2s
    if (retry == 2) return 4000;  // Lần 3: 4s
    return 8000;                  // Lần 4+: 8s
}

void sys_core_set_voice_busy_check(sys_core_is_busy_cb_t cb) {
    s_voice_busy_cb = cb;
}

static void wifi_reconnect_timer_cb(TimerHandle_t xTimer) {
    (void)xTimer;
    if (s_is_connected || s_wifi_connecting) {
        if (s_is_connected && s_reconnect_timer) {
            xTimerStop(s_reconnect_timer, 0);
        }
        return;
    }

    // Ưu tiên giọng nói: Nếu người dùng đang ra lệnh Offline (thu âm / suy luận) thì hoãn lượt quét
    if (s_voice_busy_cb && s_voice_busy_cb()) {
        ESP_LOGD(TAG, "[WIFI_RECONNECT] Hoãn quét Wi-Fi vì hệ thống giọng nói đang bận...");
        if (s_reconnect_timer) {
            xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(2000), 0);
        }
        return;
    }

    s_retry_num++;
    uint32_t next_ms = calculate_backoff_ms(s_retry_num);
    ESP_LOGI(TAG, ">>> [WIFI_RECONNECT] Thử kết nối lại Wi-Fi lần %d <<<", s_retry_num);
    s_wifi_connecting = true;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        s_wifi_connecting = false;
        ESP_LOGW(TAG, "[WIFI_RECONNECT] esp_wifi_connect() trả lỗi 0x%x -> Hẹn lại sau %lums",
                 (int)err, (unsigned long)next_ms);
        if (s_reconnect_timer) {
            xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(next_ms), 0);
        }
    }
}

static void time_sync_notification_cb(struct timeval *tv) {
    (void)tv;
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    char strftime_buf[64];
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, ">>> [SNTP SYNC SUCCESS] Thời gian hệ thống đã cập nhật tức thì: %s (GMT+7) <<<", strftime_buf);
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
        esp_wifi_set_max_tx_power(68); // 17 dBm: giảm ~40% xung dòng đỉnh 3.3V, hạn chế gợn nguồn vào mic INMP441
        s_wifi_connecting = true;
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            s_wifi_connecting = false;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        s_wifi_connecting = false;
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        uint32_t initial_delay = calculate_backoff_ms(s_retry_num);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason: %d). Sẽ tự động kết nối lại nhanh sau %lums...",
                 disc ? disc->reason : 0, (unsigned long)initial_delay);

        // Khởi động timer thử lại với backoff nhanh trong nền
        if (s_reconnect_timer) {
            xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(initial_delay), 0);
            xTimerStart(s_reconnect_timer, 0);
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, ">>> [WIFI CONNECTED] Đã nhận địa chỉ IP: " IPSTR " <<<", IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_is_connected = true;
        s_wifi_connecting = false;
        // Tắt hoàn toàn Modem Sleep của Wi-Fi để đảm bảo độ trễ thấp nhất và không bao giờ rớt gói tin WebSocket audio
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (s_reconnect_timer) {
            xTimerStop(s_reconnect_timer, 0);
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);

        // Khởi động đồng bộ giờ SNTP tức thì (Múi giờ GMT+7 Việt Nam)
        setenv("TZ", "ICT-7", 1);
        tzset();
        if (!esp_sntp_enabled()) {
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
            esp_sntp_setservername(0, "time.google.com");
            esp_sntp_setservername(1, "pool.ntp.org");
            esp_sntp_setservername(2, "time.cloudflare.com");
            esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
            esp_sntp_init();
            ESP_LOGI(TAG, "SNTP Started (IMMED mode): Syncing network time to GMT+7 via Google/Pool/Cloudflare NTP...");
        } else {
            esp_sntp_restart();
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void deferred_wifi_reset(void *pvParameter1, uint32_t ulParameter2) {
    (void)pvParameter1;
    (void)ulParameter2;
    ESP_LOGW(TAG, "Deferred Wi-Fi reset running in Timer Service task (DRAM stack)...");
    wifi_prov_mgr_reset_provisioning();
    nvs_flash_erase();
    esp_restart();
}

void sys_core_reset_wifi(void) {
    ESP_LOGW(TAG, "Resetting Wi-Fi credentials and restarting to Provisioning mode...");
    int dummy = 0;
    if (esp_ptr_external_ram(&dummy)) {
        ESP_LOGI(TAG, "Caller stack is in PSRAM, deferring Wi-Fi reset to Timer Service task");
        if (xTimerPendFunctionCall(deferred_wifi_reset, NULL, 0, pdMS_TO_TICKS(500)) != pdPASS) {
            ESP_LOGE(TAG, "Failed to pend deferred_wifi_reset!");
            wifi_prov_mgr_reset_provisioning();
            nvs_flash_erase();
            esp_restart();
        }
    } else {
        wifi_prov_mgr_reset_provisioning();
        nvs_flash_erase();
        esp_restart();
    }
}

static void *cjson_psram_malloc(size_t sz) {
    void *p = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sz);
    return p;
}

static void cjson_psram_free(void *ptr) {
    if (ptr) heap_caps_free(ptr);
}

void sys_core_platform_init(void) {
    static bool s_platform_inited = false;
    if (s_platform_inited) return;
    s_platform_inited = true;

    // Chuyển toàn bộ cấp phát cJSON AST sang PSRAM để giải phóng Internal DRAM
    cJSON_Hooks hooks = {
        .malloc_fn = cjson_psram_malloc,
        .free_fn = cjson_psram_free
    };
    cJSON_InitHooks(&hooks);
    ESP_LOGI(TAG, "cJSON dynamic allocations routed to Octal PSRAM.");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t ev_ret = esp_event_loop_create_default();
    if (ev_ret != ESP_OK && ev_ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ev_ret);
    }

    // Khởi tạo mDNS sớm cho OpenThread Border Router (khử hoàn toàn lỗi meshcop mdns)
    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        mdns_hostname_set("esp-ot-br");
        mdns_instance_name_set("ESP32-S3 Thread Border Router");
        ESP_LOGI(TAG, "mDNS initialized successfully for OpenThread Border Router (esp-ot-br)");
    } else {
        ESP_LOGW(TAG, "mDNS initialization returned %d", (int)mdns_err);
    }
}

void sys_core_init(void) {
    ESP_LOGI(TAG, "sys_core initialized (Wi-Fi STA / SoftAP Provisioning)");
    sys_core_platform_init();
    
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
        esp_wifi_set_max_tx_power(68); // Trần công suất 17 dBm (giảm xung dòng đỉnh 3.3V)
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    }

    // Khởi tạo Software Timer kết nối lại Wi-Fi trong nền với backoff linh hoạt (khởi đầu 1000ms)
    if (!s_reconnect_timer) {
        s_reconnect_timer = xTimerCreate("wifi_reconn_tmr",
                                          pdMS_TO_TICKS(1000),
                                          pdFALSE, // Oneshot: mỗi lần cb sẽ tính chu kỳ mới
                                          NULL,
                                          wifi_reconnect_timer_cb);
    }

    // Chờ tối đa 4.0 giây để kết nối Wi-Fi lúc khởi động (Sweet-Spot Boot Timeout cho Router DHCP):
    // - Nếu có mạng (bình thường DHCP mất 2.0s - 3.5s): Trả về ngay lập tức để kịp phiên Online.
    // - Nếu mất mạng / router tắt: Không bị treo kẹt, unblock sau 4.0s để chuyển sang chế độ Cục bộ (Offline)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(4000));
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

void sys_core_print_memory_stats(const char *phase_tag)
{
    size_t free_dram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t min_dram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_dram = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t min_psram = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

    ESP_LOGI("SYS_MEM", "=================================================");
    ESP_LOGI("SYS_MEM", ">>> [MEMORY AUDIT - %s] <<<", phase_tag ? phase_tag : "RUNTIME");
    ESP_LOGI("SYS_MEM", "  Internal DRAM (SRAM) : %u KB Free (Min Ever: %u KB, Max Block: %u KB)",
             (unsigned)(free_dram / 1024), (unsigned)(min_dram / 1024), (unsigned)(largest_dram / 1024));
    ESP_LOGI("SYS_MEM", "  Octal PSRAM (SPIRAM) : %u KB Free (Min Ever: %u KB)",
             (unsigned)(free_psram / 1024), (unsigned)(min_psram / 1024));
    ESP_LOGI("SYS_MEM", "=================================================");
}
