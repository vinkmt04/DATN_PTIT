#include "ui_voice_overlay.h"
#include "ui_theme.h"
#include "ui_display.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "ui_voice_overlay";

#define NUM_WAVE_BARS 7

static lv_obj_t *s_overlay_container = NULL;
static lv_obj_t *s_border_glow = NULL;
static lv_obj_t *s_status_pill = NULL;
static lv_obj_t *s_lbl_status = NULL;
static lv_obj_t *s_wave_bars[NUM_WAVE_BARS];
static lv_timer_t *s_anim_timer = NULL;

static uint8_t s_color_idx = 0;
static uint16_t s_target_rms = 0;

static void dismiss_touch_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Touch UI: Người dùng chạm dải đáy -> Huỷ tương tác giọng nói!");
    ui_display_trigger_abort();
    ui_voice_overlay_hide();
}

static void anim_timer_cb(lv_timer_t *timer) {
    if (!ui_voice_overlay_is_visible()) return;

    // 1. Hiệu ứng viền Siri xoay màu chuyển động (Siri Glowing Border)
    const lv_color_t siri_colors[] = {
        UI_COLOR_SIRI_CYAN,
        UI_COLOR_SIRI_PURPLE,
        UI_COLOR_SIRI_INDIGO,
        UI_COLOR_SIRI_ORANGE
    };
    s_color_idx = (s_color_idx + 1) % 4;
    lv_obj_set_style_border_color(s_border_glow, siri_colors[s_color_idx], 0);

    // 2. Cập nhật chiều cao các thanh sóng âm thanh (Audio Waveform)
    // RMS thực tế: 10 .. 3000
    uint8_t base_h = 6;
    uint8_t dynamic_h = (s_target_rms > 300) ? 26 : (uint8_t)(s_target_rms / 12);
    if (dynamic_h > 36) dynamic_h = 36;

    for (int i = 0; i < NUM_WAVE_BARS; i++) {
        // Tạo hình chuông (Gaussian bell curve) cho cụm sóng âm
        int dist = abs(i - (NUM_WAVE_BARS / 2));
        int bar_h = base_h + (dynamic_h / (dist + 1));
        if (bar_h < 4) bar_h = 4;
        lv_obj_set_height(s_wave_bars[i], bar_h);
    }

    // Tiêu giảm dần target RMS để sóng co giãn tự nhiên
    if (s_target_rms > 40) {
        s_target_rms -= (s_target_rms / 3);
    } else {
        s_target_rms = 0;
    }
}

