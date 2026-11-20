#include "ui_screen_idle.h"
#include "ui_theme.h"
#include "ui_display.h"
#include <stdio.h>

static lv_obj_t *s_screen_idle = NULL;
static lv_obj_t *s_label_time = NULL;
static lv_obj_t *s_label_date = NULL;
static lv_obj_t *s_lbl_idle_mesh = NULL;

// Wi-Fi Header Widgets (Idle)
static lv_obj_t *s_cont_idle_status = NULL;
static lv_obj_t *s_icon_idle_wifi = NULL;
static lv_obj_t *s_slash_idle_wifi = NULL;

// Ambient Pill Widgets (Idle)
static lv_obj_t *s_lbl_idle_temp = NULL;
static lv_obj_t *s_lbl_idle_hum = NULL;

// Weather Graphic Objects
static lv_obj_t *s_weather_cont = NULL;
static lv_obj_t *s_weather_sun_cont = NULL;
static lv_obj_t *s_weather_moon_cont = NULL;

static const lv_point_t s_slash_pts[] = {{1, 13}, {13, 1}};

// Hàm tạo biểu tượng "Ông Mặt Trời" với 8 tia nắng toả sáng rực rỡ
static lv_obj_t* create_sun_icon(lv_obj_t *parent) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 28, 28);
    lv_obj_center(cont);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Quả cầu mặt trời tròn trung tâm
    lv_obj_t *core = lv_obj_create(cont);
    lv_obj_set_size(core, 12, 12);
    lv_obj_center(core);
    lv_obj_set_style_radius(core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(core, UI_COLOR_WEATHER_SUN, 0);
    lv_obj_set_style_border_width(core, 0, 0);
    lv_obj_clear_flag(core, LV_OBJ_FLAG_SCROLLABLE);

    // 2. 4 tia nắng chính (Bắc, Nam, Tây, Đông)
    // Tia Bắc
    lv_obj_t *ray_n = lv_obj_create(cont);
    lv_obj_set_size(ray_n, 2, 5);
    lv_obj_align(ray_n, LV_ALIGN_CENTER, 0, -9);
    lv_obj_set_style_radius(ray_n, 1, 0);
    lv_obj_set_style_bg_color(ray_n, UI_COLOR_WEATHER_SUN, 0);
    lv_obj_set_style_border_width(ray_n, 0, 0);
    lv_obj_clear_flag(ray_n, LV_OBJ_FLAG_SCROLLABLE);

    // Tia Nam
    lv_obj_t *ray_s = lv_obj_create(cont);
    lv_obj_set_size(ray_s, 2, 5);
    lv_obj_align(ray_s, LV_ALIGN_CENTER, 0, 9);
    lv_obj_set_style_radius(ray_s, 1, 0);
    lv_obj_set_style_bg_color(ray_s, UI_COLOR_WEATHER_SUN, 0);
    lv_obj_set_style_border_width(ray_s, 0, 0);
    lv_obj_clear_flag(ray_s, LV_OBJ_FLAG_SCROLLABLE);

    // Tia Tây
    lv_obj_t *ray_w = lv_obj_create(cont);
    lv_obj_set_size(ray_w, 5, 2);
    lv_obj_align(ray_w, LV_ALIGN_CENTER, -9, 0);
    lv_obj_set_style_radius(ray_w, 1, 0);
    lv_obj_set_style_bg_color(ray_w, UI_COLOR_WEATHER_SUN, 0);
    lv_obj_set_style_border_width(ray_w, 0, 0);
    lv_obj_clear_flag(ray_w, LV_OBJ_FLAG_SCROLLABLE);

    // Tia Đông
    lv_obj_t *ray_e = lv_obj_create(cont);
    lv_obj_set_size(ray_e, 5, 2);
    lv_obj_align(ray_e, LV_ALIGN_CENTER, 9, 0);
    lv_obj_set_style_radius(ray_e, 1, 0);
    lv_obj_set_style_bg_color(ray_e, UI_COLOR_WEATHER_SUN, 0);
    lv_obj_set_style_border_width(ray_e, 0, 0);
    lv_obj_clear_flag(ray_e, LV_OBJ_FLAG_SCROLLABLE);

    // 3. 4 tia nắng chéo 45 độ (Tây Bắc, Đông Bắc, Tây Nam, Đông Nam)
    const int8_t diag_pos[4][2] = {{-6, -6}, {6, -6}, {-6, 6}, {6, 6}};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *ray_d = lv_obj_create(cont);
        lv_obj_set_size(ray_d, 3, 3);
        lv_obj_align(ray_d, LV_ALIGN_CENTER, diag_pos[i][0], diag_pos[i][1]);
        lv_obj_set_style_radius(ray_d, 1, 0);
        lv_obj_set_style_bg_color(ray_d, UI_COLOR_WEATHER_SUN, 0);
        lv_obj_set_style_border_width(ray_d, 0, 0);
        lv_obj_clear_flag(ray_d, LV_OBJ_FLAG_SCROLLABLE);
    }

    return cont;
}

