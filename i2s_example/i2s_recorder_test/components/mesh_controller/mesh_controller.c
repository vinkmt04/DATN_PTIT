#include "mesh_controller.h"
#include "esp_log.h"
#include "driver/uart.h"
#include <string.h>

static const char *TAG = "mesh_controller";

#define MESH_UART_NUM      UART_NUM_1
#define MESH_UART_TX_PIN   13 // Đổi từ 43 sang 13 để tránh xung đột Console UART0
#define MESH_UART_RX_PIN   14 // Đổi từ 44 sang 14 để tránh xung đột Console UART0
#define MESH_UART_BUF_SIZE 1024

void mesh_controller_init(void) {
    ESP_LOGI(TAG, "Initializing Mesh Controller (UART to Coprocessor)");

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

    ESP_ERROR_CHECK(uart_driver_install(MESH_UART_NUM, MESH_UART_BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(MESH_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(MESH_UART_NUM, MESH_UART_TX_PIN, MESH_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART initialized on TX:%d RX:%d", MESH_UART_TX_PIN, MESH_UART_RX_PIN);
}

void mesh_controller_send_command(const char *cmd_str) {
    if (cmd_str == NULL) return;
    
    // Add newline character if not present for the protocol parser on H2/C6
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s\n", cmd_str);

    uart_write_bytes(MESH_UART_NUM, buffer, strlen(buffer));
    ESP_LOGI(TAG, "Sent to Thread Mesh: %s", cmd_str);
}
