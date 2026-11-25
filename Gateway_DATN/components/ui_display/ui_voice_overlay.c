#include "ui_voice_overlay.h"
#include "ui_theme.h"
#include "ui_display.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ui_voice_overlay";

#define NUM_WAVE_BARS 9

static lv_obj_t *s_overlay_container = NULL;
static lv_obj_t *s_lbl_status = NULL;
static lv_obj_t *s_wave_bars[NUM_WAVE_BARS];
static lv_obj_t *s_btn_finish = NULL;
static lv_obj_t *s_btn_abort = NULL;
static lv_timer_t *s_anim_timer = NULL;

static uint16_t s_target_rms = 0;

// Dải chiều cao sóng âm tự nhiên bất đối xứng (phỏng theo track ghi âm giọng nói Voice Memos thực tế)
static const uint8_t s_base_wave_h[NUM_WAVE_BARS] = { 12, 28, 16, 42, 24, 38, 18, 32, 14 };

static void finish_touch_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Touch UI: Người dùng chạm 'Gửi ngay' -> Chốt câu tức thì!");
    ui_display_trigger_finish();
}

static void abort_touch_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Touch UI: Người dùng chạm 'Hủy' -> Hủy tương tác giọng nói!");
    ui_display_trigger_abort();
    ui_voice_overlay_hide();
}

static void anim_timer_cb(lv_timer_t *timer) {
    if (!ui_voice_overlay_is_visible()) return;

    // Cập nhật nhấp nhô theo năng lượng âm thanh khi AI nói trả lời
    for (int i = 0; i < NUM_WAVE_BARS; i++) {
        int boost = 0;
        if (s_target_rms > 50) {
            int dyn = (s_target_rms > 400) ? 14 : (int)(s_target_rms / 30);
            boost = (i % 2 == 0) ? (dyn * (i + 2)) / 8 : -(dyn * (8 - i)) / 10;
        }
        int bar_h = s_base_wave_h[i] + boost;
        if (bar_h < 8) bar_h = 8;
        if (bar_h > 48) bar_h = 48;
        lv_obj_set_height(s_wave_bars[i], bar_h);
    }

    // Tiêu giảm dần target RMS để sóng co giãn tự nhiên
    if (s_target_rms > 30) {
        s_target_rms -= (s_target_rms / 3);
    } else {
        s_target_rms = 0;
    }
}