// Hàm tạo biểu tượng Mặt Trăng khuyết ban đêm kèm ngôi sao nhỏ
static lv_obj_t* create_moon_icon(lv_obj_t *parent) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 28, 28);
    lv_obj_center(cont);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // Thân mặt trăng xanh ngọc
    lv_obj_t *moon_base = lv_obj_create(cont);
    lv_obj_set_size(moon_base, 20, 20);
    lv_obj_center(moon_base);
    lv_obj_set_style_radius(moon_base, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(moon_base, UI_COLOR_WEATHER_MOON, 0);
    lv_obj_set_style_border_width(moon_base, 0, 0);
    lv_obj_clear_flag(moon_base, LV_OBJ_FLAG_SCROLLABLE);

    // Mặt cắt nền tạo vầng trăng khuyết
    lv_obj_t *moon_cut = lv_obj_create(cont);
    lv_obj_set_size(moon_cut, 16, 16);
    lv_obj_align(moon_cut, LV_ALIGN_CENTER, 4, -3);
    lv_obj_set_style_radius(moon_cut, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(moon_cut, UI_COLOR_STANDBY_BG, 0);
    lv_obj_set_style_border_width(moon_cut, 0, 0);
    lv_obj_clear_flag(moon_cut, LV_OBJ_FLAG_SCROLLABLE);

    // Ngôi sao nhỏ lấp lánh cạnh vầng trăng
    lv_obj_t *star = lv_obj_create(cont);
    lv_obj_set_size(star, 3, 3);
    lv_obj_align(star, LV_ALIGN_CENTER, -7, -6);
    lv_obj_set_style_radius(star, 1, 0);
    lv_obj_set_style_bg_color(star, UI_COLOR_WEATHER_MOON, 0);
    lv_obj_set_style_border_width(star, 0, 0);
    lv_obj_clear_flag(star, LV_OBJ_FLAG_SCROLLABLE);

    return cont;
}

lv_obj_t* ui_screen_idle_init(void) {
    if (s_screen_idle) return s_screen_idle;

    s_screen_idle = lv_obj_create(NULL);
    // Áp dụng tông nền Apple Standby Dark Theme (#0B0E14) - Đen tuyền tối giản
    lv_obj_set_style_bg_color(s_screen_idle, UI_COLOR_STANDBY_BG, 0);
    lv_obj_clear_flag(s_screen_idle, LV_OBJ_FLAG_SCROLLABLE);

    // =========================================================================
    // 1. GÓC TRÊN TRÁI: WIDGET THỜI TIẾT ĐỒ HỌA ("Ông Mặt Trời" tia nắng / Trăng khuyết)
    // =========================================================================
    s_weather_cont = lv_obj_create(s_screen_idle);
    lv_obj_set_size(s_weather_cont, 28, 28);
    lv_obj_align(s_weather_cont, LV_ALIGN_TOP_LEFT, 14, 12);
    lv_obj_set_style_bg_opa(s_weather_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_weather_cont, 0, 0);
    lv_obj_set_style_pad_all(s_weather_cont, 0, 0);
    lv_obj_clear_flag(s_weather_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_weather_sun_cont = create_sun_icon(s_weather_cont);
    s_weather_moon_cont = create_moon_icon(s_weather_cont);

    // Mặc định ban đầu hiển thị ban ngày
    lv_obj_clear_flag(s_weather_sun_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_weather_moon_cont, LV_OBJ_FLAG_HIDDEN);

    // =========================================================================
    // 2. GÓC TRÊN PHẢI: ICON WI-FI ĐỘC LẬP (Có gạch chéo khi mất mạng, không chữ)
    // =========================================================================
    s_cont_idle_status = lv_obj_create(s_screen_idle);
    lv_obj_set_size(s_cont_idle_status, 24, 24);
    lv_obj_align(s_cont_idle_status, LV_ALIGN_TOP_RIGHT, -14, 12);
    lv_obj_set_style_bg_opa(s_cont_idle_status, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont_idle_status, 0, 0);
    lv_obj_set_style_pad_all(s_cont_idle_status, 0, 0);
    lv_obj_clear_flag(s_cont_idle_status, LV_OBJ_FLAG_SCROLLABLE);

    s_icon_idle_wifi = lv_label_create(s_cont_idle_status);
    lv_obj_center(s_icon_idle_wifi);
    lv_obj_set_style_text_color(s_icon_idle_wifi, UI_COLOR_WEATHER_MOON, 0);
    lv_obj_set_style_text_font(s_icon_idle_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_icon_idle_wifi, LV_SYMBOL_WIFI);

    s_slash_idle_wifi = lv_line_create(s_cont_idle_status);
    lv_line_set_points(s_slash_idle_wifi, s_slash_pts, 2);
    lv_obj_set_style_line_color(s_slash_idle_wifi, UI_COLOR_ACCENT_RED, 0);
    lv_obj_set_style_line_width(s_slash_idle_wifi, 2, 0);
    lv_obj_set_style_line_rounded(s_slash_idle_wifi, true, 0);
    lv_obj_center(s_slash_idle_wifi);
    lv_obj_add_flag(s_slash_idle_wifi, LV_OBJ_FLAG_HIDDEN); // Mặc định ẩn gạch chéo

    // =========================================================================
    // 3. CỤM TRUNG TÂM: ĐỒNG HỒ -> NGÀY THÁNG -> MẠNG THIẾT BỊ
    // =========================================================================
    // Đồng hồ số lớn 48pt
    s_label_time = lv_label_create(s_screen_idle);
    lv_obj_align(s_label_time, LV_ALIGN_CENTER, 0, -28);
    lv_obj_set_style_text_color(s_label_time, UI_COLOR_STANDBY_TEXT, 0);
    lv_obj_set_style_text_font(s_label_time, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_label_time, "--:--");

    // Thứ ngày tháng Tiếng Việt
    s_label_date = lv_label_create(s_screen_idle);
    lv_obj_set_style_text_align(s_label_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_label_date, LV_ALIGN_CENTER, 0, 14);
    lv_obj_set_style_text_color(s_label_date, UI_COLOR_STANDBY_MUTED, 0);
    lv_obj_set_style_text_font(s_label_date, &ui_font_vn_14, 0);
    lv_label_set_text(s_label_date, "Chờ đồng bộ giờ SNTP...");

    // Thông tin số lượng thiết bị mạng đặt ngay dưới ngày tháng
    s_lbl_idle_mesh = lv_label_create(s_screen_idle);
    lv_obj_set_style_text_align(s_lbl_idle_mesh, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_idle_mesh, LV_ALIGN_CENTER, 0, 38);
    lv_obj_set_style_text_color(s_lbl_idle_mesh, UI_COLOR_STANDBY_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_idle_mesh, &ui_font_vn_14, 0);
    lv_label_set_text(s_lbl_idle_mesh, "Mạng: 0 thiết bị");

    // =========================================================================
    // 4. PILL MÔI TRƯỜNG PHÁT QUANG (Căn giữa hoàn hảo bằng Flexbox)
    // =========================================================================
    lv_obj_t *pill_container = lv_obj_create(s_screen_idle);
    lv_obj_set_size(pill_container, 220, 32);
    lv_obj_align(pill_container, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(pill_container, UI_COLOR_STANDBY_CARD, 0);
    lv_obj_set_style_border_color(pill_container, UI_COLOR_STANDBY_BORDER, 0);
    lv_obj_set_style_border_width(pill_container, 1, 0);
    lv_obj_set_style_radius(pill_container, UI_RADIUS_PILL, 0);
    lv_obj_set_style_pad_all(pill_container, 0, 0);
    lv_obj_clear_flag(pill_container, LV_OBJ_FLAG_SCROLLABLE);

    // Flexbox Row căn giữa tuyệt đối cả chiều ngang và chiều dọc
    lv_obj_t *pill_flex = lv_obj_create(pill_container);
    lv_obj_set_size(pill_flex, LV_PCT(100), LV_PCT(100));
    lv_obj_center(pill_flex);
    lv_obj_set_style_bg_opa(pill_flex, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pill_flex, 0, 0);
    lv_obj_set_style_pad_all(pill_flex, 0, 0);
    lv_obj_set_style_pad_column(pill_flex, 6, 0);
    lv_obj_set_flex_flow(pill_flex, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill_flex, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pill_flex, LV_OBJ_FLAG_SCROLLABLE);

    // Icon Nhiệt kế đồ họa (Bầu tròn + cột đo màu đỏ cam)
    lv_obj_t *thermo_cont = lv_obj_create(pill_flex);
    lv_obj_set_size(thermo_cont, 10, 16);
    lv_obj_set_style_bg_opa(thermo_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(thermo_cont, 0, 0);
    lv_obj_set_style_pad_all(thermo_cont, 0, 0);
    lv_obj_clear_flag(thermo_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *thermo_bulb = lv_obj_create(thermo_cont);
    lv_obj_set_size(thermo_bulb, 8, 8);
    lv_obj_align(thermo_bulb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(thermo_bulb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(thermo_bulb, UI_COLOR_ACCENT_ORANGE, 0);
    lv_obj_set_style_border_width(thermo_bulb, 0, 0);
    lv_obj_clear_flag(thermo_bulb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *thermo_stem = lv_obj_create(thermo_cont);
    lv_obj_set_size(thermo_stem, 4, 10);
    lv_obj_align(thermo_stem, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(thermo_stem, 2, 0);
    lv_obj_set_style_bg_color(thermo_stem, UI_COLOR_ACCENT_ORANGE, 0);
    lv_obj_set_style_border_width(thermo_stem, 0, 0);
    lv_obj_clear_flag(thermo_stem, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_idle_temp = lv_label_create(pill_flex);
    lv_obj_set_style_text_color(s_lbl_idle_temp, UI_COLOR_STANDBY_TEXT, 0);
    lv_obj_set_style_text_font(s_lbl_idle_temp, &ui_font_vn_14, 0);
    lv_label_set_text(s_lbl_idle_temp, "28.5 °C");

    lv_obj_t *lbl_sep = lv_label_create(pill_flex);
    lv_obj_set_style_text_color(lbl_sep, UI_COLOR_STANDBY_MUTED, 0);
    lv_obj_set_style_text_font(lbl_sep, &ui_font_vn_14, 0);
    lv_label_set_text(lbl_sep, "  •  ");

    lv_obj_t *icon_hum = lv_label_create(pill_flex);
    lv_obj_set_style_text_color(icon_hum, UI_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_text_font(icon_hum, &lv_font_montserrat_14, 0);
    lv_label_set_text(icon_hum, LV_SYMBOL_TINT);

    s_lbl_idle_hum = lv_label_create(pill_flex);
    lv_obj_set_style_text_color(s_lbl_idle_hum, UI_COLOR_STANDBY_TEXT, 0);
    lv_obj_set_style_text_font(s_lbl_idle_hum, &ui_font_vn_14, 0);
    lv_label_set_text(s_lbl_idle_hum, "65 %");

    return s_screen_idle;
}

void ui_screen_idle_update_time(const struct tm *timeinfo) {
    if (!s_label_time || !s_label_date || !timeinfo) return;

    char time_str[16];
    char date_str[48];

    // Format HH:MM
    snprintf(time_str, sizeof(time_str), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    lv_label_set_text(s_label_time, time_str);

    // Format Ngày/Tháng tiếng Việt
    const char *wday_names[] = {"Chủ Nhật", "Thứ Hai", "Thứ Ba", "Thứ Tư", "Thứ Năm", "Thứ Sáu", "Thứ Bảy"};
    snprintf(date_str, sizeof(date_str), "%s, %02d/%02d/%04d",
             wday_names[timeinfo->tm_wday % 7],
             timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900);
    lv_label_set_text(s_label_date, date_str);

    // Cập nhật biểu tượng thời tiết đồ họa (Mặt trời ban ngày / Mặt trăng ban đêm)
    if (s_weather_sun_cont && s_weather_moon_cont) {
        if (timeinfo->tm_hour >= 6 && timeinfo->tm_hour < 18) {
            // Ban ngày: Hiện "Ông Mặt Trời" toả nắng, Ẩn Mặt trăng
            lv_obj_clear_flag(s_weather_sun_cont, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_weather_moon_cont, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Ban đêm: Hiện Mặt trăng khuyết, Ẩn Mặt trời
            lv_obj_add_flag(s_weather_sun_cont, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_weather_moon_cont, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_screen_idle_update_sensors(float temp, float hum, bool valid) {
    if (!s_lbl_idle_temp || !s_lbl_idle_hum) return;
    char t_buf[24], h_buf[24];
    if (valid) {
        snprintf(t_buf, sizeof(t_buf), "%.1f °C", temp);
        snprintf(h_buf, sizeof(h_buf), "%.0f %%", hum);
    } else {
        snprintf(t_buf, sizeof(t_buf), "--.- °C");
        snprintf(h_buf, sizeof(h_buf), "-- %%");
    }
    lv_label_set_text(s_lbl_idle_temp, t_buf);
    lv_label_set_text(s_lbl_idle_hum, h_buf);
}

void ui_screen_idle_update_status(bool wifi_ok, bool cloud_ok, uint8_t online_nodes, uint8_t total_nodes) {
    if (s_icon_idle_wifi && s_slash_idle_wifi) {
        if (wifi_ok) {
            lv_obj_set_style_text_color(s_icon_idle_wifi, UI_COLOR_WEATHER_MOON, 0);
            lv_obj_add_flag(s_slash_idle_wifi, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_style_text_color(s_icon_idle_wifi, UI_COLOR_STANDBY_MUTED, 0);
            lv_obj_clear_flag(s_slash_idle_wifi, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_lbl_idle_mesh) {
        char buf[32];
        if (total_nodes == 0) {
            snprintf(buf, sizeof(buf), "Mạng: 0 thiết bị");
        } else {
            snprintf(buf, sizeof(buf), "Mạng: %u/%u kết nối", online_nodes, total_nodes);
        }
        lv_label_set_text(s_lbl_idle_mesh, buf);
    }
}
