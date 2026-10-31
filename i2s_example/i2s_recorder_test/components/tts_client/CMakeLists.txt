idf_component_register(SRCS "tts_client.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_http_client audio_hal mbedtls)
