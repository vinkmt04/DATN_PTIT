#include "xpt2046_driver.h"
#include "ui_display.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include <stdlib.h>

static const char *TAG = "xpt2046_driver";

static spi_device_handle_t s_spi_touch = NULL;

// Thông số hiệu chuẩn 4 góc đo trực tiếp từ phần cứng thực tế của người dùng:
// Trái-Trên (TL: X=0,   Y=0  ): Raw_X ~ 3600, Raw_Y ~ 3500
// Phải-Trên (TR: X=320, Y=0  ): Raw_X ~ 480,  Raw_Y ~ 3300
// Trái-Dưới (BL: X=0,   Y=240): Raw_X ~ 3400, Raw_Y ~ 600
// Phải-Dưới (BR: X=320, Y=240): Raw_X ~ 510,  Raw_Y ~ 640
#define CALIB_TL_X   3600
#define CALIB_TL_Y   3500
#define CALIB_TR_X   480
#define CALIB_TR_Y   3300
#define CALIB_BL_X   3400
#define CALIB_BL_Y   600
#define CALIB_BR_X   510
#define CALIB_BR_Y   640

#define CMD_READ_X      0x90 // Lệnh đọc kênh X (12-bit)
#define CMD_READ_Y      0xD0 // Lệnh đọc kênh Y (12-bit)

esp_err_t xpt2046_driver_init(void) {
    ESP_LOGI(TAG, "Initializing SPI3 bus for XPT2046 Touch (CLK:%d, DIN:%d, DO:%d, CS:%d, IRQ:%d)",
             TOUCH_PIN_CLK, TOUCH_PIN_DIN, TOUCH_PIN_DO, TOUCH_PIN_CS, TOUCH_PIN_IRQ);

    // 1. Cấu hình chân ngắt IRQ (Active LOW, Pull-Up)
    gpio_config_t irq_conf = {
        .pin_bit_mask = (1ULL << TOUCH_PIN_IRQ),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&irq_conf);

    // 2. Cấu hình SPI3 Bus độc lập (HSPI)
    spi_bus_config_t buscfg = {
        .sclk_io_num = TOUCH_PIN_CLK,
        .mosi_io_num = TOUCH_PIN_DIN,
        .miso_io_num = TOUCH_PIN_DO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI3 bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Thêm thiết bị XPT2046 vào SPI3
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000, // 2 MHz
        .mode = 0,
        .spics_io_num = TOUCH_PIN_CS,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &s_spi_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add XPT2046 device to SPI3: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "XPT2046 Touch Driver initialized successfully on SPI3 @ 2MHz");
    return ESP_OK;
}

static uint16_t xpt2046_spi_cmd(uint8_t cmd) {
    if (!s_spi_touch) return 0;

    uint8_t tx_data[3] = {cmd, 0x00, 0x00};
    uint8_t rx_data[3] = {0};

    spi_transaction_t t = {
        .length = 24, // 3 bytes * 8
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    if (spi_device_transmit(s_spi_touch, &t) != ESP_OK) {
        return 0;
    }

    // Dữ liệu 12-bit ADC trả về ở byte 1 và byte 2
    uint16_t raw_val = ((uint16_t)rx_data[1] << 8) | rx_data[2];
    raw_val >>= 3; // Shift chuẩn 12-bit
    return raw_val & 0x0FFF;
}

bool xpt2046_read_raw(uint16_t *raw_x, uint16_t *raw_y) {
    // Nếu chân IRQ ở mức HIGH (1), tức là không có chạm
    if (gpio_get_level(TOUCH_PIN_IRQ) != 0) {
        return false;
    }

    // Đọc mẫu 3 lần để áp dụng bộ lọc trung vị (Median Filter)
    uint16_t samples_x[3];
    uint16_t samples_y[3];

    for (int i = 0; i < 3; i++) {
        samples_x[i] = xpt2046_spi_cmd(CMD_READ_X);
        samples_y[i] = xpt2046_spi_cmd(CMD_READ_Y);
    }

    // Sắp xếp tìm giá trị trung vị
    for (int i = 0; i < 2; i++) {
        for (int j = i + 1; j < 3; j++) {
            if (samples_x[i] > samples_x[j]) {
                uint16_t tmp = samples_x[i];
                samples_x[i] = samples_x[j];
                samples_x[j] = tmp;
            }
            if (samples_y[i] > samples_y[j]) {
                uint16_t tmp = samples_y[i];
                samples_y[i] = samples_y[j];
                samples_y[j] = tmp;
            }
        }
    }

    uint16_t med_x = samples_x[1];
    uint16_t med_y = samples_y[1];

    if (med_x < 100 || med_y < 100) {
        return false;
    }

    if (raw_x) *raw_x = med_x;
    if (raw_y) *raw_y = med_y;
    return true;
}

bool xpt2046_read(int16_t *x, int16_t *y) {
    uint16_t raw_x = 0, raw_y = 0;
    if (!xpt2046_read_raw(&raw_x, &raw_y)) {
        return false;
    }

#if UI_TOUCH_CALIBRATION_MODE
    ESP_LOGI(TAG, "Touch Raw ADC -> X: %u, Y: %u", raw_x, raw_y);
#endif

    // Ánh xạ toạ độ Bilinear 4 góc (Bilinear Interpolation):
    // Khắc phục triệt để độ lệch nghiêng của tấm kính điện trở (đặc biệt ở góc trên bên phải
    // nơi Y_max chỉ đạt ~3300 thay vì 3500, tránh làm thụt toạ độ Y xuống 17px khiến lệch nút '+')

    // 1. Tính toán biên trên (y_top) và biên dưới (y_bot) tại cột raw_x
    int32_t y_top = CALIB_TR_Y + ((int32_t)(raw_x - CALIB_TR_X) * (CALIB_TL_Y - CALIB_TR_Y)) / (CALIB_TL_X - CALIB_TR_X);
    int32_t y_bot = CALIB_BR_Y - ((int32_t)(raw_x - CALIB_BR_X) * (CALIB_BR_Y - CALIB_BL_Y)) / (CALIB_BL_X - CALIB_BR_X);
    int32_t y_span = y_top - y_bot;
    if (y_span <= 0) y_span = 1;
    int32_t calc_y = ((int32_t)(y_top - raw_y) * UI_LCD_V_RES) / y_span;

    // 2. Tính toán biên trái (x_left) và biên phải (x_right) tại hàng raw_y
    int32_t x_left = CALIB_BL_X + ((int32_t)(raw_y - CALIB_BL_Y) * (CALIB_TL_X - CALIB_BL_X)) / (CALIB_TL_Y - CALIB_BL_Y);
    int32_t x_right = CALIB_BR_X - ((int32_t)(raw_y - CALIB_BR_Y) * (CALIB_BR_X - CALIB_TR_X)) / (CALIB_TR_Y - CALIB_BR_Y);
    int32_t x_span = x_left - x_right;
    if (x_span <= 0) x_span = 1;
    int32_t calc_x = ((int32_t)(x_left - raw_x) * UI_LCD_H_RES) / x_span;

    // 3. Giới hạn biên màn hình LCD
    if (calc_x < 0) calc_x = 0;
    if (calc_x >= UI_LCD_H_RES) calc_x = UI_LCD_H_RES - 1;
    if (calc_y < 0) calc_y = 0;
    if (calc_y >= UI_LCD_V_RES) calc_y = UI_LCD_V_RES - 1;

    if (x) *x = (int16_t)calc_x;
    if (y) *y = (int16_t)calc_y;
    return true;
}
