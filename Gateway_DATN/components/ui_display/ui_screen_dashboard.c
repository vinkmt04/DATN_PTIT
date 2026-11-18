#include "ui_screen_dashboard.h"
#include "ui_theme.h"
#include "ui_display.h"
#include "mesh_controller.h"
#include "sys_core.h"
#include "esp_log.h"
#include <stdio.h>

LV_FONT_DECLARE(lv_font_montserrat_32);

static const char *TAG = "ui_dashboard";

static lv_obj_t *s_screen_dash = NULL;
static lv_obj_t *s_tabview = NULL;

// Header Widgets
static lv_obj_t *s_lbl_header_time = NULL;
static lv_obj_t *s_cont_header_wifi = NULL;
static lv_obj_t *s_icon_header_wifi = NULL;
static lv_obj_t *s_slash_header_wifi = NULL;
static lv_obj_t *s_lbl_header_wifi_dbm = NULL;
static lv_obj_t *s_lbl_header_mesh = NULL;

static const lv_point_t s_hdr_slash_pts[] = {{1, 13}, {13, 1}};

// Tab 1 (Thiết bị) Widgets - Apple Home Interactive Tiles Đối Xứng Tuyệt Đối
static lv_obj_t *s_card_relay = NULL;
static lv_obj_t *s_icon_relay_btn = NULL;
static lv_obj_t *s_icon_relay = NULL;
static lv_obj_t *s_lbl_relay_title = NULL;
static lv_obj_t *s_lbl_relay_state = NULL;

static lv_obj_t *s_card_dimmer = NULL;
static lv_obj_t *s_icon_dimmer_btn = NULL;
static lv_obj_t *s_bulb_head = NULL;
static lv_obj_t *s_bulb_base = NULL;
static lv_obj_t *s_lbl_dimmer_title = NULL;
static lv_obj_t *s_slider_dimmer = NULL;
static lv_obj_t *s_lbl_dimmer_val = NULL;

// Tab 2 (Môi trường) Widgets - DHT11 Đối xứng (Font 32px Lớn Căn Giữa)
static lv_obj_t *s_card_temp = NULL;
static lv_obj_t *s_lbl_temp_val = NULL;

static lv_obj_t *s_card_hum = NULL;
static lv_obj_t *s_lbl_hum_val = NULL;

// Tab 3 (Hệ thống) Widgets
static lv_obj_t *s_lbl_sys_ip = NULL;
static lv_obj_t *s_lbl_sys_cloud = NULL;
static lv_obj_t *s_lbl_sys_mesh = NULL;

// Trạng thái cục bộ phản hồi cảm ứng tức thì
static bool s_relay_state_local = false;
static bool s_dimmer_state_local = false;
static uint8_t s_dimmer_bri_local = 80;

// Cờ chống vòng lặp phản hồi sự kiện (loopback suppression)
static bool s_suppress_events = false;

// Cập nhật giao diện trực quan cho Relay
static void update_relay_ui(bool is_on) {
    if (!s_card_relay) return;
    if (is_on) {
        lv_obj_set_style_bg_color(s_card_relay, lv_color_hex(0xF0FDF4), 0);
        lv_obj_set_style_border_color(s_card_relay, UI_COLOR_ACCENT_GREEN, 0);
        lv_obj_set_style_border_width(s_card_relay, 2, 0);

        lv_obj_set_style_bg_color(s_icon_relay_btn, UI_COLOR_ACCENT_GREEN, 0);
        lv_obj_set_style_text_color(s_icon_relay, lv_color_white(), 0);

        lv_label_set_text(s_lbl_relay_state, "Bật");
        lv_obj_set_style_text_color(s_lbl_relay_state, UI_COLOR_ACCENT_GREEN, 0);
    } else {
        lv_obj_set_style_bg_color(s_card_relay, UI_COLOR_CARD_BG, 0);
        lv_obj_set_style_border_color(s_card_relay, UI_COLOR_CARD_BORDER, 0);
        lv_obj_set_style_border_width(s_card_relay, 1, 0);

        lv_obj_set_style_bg_color(s_icon_relay_btn, lv_color_hex(0xF2F2F7), 0);
        lv_obj_set_style_text_color(s_icon_relay, UI_COLOR_TEXT_MUTED, 0);

        lv_label_set_text(s_lbl_relay_state, "Tắt");
        lv_obj_set_style_text_color(s_lbl_relay_state, UI_COLOR_TEXT_MUTED, 0);
    }
}

