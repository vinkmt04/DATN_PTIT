#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo lớp phủ Siri Voice Overlay
void ui_voice_overlay_init(void);

// Hiển thị lớp phủ và bắt đầu hiệu ứng viền Siri phát sáng
void ui_voice_overlay_show(void);

// Ẩn lớp phủ Siri
void ui_voice_overlay_hide(void);

// Kiểm tra lớp phủ Siri có đang hiển thị hay không
bool ui_voice_overlay_is_visible(void);

// Cập nhật mức năng lượng âm thanh (Mic RMS hoặc Speaker PCM) cho thanh sóng âm visualizer
void ui_voice_overlay_set_rms(uint16_t rms);

// Cập nhật dòng trạng thái (VD: "Đang nghe...", "Đã bật đèn • 45ms", "Gemini Live...")
void ui_voice_overlay_set_status(const char *status_text, lv_color_t color);

#ifdef __cplusplus
}
#endif
