#include "sys_core.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#include <string.h>

static const char *TAG = "sys_core";

#define PROV_SECURITY WIFI_PROV_SECURITY_1
#define PROV_POP "12345678" // Mật khẩu cấu hình (Proof of Possession)
#define MAXIMUM_RETRY 5
#define BOOT_BUTTON_GPIO 0

#define DEFAULT_WIFI_SSID "FULL A+"
#define DEFAULT_WIFI_PASS "matkhaugi"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
static bool s_is_connected = false;
static bool s_is_provisioned = false;

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
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        if (s_is_provisioned) {
            ESP_LOGW(TAG, "Wi-Fi disconnected. Reconnecting to AP in 1s...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        } else {
            if (s_retry_num < MAXIMUM_RETRY) {
                esp_wifi_connect();
                s_retry_num++;
                ESP_LOGI(TAG, "retry to connect to the AP (%d/%d)", s_retry_num, MAXIMUM_RETRY);
            } else {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_is_connected = true;
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
    }
    
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

bool sys_core_is_wifi_connected(void) {
    return s_is_connected;
}