// Cập nhật giao diện trực quan cho Dimmer
static void update_dimmer_ui(bool is_on, uint8_t brightness) {
    if (!s_card_dimmer) return;
    if (is_on) {
        lv_obj_set_style_bg_color(s_card_dimmer, lv_color_hex(0xFFFBEB), 0);
        lv_obj_set_style_border_color(s_card_dimmer, UI_COLOR_LIGHT_WARM, 0);
        lv_obj_set_style_border_width(s_card_dimmer, 2, 0);

        lv_obj_set_style_bg_color(s_icon_dimmer_btn, lv_color_hex(0xFEF3C7), 0);
        lv_obj_set_style_bg_color(s_bulb_head, UI_COLOR_LIGHT_WARM, 0);
        lv_obj_set_style_bg_color(s_bulb_base, lv_color_hex(0x92400E), 0);

        char dim_buf[16];
        snprintf(dim_buf, sizeof(dim_buf), "%u%%", brightness);
        lv_label_set_text(s_lbl_dimmer_val, dim_buf);
        lv_obj_set_style_text_color(s_lbl_dimmer_val, UI_COLOR_LIGHT_WARM, 0);
    } else {
        lv_obj_set_style_bg_color(s_card_dimmer, UI_COLOR_CARD_BG, 0);
        lv_obj_set_style_border_color(s_card_dimmer, UI_COLOR_CARD_BORDER, 0);
        lv_obj_set_style_border_width(s_card_dimmer, 1, 0);

        lv_obj_set_style_bg_color(s_icon_dimmer_btn, lv_color_hex(0xF2F2F7), 0);
        lv_obj_set_style_bg_color(s_bulb_head, lv_color_hex(0xD1D5DB), 0);
        lv_obj_set_style_bg_color(s_bulb_base, lv_color_hex(0x9CA3AF), 0);

        lv_label_set_text(s_lbl_dimmer_val, "Tắt");
        lv_obj_set_style_text_color(s_lbl_dimmer_val, UI_COLOR_TEXT_MUTED, 0);
    }
    lv_slider_set_value(s_slider_dimmer, brightness, LV_ANIM_OFF);
}

static void btn_add_dev_event_cb(lv_event_t *e) {
    ESP_LOGI(TAG, "Touch UI: Người dùng chạm thẻ '+ Thêm thiết bị' (Sẵn sàng ghép nối Thread Mesh)");
}

static void relay_toggle_event_cb(lv_event_t *e) {
    if (s_suppress_events) return;
    s_relay_state_local = !s_relay_state_local;
    ESP_LOGI(TAG, "Touch UI: Chạm đúng icon Công tắc -> %s", s_relay_state_local ? "ON" : "OFF");
    update_relay_ui(s_relay_state_local);
    mesh_controller_set_relay(s_relay_state_local);
}

static void dimmer_toggle_event_cb(lv_event_t *e) {
    if (s_suppress_events) return;
    s_dimmer_state_local = !s_dimmer_state_local;
    if (s_dimmer_state_local && s_dimmer_bri_local == 0) {
        s_dimmer_bri_local = 80;
    }
    uint8_t bri = s_dimmer_state_local ? s_dimmer_bri_local : 0;
    ESP_LOGI(TAG, "Touch UI: Chạm đúng icon Đèn chiếu sáng -> %s (%u%%)",
             s_dimmer_state_local ? "ON" : "OFF", bri);
    update_dimmer_ui(s_dimmer_state_local, s_dimmer_bri_local);
    mesh_controller_set_dimmer(s_dimmer_state_local, bri);
}

static void dimmer_slider_event_cb(lv_event_t *e) {
    if (s_suppress_events) return;
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    bool is_on = (val > 0);
    s_dimmer_state_local = is_on;
    if (is_on) {
        s_dimmer_bri_local = (uint8_t)val;
    }
    update_dimmer_ui(is_on, (uint8_t)val);

    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        mesh_controller_set_dimmer(is_on, (uint8_t)val);
    }
}

