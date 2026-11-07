#include "mesh_controller.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "mesh_controller";

#define MESH_UART_NUM      UART_NUM_1
#define MESH_UART_TX_PIN   13 // Chân TX nối sang RX của ESP32-H2
#define MESH_UART_RX_PIN   14 // Chân RX nối sang TX của ESP32-H2
#define MESH_UART_BUF_SIZE 1024

#if MESH_ENABLE_RX_TASK
static TaskHandle_t s_rx_task_handle = NULL;

static void parse_mesh_rx_line(const char *line) {
    if (!line || strlen(line) == 0) return;

    // Chỉ chấp nhận bản tin có tiền tố hợp lệ trong giao thức (ACK:, DATA:, ERR:)
    if (strncmp(line, "ACK:", 4) != 0 && strncmp(line, "DATA:", 5) != 0 && strncmp(line, "ERR:", 4) != 0) {
        return;
    }

    ESP_LOGI(TAG, ">>> [THREAD MESH RX]: '%s' <<<", line);

    // 1. Phản hồi xác nhận thành công từ Node Đèn Dimmer
    // Ví dụ: "ACK:DIMMER:STATE=1,VAL=70" hoặc "ACK:DIMMER:VAL=70"
    if (strncmp(line, "ACK:DIMMER:", 11) == 0 || strncmp(line, "DATA:DIMMER:", 12) == 0) {
        int state = 1;
        int val = 0;
        if (strstr(line, "STATE=")) {
            sscanf(strstr(line, "STATE="), "STATE=%d,VAL=%d", &state, &val);
        } else if (strstr(line, "VAL=")) {
            sscanf(strstr(line, "VAL="), "VAL=%d", &val);
            state = (val > 0) ? 1 : 0;
        }
        if (val < 0) val = 0;
        if (val > 100) val = 100;
        gateway_state_set_dimmer(state == 1, (uint8_t)val);
        return;
    }

    // 2. Phản hồi xác nhận thành công từ Node Công tắc Relay 1 kênh
    // Ví dụ: "ACK:RELAY:STATE=1" hoặc "ACK:RELAY:ON"
    if (strncmp(line, "ACK:RELAY:", 10) == 0 || strncmp(line, "DATA:RELAY:", 11) == 0) {
        int state = 0;
        if (strstr(line, "STATE=")) {
            sscanf(strstr(line, "STATE="), "STATE=%d", &state);
        } else if (strstr(line, "ON")) {
            state = 1;
        } else if (strstr(line, "OFF")) {
            state = 0;
        }
        gateway_state_set_relay(state == 1);
        return;
    }

    // 3. Bản tin dữ liệu cảm biến DHT11 từ H2
    // Ví dụ: "DATA:SENSOR:TEMP=29.5,HUM=65.0"
    if (strncmp(line, "DATA:SENSOR:", 12) == 0) {
        float temp = 0.0f;
        float hum = 0.0f;
        if (sscanf(line, "DATA:SENSOR:TEMP=%f,HUM=%f", &temp, &hum) == 2) {
            gateway_state_set_sensor(temp, hum);
        } else {
            ESP_LOGW(TAG, "Sai định dạng cảm biến: %s", line);
        }
        return;
    }

    // 4. Báo lỗi Timeout từ Thread Mesh
    if (strstr(line, "ERR:") && strstr(line, "TIMEOUT")) {
        ESP_LOGW(TAG, ">>> [THREAD TIMEOUT]: Node không phản hồi (mất sóng/mất nguồn)! <<<");
        return;
    }
}

static void mesh_uart_rx_task(void *arg) {
    ESP_LOGI(TAG, "Mesh UART RX Task started. Listening for ACK/DATA from ESP32-H2...");
    uint8_t rx_buf[256];
    char line_buf[256];
    size_t line_len = 0;

    while (1) {
        int len = uart_read_bytes(MESH_UART_NUM, rx_buf, sizeof(rx_buf) - 1, pdMS_TO_TICKS(100));
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = (char)rx_buf[i];
                if (c == '\n' || c == '\r') {
                    if (line_len > 0) {
                        line_buf[line_len] = '\0';
                        parse_mesh_rx_line(line_buf);
                        line_len = 0;
                    }
                } else if ((unsigned char)c >= 32 && (unsigned char)c <= 126) {
                    // Chỉ gom các ký tự ASCII hiển thị được, loại bỏ hoàn toàn nhiễu nhị phân
                    if (line_len < sizeof(line_buf) - 1) {
                        line_buf[line_len++] = c;
                    } else {
                        // Tràn dòng, reset
                        line_len = 0;
                    }
                }
            }
        }
    }
}
#endif

