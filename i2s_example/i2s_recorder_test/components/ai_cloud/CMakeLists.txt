idf_component_register(SRCS "ai_cloud.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_client json mbedtls esp-tls esp_websocket_client audio_hal mesh_controller)