static void btn_softap_event_cb(lv_event_t *e) {
    ESP_LOGW(TAG, "Touch UI: Người dùng kích hoạt Cài đặt Wi-Fi / SoftAP");
    sys_core_reset_wifi();
}

void ui_screen_dashboard_select_tab(uint16_t tab_idx) {
    if (s_tabview) {
        lv_tabview_set_act(s_tabview, tab_idx, LV_ANIM_OFF);
    }
}

lv_obj_t* ui_screen_dashboard_init(void) {
    if (s_screen_dash) return s_screen_dash;

    s_screen_dash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen_dash, UI_COLOR_BG, 0);
    lv_obj_clear_flag(s_screen_dash, LV_OBJ_FLAG_SCROLLABLE);

    // =========================================================================
    // 1. HEADER BAR TRÊN CÙNG (Chiều cao 36px)
    // =========================================================================
    lv_obj_t *header = lv_obj_create(s_screen_dash);
    lv_obj_set_size(header, UI_LCD_H_RES, 36);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(header, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    // Header Left: Wi-Fi container (Icon + Gạch chéo khi mất mạng + dBm, KHÔNG có chữ wifi)
    s_cont_header_wifi = lv_obj_create(header);
    lv_obj_set_size(s_cont_header_wifi, 90, 24);
    lv_obj_align(s_cont_header_wifi, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_opa(s_cont_header_wifi, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont_header_wifi, 0, 0);
    lv_obj_set_style_pad_all(s_cont_header_wifi, 0, 0);
    lv_obj_clear_flag(s_cont_header_wifi, LV_OBJ_FLAG_SCROLLABLE);

    s_icon_header_wifi = lv_label_create(s_cont_header_wifi);
    lv_obj_align(s_icon_header_wifi, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(s_icon_header_wifi, UI_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_text_font(s_icon_header_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_icon_header_wifi, LV_SYMBOL_WIFI);

    s_slash_header_wifi = lv_line_create(s_cont_header_wifi);
    lv_line_set_points(s_slash_header_wifi, s_hdr_slash_pts, 2);
    lv_obj_set_style_line_color(s_slash_header_wifi, UI_COLOR_ACCENT_RED, 0);
    lv_obj_set_style_line_width(s_slash_header_wifi, 2, 0);
    lv_obj_set_style_line_rounded(s_slash_header_wifi, true, 0);
    lv_obj_align_to(s_slash_header_wifi, s_icon_header_wifi, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_slash_header_wifi, LV_OBJ_FLAG_HIDDEN); // Mặc định ẩn

    s_lbl_header_wifi_dbm = lv_label_create(s_cont_header_wifi);
    lv_obj_align(s_lbl_header_wifi_dbm, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_text_color(s_lbl_header_wifi_dbm, UI_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_text_font(s_lbl_header_wifi_dbm, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_lbl_header_wifi_dbm, "(-65dBm)");

    // Header Center: Digital Clock (SNTP GMT+7)
    s_lbl_header_time = lv_label_create(header);
    lv_obj_align(s_lbl_header_time, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(s_lbl_header_time, UI_COLOR_TEXT_MAIN, 0);
    lv_obj_set_style_text_font(s_lbl_header_time, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lbl_header_time, "--:--:--");

    // Header Right: Mesh Nodes Count
    s_lbl_header_mesh = lv_label_create(header);
    lv_obj_align(s_lbl_header_mesh, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_text_color(s_lbl_header_mesh, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_header_mesh, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_lbl_header_mesh, "Mesh: 3");

    // =========================================================================
    // 2. TAB VIEW (Chiều cao 204px: Y=36 đến Y=240)
    // =========================================================================
    s_tabview = lv_tabview_create(s_screen_dash, LV_DIR_BOTTOM, 36);
    lv_obj_set_size(s_tabview, UI_LCD_H_RES, UI_LCD_V_RES - 36);
    lv_obj_align(s_tabview, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_tabview, UI_COLOR_BG, 0);

    // Tắt thao tác vuốt lướt trên vùng nội dung (chuyển tab bằng nút ở đáy)
    lv_obj_t *tab_content = lv_tabview_get_content(s_tabview);
    lv_obj_clear_flag(tab_content, LV_OBJ_FLAG_SCROLLABLE);

    // Thanh nút Tab ở đáy (Tab Bar)
    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(s_tabview);
    lv_obj_set_style_text_font(tab_btns, &ui_font_vn_14, 0);
    lv_obj_set_style_bg_color(tab_btns, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(tab_btns, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(tab_btns, 1, 0);
    lv_obj_set_style_border_side(tab_btns, LV_BORDER_SIDE_TOP, 0);

    lv_obj_t *tab_devices = lv_tabview_add_tab(s_tabview, "Thiết bị");
    lv_obj_t *tab_env     = lv_tabview_add_tab(s_tabview, "Môi trường");
    lv_obj_t *tab_system  = lv_tabview_add_tab(s_tabview, "Hệ thống");

    lv_obj_set_style_pad_all(tab_devices, 8, 0);
    lv_obj_set_style_pad_all(tab_env, 8, 0);
    lv_obj_set_style_pad_all(tab_system, 8, 0);

    // -------------------------------------------------------------------------
    // TAB 1: THIẾT BỊ (Apple Home Interactive Tile Cards - Đối Xứng Hoàn Hảo)
    // -------------------------------------------------------------------------
    lv_obj_set_scroll_dir(tab_devices, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tab_devices, LV_SCROLLBAR_MODE_AUTO);

    // ==================== Card 1: Công tắc (Trái) ====================
    s_card_relay = lv_obj_create(tab_devices);
    lv_obj_set_size(s_card_relay, 144, 135);
    lv_obj_align(s_card_relay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_card_relay, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_card_relay, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_card_relay, 1, 0);
    lv_obj_set_style_radius(s_card_relay, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(s_card_relay, 10, 0);
    lv_obj_clear_flag(s_card_relay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_card_relay, LV_OBJ_FLAG_CLICKABLE); // Card không nhận click

    // Icon Button Công tắc tròn 44x44px (CHỈ CHẠM ICON MỚI BẬT/TẮT)
    s_icon_relay_btn = lv_obj_create(s_card_relay);
    lv_obj_set_size(s_icon_relay_btn, 44, 44);
    lv_obj_align(s_icon_relay_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(s_icon_relay_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_icon_relay_btn, lv_color_hex(0xF2F2F7), 0);
    lv_obj_set_style_border_width(s_icon_relay_btn, 0, 0);
    lv_obj_set_style_pad_all(s_icon_relay_btn, 0, 0);
    lv_obj_clear_flag(s_icon_relay_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_icon_relay_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_icon_relay_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_icon_relay_btn, relay_toggle_event_cb, LV_EVENT_CLICKED, NULL);

    s_icon_relay = lv_label_create(s_icon_relay_btn);
    lv_obj_center(s_icon_relay);
    lv_obj_set_style_text_font(s_icon_relay, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_icon_relay, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_icon_relay, LV_SYMBOL_POWER);

    // Nhãn phụ trạng thái (Góc trên phải, đối xứng hoàn hảo với Đèn)
    s_lbl_relay_state = lv_label_create(s_card_relay);
    lv_obj_align(s_lbl_relay_state, LV_ALIGN_TOP_RIGHT, 0, 12);
    lv_obj_set_style_text_font(s_lbl_relay_state, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_relay_state, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_lbl_relay_state, "Tắt");

    // Tiêu đề thiết bị (Đồng vị trí với Đèn: Y=54)
    s_lbl_relay_title = lv_label_create(s_card_relay);
    lv_obj_align(s_lbl_relay_title, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_obj_set_style_text_font(s_lbl_relay_title, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_relay_title, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_relay_title, "Công tắc");

    // ==================== Card 2: Đèn chiếu sáng (Phải) ====================
    s_card_dimmer = lv_obj_create(tab_devices);
    lv_obj_set_size(s_card_dimmer, 144, 135);
    lv_obj_align(s_card_dimmer, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_card_dimmer, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_card_dimmer, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_card_dimmer, 1, 0);
    lv_obj_set_style_radius(s_card_dimmer, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(s_card_dimmer, 10, 0);
    lv_obj_clear_flag(s_card_dimmer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_card_dimmer, LV_OBJ_FLAG_CLICKABLE);

    // Icon Button Đèn tròn 44x44px (CHỈ CHẠM ICON MỚI BẬT/TẮT)
    s_icon_dimmer_btn = lv_obj_create(s_card_dimmer);
    lv_obj_set_size(s_icon_dimmer_btn, 44, 44);
    lv_obj_align(s_icon_dimmer_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(s_icon_dimmer_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_icon_dimmer_btn, lv_color_hex(0xF2F2F7), 0);
    lv_obj_set_style_border_width(s_icon_dimmer_btn, 0, 0);
    lv_obj_set_style_pad_all(s_icon_dimmer_btn, 0, 0);
    lv_obj_clear_flag(s_icon_dimmer_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_icon_dimmer_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_icon_dimmer_btn, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_icon_dimmer_btn, dimmer_toggle_event_cb, LV_EVENT_CLICKED, NULL);

    // Đồ họa bóng đèn sắc nét: Bầu đèn tròn + Đui kim loại
    s_bulb_head = lv_obj_create(s_icon_dimmer_btn);
    lv_obj_set_size(s_bulb_head, 18, 18);
    lv_obj_align(s_bulb_head, LV_ALIGN_CENTER, 0, -3);
    lv_obj_set_style_radius(s_bulb_head, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_bulb_head, lv_color_hex(0xD1D5DB), 0);
    lv_obj_set_style_border_width(s_bulb_head, 0, 0);
    lv_obj_clear_flag(s_bulb_head, LV_OBJ_FLAG_SCROLLABLE);

    s_bulb_base = lv_obj_create(s_icon_dimmer_btn);
    lv_obj_set_size(s_bulb_base, 8, 5);
    lv_obj_align(s_bulb_base, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_radius(s_bulb_base, 2, 0);
    lv_obj_set_style_bg_color(s_bulb_base, lv_color_hex(0x9CA3AF), 0);
    lv_obj_set_style_border_width(s_bulb_base, 0, 0);
    lv_obj_clear_flag(s_bulb_base, LV_OBJ_FLAG_SCROLLABLE);

    // Nhãn % độ sáng (Góc trên phải, đối xứng hoàn hảo với Công tắc)
    s_lbl_dimmer_val = lv_label_create(s_card_dimmer);
    lv_obj_align(s_lbl_dimmer_val, LV_ALIGN_TOP_RIGHT, 0, 12);
    lv_obj_set_style_text_font(s_lbl_dimmer_val, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_dimmer_val, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_lbl_dimmer_val, "Tắt");

    // Tiêu đề thiết bị (Đồng vị trí với Công tắc: Y=54)
    s_lbl_dimmer_title = lv_label_create(s_card_dimmer);
    lv_obj_align(s_lbl_dimmer_title, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_obj_set_style_text_font(s_lbl_dimmer_title, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_dimmer_title, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_dimmer_title, "Đèn");

    // Thanh Slider điều chỉnh độ sáng (Rộng 108px căn giữa, không chạm mép khi 0% hoặc 100%)
    s_slider_dimmer = lv_slider_create(s_card_dimmer);
    lv_obj_set_size(s_slider_dimmer, 108, 8);
    lv_obj_align(s_slider_dimmer, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_slider_set_range(s_slider_dimmer, 0, 100);
    lv_obj_set_style_pad_hor(s_slider_dimmer, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_dimmer, lv_color_hex(0xE5E5EA), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_dimmer, UI_COLOR_LIGHT_WARM, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_dimmer, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_border_color(s_slider_dimmer, UI_COLOR_LIGHT_WARM, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_slider_dimmer, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider_dimmer, 1, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider_dimmer, dimmer_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    // ==================== Card 3: Thẻ '+ Thêm thiết bị' ====================
    lv_obj_t *card_add_dev = lv_obj_create(tab_devices);
    lv_obj_set_size(card_add_dev, 144, 120);
    lv_obj_align(card_add_dev, LV_ALIGN_TOP_LEFT, 0, 145);
    lv_obj_set_style_bg_color(card_add_dev, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(card_add_dev, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(card_add_dev, 1, 0);
    lv_obj_set_style_radius(card_add_dev, UI_RADIUS_CARD, 0);
    lv_obj_clear_flag(card_add_dev, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card_add_dev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card_add_dev, btn_add_dev_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon_plus = lv_label_create(card_add_dev);
    lv_obj_align(icon_plus, LV_ALIGN_CENTER, 0, -12);
    lv_obj_set_style_text_font(icon_plus, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon_plus, UI_COLOR_ACCENT_BLUE, 0);
    lv_label_set_text(icon_plus, LV_SYMBOL_PLUS);

    lv_obj_t *lbl_add_title = lv_label_create(card_add_dev);
    lv_obj_align(lbl_add_title, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_text_font(lbl_add_title, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_add_title, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(lbl_add_title, "Thêm thiết bị");

    // -------------------------------------------------------------------------
    // TAB 2: MÔI TRƯỜNG (DHT11 - Font 32px Lớn Căn Giữa, Đối Xứng 100%)
    // -------------------------------------------------------------------------
    // Card Trái: Nhiệt độ phòng
    s_card_temp = lv_obj_create(tab_env);
    lv_obj_set_size(s_card_temp, 144, 150);
    lv_obj_align(s_card_temp, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_card_temp, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_card_temp, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_card_temp, 1, 0);
    lv_obj_set_style_radius(s_card_temp, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(s_card_temp, 12, 0);
    lv_obj_clear_flag(s_card_temp, LV_OBJ_FLAG_SCROLLABLE);

    // Header Nhiệt độ: Icon Nhiệt kế đồ họa + Chữ Nhiệt độ
    lv_obj_t *t_hdr_cont = lv_obj_create(s_card_temp);
    lv_obj_set_size(t_hdr_cont, 120, 24);
    lv_obj_align(t_hdr_cont, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(t_hdr_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(t_hdr_cont, 0, 0);
    lv_obj_set_style_pad_all(t_hdr_cont, 0, 0);
    lv_obj_clear_flag(t_hdr_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t_bulb = lv_obj_create(t_hdr_cont);
    lv_obj_set_size(t_bulb, 8, 8);
    lv_obj_align(t_bulb, LV_ALIGN_LEFT_MID, 0, 3);
    lv_obj_set_style_radius(t_bulb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(t_bulb, UI_COLOR_ACCENT_ORANGE, 0);
    lv_obj_set_style_border_width(t_bulb, 0, 0);
    lv_obj_clear_flag(t_bulb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t_stem = lv_obj_create(t_hdr_cont);
    lv_obj_set_size(t_stem, 4, 10);
    lv_obj_align(t_stem, LV_ALIGN_LEFT_MID, 2, -3);
    lv_obj_set_style_radius(t_stem, 2, 0);
    lv_obj_set_style_bg_color(t_stem, UI_COLOR_ACCENT_ORANGE, 0);
    lv_obj_set_style_border_width(t_stem, 0, 0);
    lv_obj_clear_flag(t_stem, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_t_header = lv_label_create(t_hdr_cont);
    lv_obj_align(lbl_t_header, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_font(lbl_t_header, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_t_header, UI_COLOR_ACCENT_ORANGE, 0);
    lv_label_set_text(lbl_t_header, "Nhiệt độ");

    // Số đo lớn 32px căn giữa trọn vẹn thẻ
    s_lbl_temp_val = lv_label_create(s_card_temp);
    lv_obj_align(s_lbl_temp_val, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_text_font(s_lbl_temp_val, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_temp_val, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_temp_val, "28.5 °C");

    // Card Phải: Độ ẩm không khí (Đối xứng hoàn hảo với thẻ Nhiệt độ)
    s_card_hum = lv_obj_create(tab_env);
    lv_obj_set_size(s_card_hum, 144, 150);
    lv_obj_align(s_card_hum, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_card_hum, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_card_hum, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_card_hum, 1, 0);
    lv_obj_set_style_radius(s_card_hum, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(s_card_hum, 12, 0);
    lv_obj_clear_flag(s_card_hum, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h_hdr_cont = lv_obj_create(s_card_hum);
    lv_obj_set_size(h_hdr_cont, 120, 24);
    lv_obj_align(h_hdr_cont, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(h_hdr_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(h_hdr_cont, 0, 0);
    lv_obj_set_style_pad_all(h_hdr_cont, 0, 0);
    lv_obj_clear_flag(h_hdr_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_h = lv_label_create(h_hdr_cont);
    lv_obj_align(icon_h, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(icon_h, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(icon_h, UI_COLOR_ACCENT_BLUE, 0);
    lv_label_set_text(icon_h, LV_SYMBOL_TINT);

    lv_obj_t *lbl_h_header = lv_label_create(h_hdr_cont);
    lv_obj_align(lbl_h_header, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_font(lbl_h_header, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_h_header, UI_COLOR_ACCENT_BLUE, 0);
    lv_label_set_text(lbl_h_header, "Độ ẩm");

    // Số đo lớn 32px căn giữa trọn vẹn thẻ (Chỉ hiển thị % không có chữ RH)
    s_lbl_hum_val = lv_label_create(s_card_hum);
    lv_obj_align(s_lbl_hum_val, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_text_font(s_lbl_hum_val, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_hum_val, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_hum_val, "65 %");

    // -------------------------------------------------------------------------
    // TAB 3: HỆ THỐNG (Network, Topology, Provisioning)
    // -------------------------------------------------------------------------
    lv_obj_t *card_sys = lv_obj_create(tab_system);
    lv_obj_set_size(card_sys, 300, 120);
    lv_obj_align(card_sys, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card_sys, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(card_sys, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(card_sys, 1, 0);
    lv_obj_set_style_radius(card_sys, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card_sys, 8, 0);
    lv_obj_clear_flag(card_sys, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_sys_ip = lv_label_create(card_sys);
    lv_obj_align(s_lbl_sys_ip, LV_ALIGN_TOP_LEFT, 4, 0);
    lv_obj_set_style_text_font(s_lbl_sys_ip, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_lbl_sys_ip, "IP: 192.168.?.?");

    s_lbl_sys_cloud = lv_label_create(card_sys);
    lv_obj_align(s_lbl_sys_cloud, LV_ALIGN_TOP_LEFT, 4, 20);
    lv_obj_set_style_text_font(s_lbl_sys_cloud, &ui_font_vn_14, 0);
    lv_label_set_text(s_lbl_sys_cloud, "ThingsBoard: Đã kết nối");

    s_lbl_sys_mesh = lv_label_create(card_sys);
    lv_obj_align(s_lbl_sys_mesh, LV_ALIGN_TOP_LEFT, 4, 40);
    lv_obj_set_style_text_font(s_lbl_sys_mesh, &ui_font_vn_14, 0);
    lv_label_set_text(s_lbl_sys_mesh, "Mạng Thread: 3 nút (Kênh 15)");

    lv_obj_t *btn_softap = lv_btn_create(card_sys);
    lv_obj_set_size(btn_softap, 150, 28);
    lv_obj_align(btn_softap, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_softap, UI_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(btn_softap, UI_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(btn_softap, btn_softap_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_btn = lv_label_create(btn_softap);
    lv_obj_center(lbl_btn);
    lv_obj_set_style_text_font(lbl_btn, &ui_font_vn_14, 0);
    lv_label_set_text(lbl_btn, "Đổi Wi-Fi / API Key");

    return s_screen_dash;
}

void ui_screen_dashboard_update_state(const gateway_device_state_t *state) {
    if (!state || !s_screen_dash) return;
    s_suppress_events = true;

    s_relay_state_local = state->relay_on;
    s_dimmer_state_local = state->dimmer_on;
    s_dimmer_bri_local = state->dimmer_brightness;

    // 1. Cập nhật Thẻ Công tắc
    if (state->relay_node_online) {
        lv_obj_set_style_opa(s_card_relay, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_icon_relay_btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_set_style_opa(s_card_relay, LV_OPA_40, 0);
        lv_obj_clear_flag(s_icon_relay_btn, LV_OBJ_FLAG_CLICKABLE);
    }
    update_relay_ui(state->relay_on);

    // 2. Cập nhật Thẻ Đèn Dimmer
    if (state->dimmer_node_online) {
        lv_obj_set_style_opa(s_card_dimmer, LV_OPA_COVER, 0);
        lv_obj_add_flag(s_icon_dimmer_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_state(s_slider_dimmer, LV_STATE_DISABLED);
    } else {
        lv_obj_set_style_opa(s_card_dimmer, LV_OPA_40, 0);
        lv_obj_clear_flag(s_icon_dimmer_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_state(s_slider_dimmer, LV_STATE_DISABLED);
    }
    update_dimmer_ui(state->dimmer_on, state->dimmer_brightness);

    // 3. Cập nhật Môi trường DHT11 (2 Thẻ Card đối xứng)
    if (state->sensor_node_online && state->sensor_valid) {
        lv_obj_set_style_opa(s_card_temp, LV_OPA_COVER, 0);
        lv_obj_set_style_opa(s_card_hum, LV_OPA_COVER, 0);

        char t_buf[24], h_buf[24];
        snprintf(t_buf, sizeof(t_buf), "%.1f °C", state->temperature);
        snprintf(h_buf, sizeof(h_buf), "%.0f %%", state->humidity);
        lv_label_set_text(s_lbl_temp_val, t_buf);
        lv_label_set_text(s_lbl_hum_val, h_buf);
    } else {
        lv_obj_set_style_opa(s_card_temp, LV_OPA_40, 0);
        lv_obj_set_style_opa(s_card_hum, LV_OPA_40, 0);
        lv_label_set_text(s_lbl_temp_val, "--.- °C");
        lv_label_set_text(s_lbl_hum_val, "-- %");
    }

    // 4. Header Mesh Count
    char mesh_buf[32];
    snprintf(mesh_buf, sizeof(mesh_buf), "Mạng: %u nút", state->mesh_node_count);
    lv_label_set_text(s_lbl_header_mesh, mesh_buf);
    snprintf(mesh_buf, sizeof(mesh_buf), "Mạng Thread: %u nút", state->mesh_node_count);
    lv_label_set_text(s_lbl_sys_mesh, mesh_buf);

    s_suppress_events = false;
}

void ui_screen_dashboard_update_time(const struct tm *timeinfo) {
    if (!s_lbl_header_time || !timeinfo) return;
    char time_str[24];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d",
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    lv_label_set_text(s_lbl_header_time, time_str);
}

void ui_screen_dashboard_update_network(bool wifi_ok, const char *ip_str, int8_t rssi, bool cloud_ok) {
    if (s_icon_header_wifi && s_slash_header_wifi) {
        if (wifi_ok) {
            lv_obj_set_style_text_color(s_icon_header_wifi, UI_COLOR_ACCENT_BLUE, 0);
            lv_obj_add_flag(s_slash_header_wifi, LV_OBJ_FLAG_HIDDEN);
            if (s_lbl_header_wifi_dbm) {
                char buf[16];
                snprintf(buf, sizeof(buf), " (%ddBm)", rssi);
                lv_label_set_text(s_lbl_header_wifi_dbm, buf);
                lv_obj_clear_flag(s_lbl_header_wifi_dbm, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_set_style_text_color(s_icon_header_wifi, UI_COLOR_TEXT_MUTED, 0);
            lv_obj_clear_flag(s_slash_header_wifi, LV_OBJ_FLAG_HIDDEN);
            if (s_lbl_header_wifi_dbm) {
                lv_obj_add_flag(s_lbl_header_wifi_dbm, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (s_lbl_sys_ip && ip_str) {
        char ip_buf[36];
        snprintf(ip_buf, sizeof(ip_buf), "IP: %s", ip_str);
        lv_label_set_text(s_lbl_sys_ip, ip_buf);
    }
    if (s_lbl_sys_cloud) {
        lv_label_set_text(s_lbl_sys_cloud, cloud_ok ? "ThingsBoard: Đã kết nối" : "ThingsBoard: Ngoại tuyến");
        lv_obj_set_style_text_color(s_lbl_sys_cloud, cloud_ok ? UI_COLOR_ACCENT_GREEN : UI_COLOR_ACCENT_RED, 0);
    }
}
