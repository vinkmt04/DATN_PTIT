/*
 * ESP-IDF OpenThread Configuration for ESP32-S3 Gateway
 * Spinel UART connected to ESP32-H2 RCP:
 *   - TX: GPIO 18 (ESP32-S3 TX -> ESP32-H2 RX)
 *   - RX: GPIO 14 (ESP32-S3 RX <- ESP32-H2 TX)
 *   - Baudrate: 460800 bps
 */

#pragma once

#include "esp_openthread_types.h"
#include "driver/uart.h"

#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()              \
    {                                                      \
        .radio_mode = RADIO_MODE_UART_RCP,                 \
        .radio_uart_config = {                             \
            .port = UART_NUM_1,                            \
            .uart_config =                                 \
                {                                          \
                    .baud_rate = 460800,                   \
                    .data_bits = UART_DATA_8_BITS,         \
                    .parity = UART_PARITY_DISABLE,         \
                    .stop_bits = UART_STOP_BITS_1,         \
                    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, \
                    .rx_flow_ctrl_thresh = 0,              \
                    .source_clk = UART_SCLK_DEFAULT,       \
                },                                         \
            .rx_pin = 14,                                  \
            .tx_pin = 18,                                  \
        },                                                 \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()               \
    {                                                      \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE, \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()               \
    {                                                      \
        .storage_partition_name = "nvs",                   \
        .netif_queue_size = 10,                            \
        .task_queue_size = 10,                             \
    }