void ui_voice_overlay_init(void) {
    if (s_overlay_container) return;

    // Lớp phủ trên cùng (Toàn màn hình 320x240) vuông góc 100%, phủ kín 4 góc không khung viền
    s_overlay_container = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay_container, UI_LCD_H_RES, UI_LCD_V_RES);
    lv_obj_align(s_overlay_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay_container, UI_COLOR_STANDBY_BG, 0); // 0x0B0E14 Nền đen sâu
    lv_obj_set_style_bg_opa(s_overlay_container, LV_OPA_COVER, 0);         // 100% Opaque -> Render tức thì
    lv_obj_set_style_radius(s_overlay_container, 0, 0);                    // Vuông góc 4 cạnh (bỏ bo góc mặc định của LVGL card theme)
    lv_obj_set_style_border_width(s_overlay_container, 0, 0);              // Bỏ khung viền bao quanh
    lv_obj_set_style_outline_width(s_overlay_container, 0, 0);
    lv_obj_set_style_shadow_width(s_overlay_container, 0, 0);
    lv_obj_set_style_pad_all(s_overlay_container, 0, 0);
    lv_obj_clear_flag(s_overlay_container, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Cụm thanh sóng âm (Audio Waveform Bars) được đưa lên cao hơn (Y = -42) để tạo khoảng cách thoáng đãng
    lv_obj_t *wave_container = lv_obj_create(s_overlay_container);
    lv_obj_set_size(wave_container, 160, 54);
    lv_obj_align(wave_container, LV_ALIGN_CENTER, 0, -42);
    lv_obj_set_style_bg_opa(wave_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wave_container, 0, 0);
    lv_obj_set_style_pad_all(wave_container, 0, 0);
    lv_obj_clear_flag(wave_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(wave_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wave_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wave_container, 6, 0);

    const lv_color_t bar_colors[NUM_WAVE_BARS] = {
        lv_color_hex(0x0284C7), // Xanh dương
        lv_color_hex(0x00B4D8), // Cyan sáng
        UI_COLOR_SIRI_CYAN,     // Siri Cyan (0x00C7BE)
        lv_color_hex(0xE0F7FA), // Đỉnh 1: Trắng ngọc phát quang
        UI_COLOR_SIRI_CYAN,     // Siri Cyan
        lv_color_hex(0xE0F7FA), // Đỉnh 2: Trắng ngọc phát quang
        UI_COLOR_SIRI_CYAN,     // Siri Cyan
        lv_color_hex(0x00B4D8), // Cyan sáng
        lv_color_hex(0x0284C7)  // Xanh dương
    };

    for (int i = 0; i < NUM_WAVE_BARS; i++) {
        s_wave_bars[i] = lv_obj_create(wave_container);
        lv_obj_set_size(s_wave_bars[i], 7, s_base_wave_h[i]); // Dạng cột sóng âm thanh máy ghi âm
        lv_obj_set_style_bg_color(s_wave_bars[i], bar_colors[i], 0);
        lv_obj_set_style_radius(s_wave_bars[i], 3, 0);
        lv_obj_set_style_border_width(s_wave_bars[i], 0, 0);
        lv_obj_clear_flag(s_wave_bars[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    // 2. Dòng trạng thái nổi tự nhiên, KHÔNG dùng khung viền hộp bao quanh
    s_lbl_status = lv_label_create(s_overlay_container);
    lv_obj_align(s_lbl_status, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_text_font(s_lbl_status, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_status, UI_COLOR_STANDBY_TEXT, 0);
    lv_label_set_text(s_lbl_status, "Đang lắng nghe...");

    // 3. Hộp chứa 2 nút cảm ứng đáy: [Gửi ngay] (xanh dương) & [Hủy] (xám)
    lv_obj_t *bottom_btn_box = lv_obj_create(s_overlay_container);
    lv_obj_set_size(bottom_btn_box, 240, 42);
    lv_obj_align(bottom_btn_box, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(bottom_btn_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_btn_box, 0, 0);
    lv_obj_set_style_pad_all(bottom_btn_box, 0, 0);
    lv_obj_clear_flag(bottom_btn_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bottom_btn_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom_btn_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bottom_btn_box, 14, 0);

    // Nút "Gửi ngay" - Chữ màu xanh dương, hoàn toàn không khung viền
    s_btn_finish = lv_btn_create(bottom_btn_box);
    lv_obj_set_size(s_btn_finish, 110, 36);
    lv_obj_set_style_bg_opa(s_btn_finish, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btn_finish, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_finish, 0, 0);
    lv_obj_set_style_outline_width(s_btn_finish, 0, 0);
    lv_obj_set_style_pad_all(s_btn_finish, 0, 0);
    lv_obj_set_style_bg_opa(s_btn_finish, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_btn_finish, lv_color_hex(0x38BDF8), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_btn_finish, finish_touch_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_finish = lv_label_create(s_btn_finish);
    lv_obj_center(lbl_finish);
    lv_obj_set_style_text_font(lbl_finish, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_finish, lv_color_hex(0x38BDF8), 0); // Chữ màu xanh dương Apple
    lv_label_set_text(lbl_finish, "Gửi ngay");

    // Nút "Hủy" - Chữ màu xám xám, hoàn toàn không khung viền
    s_btn_abort = lv_btn_create(bottom_btn_box);
    lv_obj_set_size(s_btn_abort, 90, 36);
    lv_obj_set_style_bg_opa(s_btn_abort, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_btn_abort, 0, 0);
    lv_obj_set_style_shadow_width(s_btn_abort, 0, 0);
    lv_obj_set_style_outline_width(s_btn_abort, 0, 0);
    lv_obj_set_style_pad_all(s_btn_abort, 0, 0);
    lv_obj_set_style_bg_opa(s_btn_abort, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_btn_abort, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_btn_abort, abort_touch_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_abort = lv_label_create(s_btn_abort);
    lv_obj_center(lbl_abort);
    lv_obj_set_style_text_font(lbl_abort, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_abort, lv_color_hex(0x94A3B8), 0); // Chữ màu xám xám dịu mắt
    lv_label_set_text(lbl_abort, "Hủy");

    // Mặc định ban đầu ẩn lớp phủ
    lv_obj_add_flag(s_overlay_container, LV_OBJ_FLAG_HIDDEN);

    // Tạo Timer animation cập nhật sóng âm (chu kỳ 150ms để tối ưu CPU & giảm tải SPI DMA)
    s_anim_timer = lv_timer_create(anim_timer_cb, 150, NULL);
    lv_timer_pause(s_anim_timer);
}

void ui_voice_overlay_show(void) {
    if (!s_overlay_container) return;
    lv_obj_clear_flag(s_overlay_container, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_finish) {
        lv_obj_clear_flag(s_btn_finish, LV_OBJ_FLAG_HIDDEN);
    }
    // Khôi phục các thanh sóng âm về dạng nhấp nhô trực quan chuẩn
    for (int i = 0; i < NUM_WAVE_BARS; i++) {
        if (s_wave_bars[i]) {
            lv_obj_set_height(s_wave_bars[i], s_base_wave_h[i]);
        }
    }
    lv_label_set_text(s_lbl_status, "Đang lắng nghe 'Hey Cherry'...");
    lv_obj_set_style_text_color(s_lbl_status, UI_COLOR_STANDBY_TEXT, 0);
    // Giữ giao diện tĩnh hoàn toàn (Static Overlay), KHÔNG chạy animation timer khi thu âm
    if (s_anim_timer) {
        lv_timer_pause(s_anim_timer);
    }
}

void ui_voice_overlay_resume_anim(void) {
    if (!s_overlay_container || !ui_voice_overlay_is_visible()) return;
    // Khi AI nói trả lời, ẩn nút "Gửi ngay" (vì câu đã gửi rồi), chỉ giữ nút "Hủy" nếu muốn ngắt lời
    if (s_btn_finish) {
        lv_obj_add_flag(s_btn_finish, LV_OBJ_FLAG_HIDDEN);
    }
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
