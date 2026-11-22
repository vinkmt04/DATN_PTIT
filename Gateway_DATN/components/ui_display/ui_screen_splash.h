#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khởi tạo Màn hình Splash Screen với thanh tiến trình đồ họa 0 - 100%
 * @return lv_obj_t* Con trỏ tới màn hình Splash
 */
lv_obj_t* ui_screen_splash_init(void);

/**
 * @brief Cập nhật phần trăm tiến trình và chuỗi văn bản trạng thái
 * @param percent Phần trăm từ 0 đến 100
 * @param status_msg Chuỗi văn bản mô tả giai đoạn đang nạp
 */
void ui_screen_splash_set_progress(uint8_t percent, const char *status_msg);

/**
 * @brief Lấy đối tượng màn hình Splash hiện tại
 */
lv_obj_t* ui_screen_splash_get_screen(void);

#ifdef __cplusplus
}
#endif
