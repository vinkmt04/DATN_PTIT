idf_component_register(SRCS "audio_hal.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2s freertos esp_ringbuf)
