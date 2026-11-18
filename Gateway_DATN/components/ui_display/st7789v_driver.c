#include "st7789v_driver.h"
#include "ui_display.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_st7789.h"

static const char *TAG = "st7789v_driver";

#define LCD_LEDC_TIMER       LEDC_TIMER_0
#define LCD_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LCD_LEDC_CHANNEL     LEDC_CHANNEL_0
#define LCD_LEDC_FREQ_HZ     5000

esp_err_t st7789v_driver_init(esp_lcd_panel_handle_t *out_panel, esp_lcd_panel_io_handle_t *out_io) {
    ESP_LOGI(TAG, "Initializing SPI2 bus for ST7789V (SCLK:%d, MOSI:%d, CS:%d, DC:%d, RST:%d, BL:%d)",
             LCD_PIN_SCLK, LCD_PIN_MOSI, LCD_PIN_CS, LCD_PIN_DC, LCD_PIN_RST, LCD_PIN_BL);

    // 1. Cấu hình SPI2 Bus cho màn hình
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = UI_LCD_H_RES * 40 * sizeof(uint16_t),
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI2 bus: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2. Cấu hình Panel IO SPI
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = 40 * 1000 * 1000, // 40MHz
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        return ret;
    }

    // 3. Cấu hình Panel ST7789
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Khởi tạo phần cứng màn hình
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);

    // Tắt invert color (để hiển thị đúng màu RGB tự nhiên chuẩn Apple)
    esp_lcd_panel_invert_color(panel_handle, false);

    // Chuyển sang chế độ Ngang (Landscape 320x240)
    esp_lcd_panel_swap_xy(panel_handle, true);
    esp_lcd_panel_mirror(panel_handle, false, true);

    // Bật hiển thị panel
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // 4. Cấu hình PWM đèn nền (LEDC) trên GPIO 3
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LCD_LEDC_MODE,
        .timer_num        = LCD_LEDC_TIMER,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .freq_hz          = LCD_LEDC_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LCD_LEDC_MODE,
        .channel        = LCD_LEDC_CHANNEL,
        .timer_sel      = LCD_LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LCD_PIN_BL,
        .duty           = 255, // Mặc định khởi động 100% độ sáng
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);

    if (out_panel) *out_panel = panel_handle;
    if (out_io) *out_io = io_handle;

    ESP_LOGI(TAG, "ST7789V Driver initialized successfully (320x240 Landscape @ 40MHz DMA)");
    return ESP_OK;
}

void st7789v_set_backlight(uint8_t brightness_percent) {
    if (brightness_percent > 100) brightness_percent = 100;
    uint32_t duty = (brightness_percent * 255) / 100;
    ledc_set_duty(LCD_LEDC_MODE, LCD_LEDC_CHANNEL, duty);
    ledc_update_duty(LCD_LEDC_MODE, LCD_LEDC_CHANNEL);
}