void ui_voice_overlay_init(void) {
    if (s_overlay_container) return;

    // Lớp phủ trên cùng (Toàn màn hình 320x240) với nền mờ đen phủ bóng (85% Dark Dim)
    s_overlay_container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay_container, UI_LCD_H_RES, UI_LCD_V_RES);
    lv_obj_align(s_overlay_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay_container, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay_container, LV_OPA_80, 0); // 85% Dark Dim chống rối mắt
    lv_obj_set_style_border_width(s_overlay_container, 0, 0);
    lv_obj_set_style_pad_all(s_overlay_container, 0, 0);
    lv_obj_clear_flag(s_overlay_container, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Khung viền sáng Apple Siri (Border Glow)
    s_border_glow = lv_obj_create(s_overlay_container);
    lv_obj_set_size(s_border_glow, UI_LCD_H_RES, UI_LCD_V_RES);
    lv_obj_center(s_border_glow);
    lv_obj_set_style_bg_opa(s_border_glow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_border_glow, UI_COLOR_SIRI_CYAN, 0);
    lv_obj_set_style_border_width(s_border_glow, 4, 0);
    lv_obj_set_style_radius(s_border_glow, 10, 0);
    lv_obj_clear_flag(s_border_glow, LV_OBJ_FLAG_SCROLLABLE);

    // 2. Hộp thoại trạng thái dạng Pill (Floating Status Pill)
    s_status_pill = lv_obj_create(s_overlay_container);
    lv_obj_set_size(s_status_pill, 240, 36);
    lv_obj_align(s_status_pill, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(s_status_pill, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_status_pill, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_status_pill, 1, 0);
    lv_obj_set_style_radius(s_status_pill, UI_RADIUS_PILL, 0);
    lv_obj_set_style_pad_all(s_status_pill, 0, 0);
    lv_obj_clear_flag(s_status_pill, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_status = lv_label_create(s_status_pill);
    lv_obj_center(s_lbl_status);
    lv_obj_set_style_text_font(s_lbl_status, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_status, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_status, "Đang lắng nghe...");

    // 3. Cụm thanh sóng âm (Audio Waveform Bars) ở đáy
    lv_obj_t *wave_container = lv_obj_create(s_overlay_container);
    lv_obj_set_size(wave_container, 140, 44);
    lv_obj_align(wave_container, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_opa(wave_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wave_container, 0, 0);
    lv_obj_set_style_pad_all(wave_container, 0, 0);
    lv_obj_clear_flag(wave_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(wave_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wave_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < NUM_WAVE_BARS; i++) {
        s_wave_bars[i] = lv_obj_create(wave_container);
        lv_obj_set_size(s_wave_bars[i], 6, 8);
        lv_obj_set_style_bg_color(s_wave_bars[i], UI_COLOR_SIRI_CYAN, 0);
        lv_obj_set_style_radius(s_wave_bars[i], 3, 0);
        lv_obj_set_style_border_width(s_wave_bars[i], 0, 0);
        lv_obj_clear_flag(s_wave_bars[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    // 4. Dải cảm ứng chạm đáy để huỷ lượt (Bottom Dismiss Touch Zone: Y=180..240)
    lv_obj_t *dismiss_zone = lv_obj_create(s_overlay_container);
    lv_obj_set_size(dismiss_zone, UI_LCD_H_RES, 60);
    lv_obj_align(dismiss_zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(dismiss_zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dismiss_zone, 0, 0);
    lv_obj_add_flag(dismiss_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dismiss_zone, dismiss_touch_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_dismiss = lv_label_create(dismiss_zone);
    lv_obj_align(lbl_dismiss, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_text_font(lbl_dismiss, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_dismiss, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(lbl_dismiss, "Chạm để hủy");

    // Mặc định ban đầu ẩn lớp phủ
    lv_obj_add_flag(s_overlay_container, LV_OBJ_FLAG_HIDDEN);

    // Tạo Timer animation quét viền và sóng âm (chu kỳ 50ms = 20 FPS)
    s_anim_timer = lv_timer_create(anim_timer_cb, 50, NULL);
    lv_timer_pause(s_anim_timer);
}

void ui_voice_overlay_show(void) {
    if (!s_overlay_container) return;
    lv_obj_clear_flag(s_overlay_container, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_lbl_status, "Đang lắng nghe 'Hey Cherry'...");
    lv_obj_set_style_text_color(s_lbl_status, UI_COLOR_TEXT_MAIN, 0);
    if (s_anim_timer) {
        lv_timer_resume(s_anim_timer);
    }
}

void ui_voice_overlay_hide(void) {
    if (!s_overlay_container) return;
    lv_obj_add_flag(s_overlay_container, LV_OBJ_FLAG_HIDDEN);
    if (s_anim_timer) {
        lv_timer_pause(s_anim_timer);
    }
}

bool ui_voice_overlay_is_visible(void) {
    if (!s_overlay_container) return false;
    return !lv_obj_has_flag(s_overlay_container, LV_OBJ_FLAG_HIDDEN);
}

void ui_voice_overlay_set_rms(uint16_t rms) {
    s_target_rms = rms;
}

void ui_voice_overlay_set_status(const char *status_text, lv_color_t color) {
    if (!s_lbl_status || !status_text) return;
    lv_label_set_text(s_lbl_status, status_text);
    lv_obj_set_style_text_color(s_lbl_status, color, 0);
}
