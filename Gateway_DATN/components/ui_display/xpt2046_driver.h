#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo SPI3 độc lập và cấu hình chân cảm ứng XPT2046
esp_err_t xpt2046_driver_init(void);

// Đọc toạ độ cảm ứng đã qua lọc nhiễu Median và ánh xạ toạ độ màn hình 320x240
// Trả về true nếu đang chạm (pressed), false nếu không chạm
bool xpt2046_read(int16_t *x, int16_t *y);

// Lấy toạ độ ADC thô (dùng cho chế độ hiệu chuẩn Touch Calibration)
bool xpt2046_read_raw(uint16_t *raw_x, uint16_t *raw_y);

#ifdef __cplusplus
}
#endif