void mesh_controller_init(void) {
    ESP_LOGI(TAG, "Initializing Mesh Controller (UART1 to ESP32-H2 Thread Co-processor)");

    // 1. Khởi tạo Gateway State Manager trung tâm
    gateway_state_manager_init();

    // 2. Cấu hình UART1
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

    ESP_ERROR_CHECK(uart_driver_install(MESH_UART_NUM, MESH_UART_BUF_SIZE * 2, MESH_UART_BUF_SIZE * 2, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(MESH_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(MESH_UART_NUM, MESH_UART_TX_PIN, MESH_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Bật Internal Pull-Up trên chân RX (GPIO 14) để luôn giữ mức 3.3V (UART Idle),
    // chống sóng nhiễu điện từ kích hoạt Start Bit giả mạo khi chân đang để lơ lửng
    gpio_set_pull_mode(MESH_UART_RX_PIN, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "UART1 initialized on TX:%d RX:%d @ 115200bps (Internal Pull-Up enabled on RX)", MESH_UART_TX_PIN, MESH_UART_RX_PIN);

#if MESH_ENABLE_RX_TASK
    // 3. Tạo Task đón bản tin ACK / DATA từ ESP32-H2
    if (s_rx_task_handle == NULL) {
        xTaskCreate(mesh_uart_rx_task, "mesh_rx_task", 4096, NULL, 5, &s_rx_task_handle);
    }
#else
    ESP_LOGI(TAG, "Mesh UART RX Task tạm thời TẮT (MESH_ENABLE_RX_TASK == 0). Bật lại trong mesh_controller.h khi cắm H2.");
#endif
}

void mesh_controller_send_command(const char *cmd_str) {
    if (cmd_str == NULL) return;

    char clean_cmd[128];
    const char *src = cmd_str;

    // Loại bỏ dấu ngoặc nhọn nếu có: <CMD:...> -> CMD:...
    if (src[0] == '<') src++;
    size_t len = strlen(src);
    if (len > 0 && src[len - 1] == '>') {
        len--;
    }
    if (len >= sizeof(clean_cmd)) len = sizeof(clean_cmd) - 1;
    memcpy(clean_cmd, src, len);
    clean_cmd[len] = '\0';

    // Map các alias cũ sang giao thức chuẩn mới:
    // LIGHT:ON -> DIMMER:ON, LIGHT:OFF -> DIMMER:OFF, SWITCH:ON -> RELAY:ON, SWITCH:OFF -> RELAY:OFF
    char final_cmd[128];
    if (strcmp(clean_cmd, "CMD:LIGHT:ON") == 0) {
        gateway_device_state_t st;
        gateway_state_get(&st);
        uint8_t target_bright = st.dimmer_last_brightness > 0 ? st.dimmer_last_brightness : 80;
        snprintf(final_cmd, sizeof(final_cmd), "CMD:DIMMER:SET:%u", target_bright);
    } else if (strcmp(clean_cmd, "CMD:LIGHT:OFF") == 0) {
        snprintf(final_cmd, sizeof(final_cmd), "CMD:DIMMER:OFF");
    } else if (strcmp(clean_cmd, "CMD:SWITCH:ON") == 0) {
        snprintf(final_cmd, sizeof(final_cmd), "CMD:RELAY:ON");
    } else if (strcmp(clean_cmd, "CMD:SWITCH:OFF") == 0) {
        snprintf(final_cmd, sizeof(final_cmd), "CMD:RELAY:OFF");
    } else {
        snprintf(final_cmd, sizeof(final_cmd), "%s", clean_cmd);
    }

    char buffer[160];
    snprintf(buffer, sizeof(buffer), "%s\n", final_cmd);
    uart_write_bytes(MESH_UART_NUM, buffer, strlen(buffer));
    ESP_LOGI(TAG, "Sent to Thread Mesh (H2): %s", final_cmd);
}

void mesh_controller_set_dimmer(bool on, uint8_t brightness) {
    if (on) {
        if (brightness == 0) {
            gateway_device_state_t st;
            gateway_state_get(&st);
            brightness = st.dimmer_last_brightness > 0 ? st.dimmer_last_brightness : 80;
        }
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "CMD:DIMMER:SET:%u", brightness);
        mesh_controller_send_command(cmd);
    } else {
        mesh_controller_send_command("CMD:DIMMER:OFF");
    }
}

void mesh_controller_set_relay(bool on) {
    if (on) {
        mesh_controller_send_command("CMD:RELAY:ON");
    } else {
        mesh_controller_send_command("CMD:RELAY:OFF");
    }
}
