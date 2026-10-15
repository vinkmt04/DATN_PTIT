idf_component_register(SRCS "sys_core.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_event esp_netif driver wifi_provisioning)
