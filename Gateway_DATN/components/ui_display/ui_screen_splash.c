#include "ui_screen_splash.h"
#include "ui_theme.h"
#include "ui_display.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *s_scr_splash = NULL;
static lv_obj_t *s_bar_progress = NULL;
static lv_obj_t *s_lbl_status = NULL;
static lv_obj_t *s_lbl_percent = NULL;

lv_obj_t* ui_screen_splash_init(void) {
    if (s_scr_splash) return s_scr_splash;

    // 1. Tạo màn hình chính (Nền đen tối chuẩn Standby Deep Black)
    s_scr_splash = lv_obj_create(NULL);
    lv_obj_set_size(s_scr_splash, UI_LCD_H_RES, UI_LCD_V_RES);
    lv_obj_set_style_bg_color(s_scr_splash, UI_COLOR_STANDBY_BG, 0);
    lv_obj_set_style_bg_opa(s_scr_splash, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr_splash, 0, 0);
    lv_obj_set_style_border_width(s_scr_splash, 0, 0);
    lv_obj_clear_flag(s_scr_splash, LV_OBJ_FLAG_SCROLLABLE);

    // 2. Huy hiệu biểu tượng trung tâm (Glowing Circular Hub Badge)
    lv_obj_t *icon_badge = lv_obj_create(s_scr_splash);
    lv_obj_set_size(icon_badge, 44, 44);
    lv_obj_align(icon_badge, LV_ALIGN_CENTER, 0, -50);
    lv_obj_set_style_radius(icon_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(icon_badge, UI_COLOR_STANDBY_CARD, 0);
    lv_obj_set_style_border_color(icon_badge, UI_COLOR_SIRI_CYAN, 0);
    lv_obj_set_style_border_width(icon_badge, 2, 0);
    lv_obj_set_style_pad_all(icon_badge, 0, 0);
    lv_obj_clear_flag(icon_badge, LV_OBJ_FLAG_SCROLLABLE);

    // Nhân sáng phát quang bên trong biểu tượng
    lv_obj_t *inner_core = lv_obj_create(icon_badge);
    lv_obj_set_size(inner_core, 16, 16);
    lv_obj_center(inner_core);
    lv_obj_set_style_radius(inner_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(inner_core, UI_COLOR_SIRI_CYAN, 0);
    lv_obj_set_style_border_width(inner_core, 0, 0);
    lv_obj_clear_flag(inner_core, LV_OBJ_FLAG_SCROLLABLE);

    // 3. Tiêu đề hệ thống
    lv_obj_t *lbl_title = lv_label_create(s_scr_splash);
    lv_obj_align(lbl_title, LV_ALIGN_CENTER, 0, -14);
    lv_obj_set_style_text_font(lbl_title, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_title, UI_COLOR_STANDBY_TEXT, 0);
    lv_label_set_text(lbl_title, "SMART HOME GATEWAY");

    // 4. Nhãn phụ / Đơn vị thực hiện
    lv_obj_t *lbl_sub = lv_label_create(s_scr_splash);
    lv_obj_align(lbl_sub, LV_ALIGN_CENTER, 0, 6);
    lv_obj_set_style_text_font(lbl_sub, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_sub, UI_COLOR_STANDBY_MUTED, 0);
    lv_label_set_text(lbl_sub, "PTIT AIoT Embedded Hub");

    // 5. Thanh tiến trình đồ họa (Progress Bar Apple Style)
    s_bar_progress = lv_bar_create(s_scr_splash);
    lv_obj_set_size(s_bar_progress, 220, 8);
    lv_obj_align(s_bar_progress, LV_ALIGN_CENTER, 0, 32);
    lv_bar_set_range(s_bar_progress, 0, 100);
    lv_bar_set_value(s_bar_progress, 0, LV_ANIM_OFF);

    // Style thanh ray nền (Track)
    lv_obj_set_style_bg_color(s_bar_progress, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar_progress, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bar_progress, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar_progress, 0, LV_PART_MAIN);

    // Style thanh chạy tiến trình (Indicator)
    lv_obj_set_style_bg_color(s_bar_progress, UI_COLOR_SIRI_CYAN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar_progress, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_bar_progress, 4, LV_PART_INDICATOR);

    // 6. Nhãn trạng thái chi tiết đang nạp
    s_lbl_status = lv_label_create(s_scr_splash);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, 56);
    lv_obj_set_style_text_font(s_lbl_status, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_status, UI_COLOR_STANDBY_MUTED, 0);
    lv_label_set_text(s_lbl_status, "Đang khởi động phần cứng...");

    // 7. Nhãn hiển thị số phần trăm
    s_lbl_percent = lv_label_create(s_scr_splash);
    lv_obj_align(s_lbl_percent, LV_ALIGN_CENTER, 0, 78);
    lv_obj_set_style_text_font(s_lbl_percent, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_percent, UI_COLOR_SIRI_CYAN, 0);
    lv_label_set_text(s_lbl_percent, "0%");

    return s_scr_splash;
}

void ui_screen_splash_set_progress(uint8_t percent, const char *status_msg) {
    if (!s_scr_splash) return;

    if (percent > 100) percent = 100;

    if (s_bar_progress) {
        lv_bar_set_value(s_bar_progress, percent, LV_ANIM_ON);
    }

    if (s_lbl_percent) {
        char pct_str[16];
        snprintf(pct_str, sizeof(pct_str), "%u%%", percent);
        lv_label_set_text(s_lbl_percent, pct_str);
    }

    if (s_lbl_status && status_msg) {
        lv_label_set_text(s_lbl_status, status_msg);
    }
}

lv_obj_t* ui_screen_splash_get_screen(void) {
    return s_scr_splash;
}
