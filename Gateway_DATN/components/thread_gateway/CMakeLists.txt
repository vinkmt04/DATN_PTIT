idf_component_register(SRCS "thread_gateway.c" "device_registry.c" "actuator_client.c" "sensor_client.c" "thread_commissioner.c"
                        INCLUDE_DIRS "."
                        REQUIRES openthread esp_netif nvs_flash driver json mdns esp_timer freertos)

if(TARGET openthread_br)
    target_link_libraries(openthread_br INTERFACE espressif__mdns)
endif()
