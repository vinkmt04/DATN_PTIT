idf_component_register(SRCS "thread_gateway.c"
                       INCLUDE_DIRS "."
                       REQUIRES openthread esp_netif nvs_flash driver)
