idf_component_register(SRCS "iot_cloud.c"
                       INCLUDE_DIRS "."
                       REQUIRES mqtt json mesh_controller esp_netif esp_wifi)
