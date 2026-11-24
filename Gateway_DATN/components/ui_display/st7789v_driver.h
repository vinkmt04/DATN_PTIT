#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo SPI2 DMA, panel ST7789V và LEDC PWM đèn nền trên GPIO 3
esp_err_t st7789v_driver_init(esp_lcd_panel_handle_t *out_panel, esp_lcd_panel_io_handle_t *out_io,
                             esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done, void *user_ctx);

// Điều chỉnh độ sáng đèn nền (0 - 100%) qua PWM GPIO 3
void st7789v_set_backlight(uint8_t brightness_percent);

#ifdef __cplusplus
}
#endif
