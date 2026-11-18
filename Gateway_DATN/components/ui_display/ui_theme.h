#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// BẢNG MÀU CHUẨN APPLE CLEAN MODERN LIGHT THEME (iOS Style)
// =============================================================================
#define UI_COLOR_BG             lv_color_hex(0xF2F2F7)  // Nền xám nhạt iOS
#define UI_COLOR_CARD_BG        lv_color_hex(0xFFFFFF)  // Thẻ card trắng tinh
#define UI_COLOR_CARD_BORDER    lv_color_hex(0xE5E5EA)  // Viền mỏng 1px
#define UI_COLOR_TEXT_MAIN      lv_color_hex(0x1C1C1E)  // Chữ chính đen xám
#define UI_COLOR_TEXT_MUTED     lv_color_hex(0x8E8E93)  // Chữ phụ xám mờ
#define UI_COLOR_ACCENT_BLUE    lv_color_hex(0x007AFF)  // Xanh dương Apple
#define UI_COLOR_ACCENT_GREEN   lv_color_hex(0x34C759)  // Xanh lá kích hoạt
#define UI_COLOR_ACCENT_ORANGE  lv_color_hex(0xFF9500)  // Cam cảnh báo / Dimmer
#define UI_COLOR_ACCENT_RED     lv_color_hex(0xFF3B30)  // Đỏ lỗi / Mất kết nối
#define UI_COLOR_CARD_DISABLED  lv_color_hex(0xFAFAFC)  // Màu card khi node offline

// Màu dải viền Siri Gradient
#define UI_COLOR_SIRI_CYAN      lv_color_hex(0x00C7BE)
#define UI_COLOR_SIRI_PURPLE    lv_color_hex(0xAF52DE)
#define UI_COLOR_SIRI_INDIGO    lv_color_hex(0x5856D6)
#define UI_COLOR_SIRI_ORANGE    lv_color_hex(0xFF9500)

// Bảng màu Apple Standby Dark Theme (Cho Màn hình Chờ ban đêm)
#define UI_COLOR_STANDBY_BG     lv_color_hex(0x0B0E14)  // Nền đen sâu huyền ảo
#define UI_COLOR_STANDBY_CARD   lv_color_hex(0x161B26)  // Hộp card nền tối
#define UI_COLOR_STANDBY_BORDER lv_color_hex(0x283044)  // Viền mỏng tối 1px
#define UI_COLOR_STANDBY_TEXT   lv_color_hex(0xF8FAFC)  // Chữ phát sáng trắng ngọc
#define UI_COLOR_STANDBY_MUTED  lv_color_hex(0x94A3B8)  // Chữ phụ xám khói
#define UI_COLOR_WEATHER_SUN    lv_color_hex(0xF59E0B)  // Vàng cam mặt trời ban ngày
#define UI_COLOR_WEATHER_MOON   lv_color_hex(0x38BDF8)  // Xanh trăng sao ban đêm
#define UI_COLOR_LIGHT_WARM     lv_color_hex(0xFFB800)  // Vàng ấm đèn chiếu sáng bật

// Thông số bo góc chuẩn Apple
#define UI_RADIUS_CARD          12
#define UI_RADIUS_PILL          16
#define UI_RADIUS_BUTTON        8

// Font Tiếng Việt có dấu đầy đủ (Kích thước 14, Bpp 4 Antialiased)
extern const lv_font_t ui_font_vn_14;

#ifdef __cplusplus
}
#endif
