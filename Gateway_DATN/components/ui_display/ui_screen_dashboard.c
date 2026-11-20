#include "ui_screen_dashboard.h"
#include "ui_theme.h"
#include "ui_display.h"
#include "mesh_controller.h"
#include "thread_commissioner.h"
#include "thread_gateway.h"
#include "sys_core.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

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

// Tab 1 (Thiết bị) Widgets - Apple Home Interactive Tiles
static lv_obj_t *s_tab_devices = NULL;
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

// Tab 1: Empty State & Add Device Card
static lv_obj_t *s_cont_empty_state = NULL;
static lv_obj_t *s_card_add_dev = NULL;

// Tab 2 (Môi trường) Widgets - DHT11 Đối xứng (Font 32px Lớn Căn Giữa)
static lv_obj_t *s_card_temp = NULL;
static lv_obj_t *s_lbl_temp_val = NULL;

static lv_obj_t *s_card_hum = NULL;
static lv_obj_t *s_lbl_hum_val = NULL;

// Tab 2: Warning Toast Widget
static lv_obj_t *s_toast_env = NULL;
static lv_timer_t *s_toast_timer = NULL;

// Tab 3 (Hệ thống) Widgets
static lv_obj_t *s_lbl_sys_ip = NULL;
static lv_obj_t *s_lbl_sys_cloud = NULL;
static lv_obj_t *s_lbl_sys_mesh = NULL;

// Commissioning Modal Widgets (Phê duyệt thiết bị vào mạng)
static lv_obj_t *s_modal_backdrop = NULL;
static lv_obj_t *s_modal_box = NULL;
static lv_obj_t *s_lbl_modal_title = NULL;
static lv_obj_t *s_lbl_modal_countdown = NULL;
static lv_obj_t *s_lbl_modal_sub = NULL;
static lv_obj_t *s_spinner_scan = NULL;
static lv_obj_t *s_cont_discovered = NULL;
static lv_obj_t *s_icon_discovered = NULL;
static lv_obj_t *s_lbl_disc_name = NULL;
static lv_obj_t *s_lbl_disc_info = NULL;
static lv_obj_t *s_btn_approve = NULL;
static lv_obj_t *s_btn_modal_cancel = NULL;
static lv_obj_t *s_lbl_modal_cancel = NULL;
static lv_timer_t *s_commissioning_timer = NULL;
static int s_countdown_val = 60;
static bool s_waiting_pairing = false;
static char s_discovered_id_str[32] = {0};

// Modal xác nhận gỡ bỏ thiết bị
static lv_obj_t *s_modal_del_backdrop = NULL;
static lv_obj_t *s_lbl_del_msg = NULL;
static char s_del_target_id[32] = {0};

// Trạng thái cục bộ phản hồi cảm ứng tức thì
static bool s_relay_registered_local = false;
static bool s_dimmer_registered_local = false;
static bool s_sensor_registered_local = false;
static bool s_relay_online_local = false;
static bool s_dimmer_online_local = false;
static bool s_sensor_online_local = false;
static bool s_relay_state_local = false;
static bool s_dimmer_state_local = false;
static uint8_t s_dimmer_bri_local = 80;
static bool s_sensor_valid_local = false;

// Cờ chống vòng lặp phản hồi sự kiện (loopback suppression)
static bool s_suppress_events = false;

// =============================================================================
// CẬP NHẬT GIAO DIỆN TRỰC QUAN THIẾT BỊ
// =============================================================================

static void update_relay_ui(bool is_online, bool is_on) {
    if (!s_card_relay) return;
    if (!is_online) {
        // Mờ xám khi Offline
        lv_obj_set_style_opa(s_card_relay, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(s_card_relay, lv_color_hex(0xF3F4F6), 0);
        lv_obj_set_style_border_color(s_card_relay, lv_color_hex(0xD1D5DB), 0);
        lv_obj_set_style_border_width(s_card_relay, 1, 0);

        lv_obj_set_style_bg_color(s_icon_relay_btn, lv_color_hex(0xE5E7EB), 0);
        lv_obj_set_style_text_color(s_icon_relay, lv_color_hex(0x9CA3AF), 0);

        lv_label_set_text(s_lbl_relay_state, "Offline");
        lv_obj_set_style_text_color(s_lbl_relay_state, lv_color_hex(0x9CA3AF), 0);
    } else {
        lv_obj_set_style_opa(s_card_relay, LV_OPA_COVER, 0);
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
}

static void update_dimmer_ui(bool is_online, bool is_on, uint8_t brightness) {
    if (!s_card_dimmer) return;
    if (!is_online) {
        // Mờ xám khi Offline
        lv_obj_set_style_opa(s_card_dimmer, LV_OPA_60, 0);
        lv_obj_set_style_bg_color(s_card_dimmer, lv_color_hex(0xF3F4F6), 0);
        lv_obj_set_style_border_color(s_card_dimmer, lv_color_hex(0xD1D5DB), 0);
        lv_obj_set_style_border_width(s_card_dimmer, 1, 0);

        lv_obj_set_style_bg_color(s_icon_dimmer_btn, lv_color_hex(0xE5E7EB), 0);
        lv_obj_set_style_bg_color(s_bulb_head, lv_color_hex(0xD1D5DB), 0);
        lv_obj_set_style_bg_color(s_bulb_base, lv_color_hex(0x9CA3AF), 0);

        lv_label_set_text(s_lbl_dimmer_val, "Offline");
        lv_obj_set_style_text_color(s_lbl_dimmer_val, lv_color_hex(0x9CA3AF), 0);
    } else {
        lv_obj_set_style_opa(s_card_dimmer, LV_OPA_COVER, 0);
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
    }
    lv_slider_set_value(s_slider_dimmer, brightness, LV_ANIM_OFF);
}

// =============================================================================
// COMMISSIONING MODAL (QUY TRÌNH QUÉT & PHÊ DUYỆT THIẾT BỊ)
// =============================================================================

static void commissioning_timer_cb(lv_timer_t *tmr) {
    (void)tmr;
    s_countdown_val--;
    if (s_countdown_val <= 0) {
        thread_commissioner_stop();
        if (s_lbl_modal_countdown) {
            lv_label_set_text(s_lbl_modal_countdown, "Hết thời gian quét");
        }
        if (s_spinner_scan) {
            lv_obj_add_flag(s_spinner_scan, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_commissioning_timer) {
            lv_timer_del(s_commissioning_timer);
            s_commissioning_timer = NULL;
        }
    } else {
        if (s_lbl_modal_countdown) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Đang tìm thiết bị... %ds", s_countdown_val);
            lv_label_set_text(s_lbl_modal_countdown, buf);
        }
    }
}

static void close_commissioning_modal(void) {
    s_waiting_pairing = false;
    if (s_commissioning_timer) {
        lv_timer_del(s_commissioning_timer);
        s_commissioning_timer = NULL;
    }
    thread_commissioner_stop();
    if (s_modal_backdrop) {
        lv_obj_add_flag(s_modal_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_lbl_modal_title) {
        lv_obj_set_style_text_color(s_lbl_modal_title, UI_COLOR_TEXT_MAIN, 0);
    }
    if (s_btn_modal_cancel) {
        lv_obj_set_style_bg_color(s_btn_modal_cancel, lv_color_hex(0xE5E5EA), 0);
    }
    if (s_lbl_modal_cancel) {
        lv_label_set_text(s_lbl_modal_cancel, "Hủy");
        lv_obj_set_style_text_color(s_lbl_modal_cancel, UI_COLOR_TEXT_MAIN, 0);
    }
    ESP_LOGI(TAG, "UI: Đóng modal Commissioner");
}

static void open_commissioning_modal(void) {
    if (!s_modal_backdrop) return;

    s_waiting_pairing = false;
    s_countdown_val = 60;
    lv_label_set_text(s_lbl_modal_title, "Thêm thiết bị Thread");
    lv_obj_set_style_text_color(s_lbl_modal_title, UI_COLOR_TEXT_MAIN, 0);

    lv_label_set_text(s_lbl_modal_countdown, "Đang tìm thiết bị... 60s");
    lv_label_set_text(s_lbl_modal_sub, "Nhấn giữ nút cấu hình trên thiết bị...");
    lv_obj_set_style_text_color(s_lbl_modal_sub, UI_COLOR_TEXT_MUTED, 0);

    lv_obj_clear_flag(s_spinner_scan, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lbl_modal_countdown, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lbl_modal_sub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cont_discovered, LV_OBJ_FLAG_HIDDEN);

    if (s_btn_modal_cancel) {
        lv_obj_clear_flag(s_btn_modal_cancel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_btn_modal_cancel, lv_color_hex(0xE5E5EA), 0);
    }
    if (s_lbl_modal_cancel) {
        lv_label_set_text(s_lbl_modal_cancel, "Hủy");
        lv_obj_set_style_text_color(s_lbl_modal_cancel, UI_COLOR_TEXT_MAIN, 0);
    }

    lv_obj_clear_flag(s_modal_backdrop, LV_OBJ_FLAG_HIDDEN);

    if (s_commissioning_timer) {
        lv_timer_del(s_commissioning_timer);
    }
    s_commissioning_timer = lv_timer_create(commissioning_timer_cb, 1000, NULL);

    thread_commissioner_start_scan(60);
    ESP_LOGI(TAG, "UI: Bật Commissioner tìm kiếm thiết bị Thread trong 60s");
}

static void btn_modal_cancel_event_cb(lv_event_t *e) {
    (void)e;
    close_commissioning_modal();
    gateway_device_state_t cur_st;
    gateway_state_get(&cur_st);
    ui_screen_dashboard_update_state(&cur_st);
}

void ui_screen_dashboard_on_pairing_result(bool success) {
    if (!s_modal_backdrop || lv_obj_has_flag(s_modal_backdrop, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    s_waiting_pairing = false;
    if (s_commissioning_timer) {
        lv_timer_del(s_commissioning_timer);
        s_commissioning_timer = NULL;
    }
    thread_commissioner_stop();

    if (s_spinner_scan) lv_obj_add_flag(s_spinner_scan, LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_modal_countdown) lv_obj_add_flag(s_lbl_modal_countdown, LV_OBJ_FLAG_HIDDEN);
    if (s_cont_discovered) lv_obj_add_flag(s_cont_discovered, LV_OBJ_FLAG_HIDDEN);
    if (s_lbl_modal_sub) lv_obj_clear_flag(s_lbl_modal_sub, LV_OBJ_FLAG_HIDDEN);

    if (success) {
        if (strlen(s_discovered_id_str) > 0) {
            gateway_state_set_device_registered(s_discovered_id_str, true);
        }
        if (s_lbl_modal_title) {
            lv_label_set_text(s_lbl_modal_title, "Thêm thiết bị thành công!");
            lv_obj_set_style_text_color(s_lbl_modal_title, UI_COLOR_ACCENT_GREEN, 0);
        }
        if (s_lbl_modal_sub) {
            lv_label_set_text(s_lbl_modal_sub, "Thiết bị đã kết nối an toàn vào mạng Thread.");
            lv_obj_set_style_text_color(s_lbl_modal_sub, UI_COLOR_TEXT_MAIN, 0);
        }
        if (s_btn_modal_cancel) {
            lv_obj_clear_flag(s_btn_modal_cancel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_btn_modal_cancel, UI_COLOR_ACCENT_GREEN, 0);
        }
        if (s_lbl_modal_cancel) {
            lv_label_set_text(s_lbl_modal_cancel, "OK");
            lv_obj_set_style_text_color(s_lbl_modal_cancel, lv_color_white(), 0);
        }
        ESP_LOGI(TAG, "UI: Ghép nối thiết bị THÀNH CÔNG! Chờ người dùng ấn [OK]");
    } else {
        if (s_lbl_modal_title) {
            lv_label_set_text(s_lbl_modal_title, "Thêm thiết bị thất bại");
            lv_obj_set_style_text_color(s_lbl_modal_title, UI_COLOR_ACCENT_RED, 0);
        }
        if (s_lbl_modal_sub) {
            lv_label_set_text(s_lbl_modal_sub, "Không nhận được phản hồi từ thiết bị.");
            lv_obj_set_style_text_color(s_lbl_modal_sub, UI_COLOR_TEXT_MUTED, 0);
        }
        if (s_btn_modal_cancel) {
            lv_obj_clear_flag(s_btn_modal_cancel, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_btn_modal_cancel, lv_color_hex(0xE5E5EA), 0);
        }
        if (s_lbl_modal_cancel) {
            lv_label_set_text(s_lbl_modal_cancel, "Đóng");
            lv_obj_set_style_text_color(s_lbl_modal_cancel, UI_COLOR_TEXT_MAIN, 0);
        }
        ESP_LOGW(TAG, "UI: Ghép nối thiết bị THẤT BẠI hoặc HẾT HẠN");
    }
}

static void commissioning_pairing_timeout_cb(lv_timer_t *tmr) {
    (void)tmr;
    ui_screen_dashboard_on_pairing_result(false);
}

static void btn_approve_event_cb(lv_event_t *e) {
    (void)e;
    if (strlen(s_discovered_id_str) == 0) return;

    ESP_LOGI(TAG, "UI: Người dùng chấp nhận thiết bị: '%s'", s_discovered_id_str);
    thread_commissioner_approve_device(s_discovered_id_str);

    s_waiting_pairing = true;
    lv_label_set_text(s_lbl_modal_title, "Đang kết nối thiết bị...");
    lv_obj_set_style_text_color(s_lbl_modal_title, UI_COLOR_TEXT_MAIN, 0);

    lv_label_set_text(s_lbl_modal_countdown, "Đang đồng bộ mạng & cấu hình bảo mật...");
    lv_label_set_text(s_lbl_modal_sub, "Vui lòng giữ thiết bị gần Gateway...");

    lv_obj_clear_flag(s_spinner_scan, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lbl_modal_countdown, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_lbl_modal_sub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cont_discovered, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_btn_modal_cancel, LV_OBJ_FLAG_HIDDEN);

    if (s_commissioning_timer) {
        lv_timer_del(s_commissioning_timer);
        s_commissioning_timer = NULL;
    }
    s_commissioning_timer = lv_timer_create(commissioning_pairing_timeout_cb, 25000, NULL);
}

void ui_screen_dashboard_on_device_discovered(commissioner_dev_type_t type, const char *name, const char *id_str, int8_t rssi) {
    if (!s_modal_backdrop || !s_cont_discovered) return;

    strncpy(s_discovered_id_str, id_str ? id_str : "", sizeof(s_discovered_id_str) - 1);

    // Ẩn spinner và các hướng dẫn quét
    lv_obj_add_flag(s_spinner_scan, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_modal_countdown, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lbl_modal_sub, LV_OBJ_FLAG_HIDDEN);

    // Trích xuất 4 ký tự cuối của EUI-64 / MAC để làm mã nhận diện phần cứng độc nhất
    char short_id[8] = "0000";
    size_t id_len = id_str ? strlen(id_str) : 0;
    if (id_len >= 4) {
        for (int i = 0; i < 4; i++) {
            char c = id_str[id_len - 4 + i];
            short_id[i] = (c >= 'a' && c <= 'z') ? (c - 'a' + 'A') : c;
        }
        short_id[4] = '\0';
    }

    char title_buf[48];
    char info_buf[64];
    const char *icon_symbol = LV_SYMBOL_SETTINGS;

    if (type == COMMISSIONER_DEV_RELAY) {
        icon_symbol = LV_SYMBOL_POWER;
        snprintf(title_buf, sizeof(title_buf), "Công tắc [Relay-%s]", short_id);
        snprintf(info_buf, sizeof(info_buf), "Mạng: OpenThread-PTIT • %ddBm", rssi);
    } else if (type == COMMISSIONER_DEV_DIMMER) {
        icon_symbol = LV_SYMBOL_EYE_OPEN;
        snprintf(title_buf, sizeof(title_buf), "Đèn [Dimmer-%s]", short_id);
        snprintf(info_buf, sizeof(info_buf), "Mạng: OpenThread-PTIT • %ddBm", rssi);
    } else if (type == COMMISSIONER_DEV_SENSOR) {
        icon_symbol = LV_SYMBOL_TINT;
        snprintf(title_buf, sizeof(title_buf), "Cảm biến [Sensor-%s]", short_id);
        snprintf(info_buf, sizeof(info_buf), "Mạng: OpenThread-PTIT • %ddBm", rssi);
    } else {
        snprintf(title_buf, sizeof(title_buf), "Thiết bị mới [MAC ...%s]", short_id);
        snprintf(info_buf, sizeof(info_buf), "Mạng: OpenThread-PTIT • %ddBm", rssi);
    }

    lv_label_set_text(s_icon_discovered, icon_symbol);
    lv_label_set_text(s_lbl_disc_name, title_buf);
    lv_label_set_text(s_lbl_disc_info, info_buf);

    lv_label_set_text(s_lbl_modal_title, "Phát hiện thiết bị mới!");
    lv_obj_clear_flag(s_cont_discovered, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "UI: Hiển thị thẻ phê duyệt thiết bị '%s' (%s)", title_buf, info_buf);
}

// =============================================================================
// TOAST CẢNH BÁO MÔI TRƯỜNG CHƯA KẾT NỐI
// =============================================================================

static void toast_auto_close_cb(lv_timer_t *tmr) {
    if (s_toast_env) {
        lv_obj_add_flag(s_toast_env, LV_OBJ_FLAG_HIDDEN);
    }
    s_toast_timer = NULL;
    lv_timer_del(tmr);
}

static void show_sensor_warning_toast(void) {
    if (!s_toast_env) return;
    lv_obj_clear_flag(s_toast_env, LV_OBJ_FLAG_HIDDEN);
    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
    }
    s_toast_timer = lv_timer_create(toast_auto_close_cb, 2000, NULL);
    ESP_LOGI(TAG, "UI: Hiển thị cảnh báo chưa kết nối cảm biến DHT11");
}

static void show_delete_confirm_modal(const char *target_id, const char *dev_name) {
    if (!target_id) return;
    strncpy(s_del_target_id, target_id, sizeof(s_del_target_id) - 1);
    if (!s_modal_del_backdrop || !s_lbl_del_msg) return;
    char msg[128];
    snprintf(msg, sizeof(msg), "Bạn có chắc muốn xóa\n%s khỏi mạng Thread?", dev_name);
    lv_label_set_text(s_lbl_del_msg, msg);
    lv_obj_clear_flag(s_modal_del_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void card_sensor_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        if (s_sensor_registered_local) {
            show_delete_confirm_modal("sensor-01", "Cảm biến DHT11");
        } else {
            show_sensor_warning_toast();
        }
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (!s_sensor_valid_local) {
            show_sensor_warning_toast();
        }
    }
}

static void tabview_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t *tv = lv_event_get_target(e);
        uint16_t act_tab = lv_tabview_get_tab_act(tv);
        if (act_tab == 1) { // Chuyển sang Tab 2: Môi trường
            if (!s_sensor_valid_local) {
                show_sensor_warning_toast();
            }
        }
    }
}

// =============================================================================
// SỰ KIỆN ĐIỀU KHIỂN THIẾT BỊ
// =============================================================================

static void btn_add_dev_event_cb(lv_event_t *e) {
    open_commissioning_modal();
}

static void btn_del_confirm_cb(lv_event_t *e) {
    (void)e;
    if (strlen(s_del_target_id) > 0) {
        ESP_LOGI(TAG, "UI: User confirmed delete device: %s", s_del_target_id);
        thread_gateway_remove_device(s_del_target_id);
        if (strstr(s_del_target_id, "relay") != NULL) {
            gateway_state_set_device_registered("relay", false);
            gateway_state_set_node_presence(false, gateway_state_is_dimmer_online(), gateway_state_is_sensor_online());
        } else if (strstr(s_del_target_id, "dimmer") != NULL) {
            gateway_state_set_device_registered("dimmer", false);
            gateway_state_set_node_presence(gateway_state_is_relay_online(), false, gateway_state_is_sensor_online());
        } else if (strstr(s_del_target_id, "sensor") != NULL) {
            gateway_state_set_device_registered("sensor", false);
            gateway_state_set_node_presence(gateway_state_is_relay_online(), gateway_state_is_dimmer_online(), false);
        }
        // Cập nhật lại giao diện ngay lập tức
        gateway_device_state_t cur_st;
        gateway_state_get(&cur_st);
        ui_screen_dashboard_update_state(&cur_st);
    }
    if (s_modal_del_backdrop) {
        lv_obj_add_flag(s_modal_del_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
}

static void btn_del_cancel_cb(lv_event_t *e) {
    (void)e;
    if (s_modal_del_backdrop) {
        lv_obj_add_flag(s_modal_del_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
}

static void relay_toggle_event_cb(lv_event_t *e) {
    if (s_suppress_events) return;
    s_relay_state_local = !s_relay_state_local;
    ESP_LOGI(TAG, "Touch UI: Công tắc -> %s", s_relay_state_local ? "ON" : "OFF");
    update_relay_ui(s_relay_online_local, s_relay_state_local);
    mesh_controller_set_relay(s_relay_state_local);
}

static void card_relay_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        show_delete_confirm_modal("relay-01", "Công tắc Relay");
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (!s_relay_online_local) {
            ESP_LOGW(TAG, "Touch UI: Relay đang Offline, chặn thao tác chạm");
            return;
        }
        relay_toggle_event_cb(e);
    }
}

static void dimmer_toggle_event_cb(lv_event_t *e) {
    if (s_suppress_events) return;
    s_dimmer_state_local = !s_dimmer_state_local;
    if (s_dimmer_state_local) {
        gateway_device_state_t st;
        gateway_state_get(&st);
        uint8_t target_bri = st.dimmer_last_brightness > 0 ? st.dimmer_last_brightness : 80;
        s_dimmer_bri_local = target_bri;
        ESP_LOGI(TAG, "Touch UI: Đèn -> ON (%u%%)", target_bri);
        update_dimmer_ui(s_dimmer_online_local, true, target_bri);
        mesh_controller_set_dimmer(true, target_bri);
    } else {
        ESP_LOGI(TAG, "Touch UI: Đèn -> OFF (0%%)");
        update_dimmer_ui(s_dimmer_online_local, false, 0);
        mesh_controller_set_dimmer(false, 0);
    }
}

static void card_dimmer_event_cb(lv_event_t *e) {
    lv_obj_t *target = lv_event_get_target(e);
    // Bỏ qua nếu thao tác trên Slider hoặc các thành phần của Slider
    if (target == s_slider_dimmer || lv_obj_get_parent(target) == s_slider_dimmer) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        show_delete_confirm_modal("dimmer-01", "Đèn chiếu sáng");
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        if (!s_dimmer_online_local) {
            ESP_LOGW(TAG, "Touch UI: Dimmer đang Offline, chặn thao tác chạm");
            return;
        }
        dimmer_toggle_event_cb(e);
    }
}

static void dimmer_slider_event_cb(lv_event_t *e) {
    if (s_suppress_events) return;
    if (!s_dimmer_online_local) {
        // Khóa slider khi Dimmer offline
        return;
    }
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t val = lv_slider_get_value(slider);
    bool is_on = (val > 0);

    if (code == LV_EVENT_VALUE_CHANGED) {
        // Cập nhật giao diện mượt mà theo ngón tay (không gửi mạng)
        s_dimmer_state_local = is_on;
        if (is_on) {
            s_dimmer_bri_local = (uint8_t)val;
        }
        update_dimmer_ui(s_dimmer_online_local, is_on, (uint8_t)val);
    } else if (code == LV_EVENT_RELEASED) {
        // Chỉ gửi lệnh CoAP Mesh & ThingsBoard MQTT một lần duy nhất khi nhấc ngón tay!
        ESP_LOGI(TAG, "Touch UI: Thả thanh trượt Đèn -> %u%%", (unsigned)val);
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
        if (tab_idx == 1 && !s_sensor_valid_local) {
            show_sensor_warning_toast();
        }
    }
}

// =============================================================================
// KHỞI TẠO MÀN HÌNH DASHBOARD
// =============================================================================

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

    // Header Left: Wi-Fi container
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
    lv_obj_add_flag(s_slash_header_wifi, LV_OBJ_FLAG_HIDDEN);

    s_lbl_header_wifi_dbm = lv_label_create(s_cont_header_wifi);
    lv_obj_align(s_lbl_header_wifi_dbm, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_text_color(s_lbl_header_wifi_dbm, UI_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_text_font(s_lbl_header_wifi_dbm, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_lbl_header_wifi_dbm, "(-65dBm)");

    // Header Center: Digital Clock
    s_lbl_header_time = lv_label_create(header);
    lv_obj_align(s_lbl_header_time, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(s_lbl_header_time, UI_COLOR_TEXT_MAIN, 0);
    lv_obj_set_style_text_font(s_lbl_header_time, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_lbl_header_time, "--:--:--");

    // Header Right: Mesh Nodes Count
    s_lbl_header_mesh = lv_label_create(header);
    lv_obj_align(s_lbl_header_mesh, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_text_color(s_lbl_header_mesh, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_lbl_header_mesh, &ui_font_vn_14, 0);
    lv_label_set_text(s_lbl_header_mesh, "Mạng: 0 nút");

    // =========================================================================
    // 2. TAB VIEW (Chiều cao 204px: Y=36 đến Y=240)
    // =========================================================================
    s_tabview = lv_tabview_create(s_screen_dash, LV_DIR_BOTTOM, 36);
    lv_obj_set_size(s_tabview, UI_LCD_H_RES, UI_LCD_V_RES - 36);
    lv_obj_align(s_tabview, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_tabview, UI_COLOR_BG, 0);
    lv_obj_add_event_cb(s_tabview, tabview_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *tab_content = lv_tabview_get_content(s_tabview);
    lv_obj_clear_flag(tab_content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tab_btns = lv_tabview_get_tab_btns(s_tabview);
    lv_obj_set_style_text_font(tab_btns, &ui_font_vn_14, 0);
    lv_obj_set_style_bg_color(tab_btns, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(tab_btns, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(tab_btns, 1, 0);
    lv_obj_set_style_border_side(tab_btns, LV_BORDER_SIDE_TOP, 0);

    s_tab_devices       = lv_tabview_add_tab(s_tabview, "Thiết bị");
    lv_obj_t *tab_env   = lv_tabview_add_tab(s_tabview, "Môi trường");
    lv_obj_t *tab_system = lv_tabview_add_tab(s_tabview, "Hệ thống");

    lv_obj_set_style_pad_all(s_tab_devices, 8, 0);
    lv_obj_set_style_pad_all(tab_env, 8, 0);
    lv_obj_set_style_pad_all(tab_system, 8, 0);

    // -------------------------------------------------------------------------
    // TAB 1: THIẾT BỊ
    // -------------------------------------------------------------------------
    lv_obj_set_scroll_dir(s_tab_devices, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_tab_devices, LV_SCROLLBAR_MODE_AUTO);

    // ==================== Empty State Container (Khi chưa có thiết bị) ====================
    s_cont_empty_state = lv_obj_create(s_tab_devices);
    lv_obj_set_size(s_cont_empty_state, 280, 150);
    lv_obj_align(s_cont_empty_state, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_cont_empty_state, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont_empty_state, 0, 0);
    lv_obj_set_style_pad_all(s_cont_empty_state, 0, 0);
    lv_obj_clear_flag(s_cont_empty_state, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_home = lv_label_create(s_cont_empty_state);
    lv_obj_align(icon_home, LV_ALIGN_CENTER, 0, -32);
    lv_obj_set_style_text_font(icon_home, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(icon_home, UI_COLOR_ACCENT_BLUE, 0);
    lv_label_set_text(icon_home, LV_SYMBOL_HOME);

    lv_obj_t *lbl_empty_msg = lv_label_create(s_cont_empty_state);
    lv_obj_align(lbl_empty_msg, LV_ALIGN_CENTER, 0, 5);
    lv_obj_set_style_text_font(lbl_empty_msg, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_empty_msg, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(lbl_empty_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_empty_msg, "Chưa có thiết bị trong mạng Thread");

    lv_obj_t *btn_empty_add = lv_btn_create(s_cont_empty_state);
    lv_obj_set_size(btn_empty_add, 160, 36);
    lv_obj_align(btn_empty_add, LV_ALIGN_CENTER, 0, 42);
    lv_obj_set_style_bg_color(btn_empty_add, UI_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(btn_empty_add, UI_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(btn_empty_add, btn_add_dev_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_btn_empty = lv_label_create(btn_empty_add);
    lv_obj_center(lbl_btn_empty);
    lv_obj_set_style_text_font(lbl_btn_empty, &ui_font_vn_14, 0);
    lv_label_set_text(lbl_btn_empty, "+ Thêm thiết bị");

    // ==================== Card 1: Công tắc (Trái) ====================
    s_card_relay = lv_obj_create(s_tab_devices);
    lv_obj_set_size(s_card_relay, 144, 135);
    lv_obj_align(s_card_relay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_card_relay, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_card_relay, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_card_relay, 1, 0);
    lv_obj_set_style_radius(s_card_relay, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(s_card_relay, 10, 0);
    lv_obj_clear_flag(s_card_relay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card_relay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_card_relay, LV_OBJ_FLAG_HIDDEN); // Mặc định ẩn khi chưa có node
    lv_obj_add_event_cb(s_card_relay, card_relay_event_cb, LV_EVENT_ALL, NULL);

    s_icon_relay_btn = lv_obj_create(s_card_relay);
    lv_obj_set_size(s_icon_relay_btn, 44, 44);
    lv_obj_align(s_icon_relay_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(s_icon_relay_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_icon_relay_btn, lv_color_hex(0xF2F2F7), 0);
    lv_obj_set_style_border_width(s_icon_relay_btn, 0, 0);
    lv_obj_set_style_pad_all(s_icon_relay_btn, 0, 0);
    lv_obj_clear_flag(s_icon_relay_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_icon_relay_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_icon_relay_btn, card_relay_event_cb, LV_EVENT_ALL, NULL);

    s_icon_relay = lv_label_create(s_icon_relay_btn);
    lv_obj_center(s_icon_relay);
    lv_obj_set_style_text_font(s_icon_relay, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_icon_relay, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_icon_relay, LV_SYMBOL_POWER);

    s_lbl_relay_state = lv_label_create(s_card_relay);
    lv_obj_align(s_lbl_relay_state, LV_ALIGN_TOP_RIGHT, 0, 12);
    lv_obj_set_style_text_font(s_lbl_relay_state, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_relay_state, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_lbl_relay_state, "Tắt");

    s_lbl_relay_title = lv_label_create(s_card_relay);
    lv_obj_align(s_lbl_relay_title, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_obj_set_style_text_font(s_lbl_relay_title, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_relay_title, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_relay_title, "Công tắc");

    // ==================== Card 2: Đèn chiếu sáng (Phải) ====================
    s_card_dimmer = lv_obj_create(s_tab_devices);
    lv_obj_set_size(s_card_dimmer, 144, 135);
    lv_obj_align(s_card_dimmer, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_card_dimmer, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_card_dimmer, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_card_dimmer, 1, 0);
    lv_obj_set_style_radius(s_card_dimmer, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(s_card_dimmer, 10, 0);
    lv_obj_clear_flag(s_card_dimmer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card_dimmer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_card_dimmer, LV_OBJ_FLAG_HIDDEN); // Mặc định ẩn khi chưa có node
    lv_obj_add_event_cb(s_card_dimmer, card_dimmer_event_cb, LV_EVENT_ALL, NULL);

    s_icon_dimmer_btn = lv_obj_create(s_card_dimmer);
    lv_obj_set_size(s_icon_dimmer_btn, 44, 44);
    lv_obj_align(s_icon_dimmer_btn, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(s_icon_dimmer_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_icon_dimmer_btn, lv_color_hex(0xF2F2F7), 0);
    lv_obj_set_style_border_width(s_icon_dimmer_btn, 0, 0);
    lv_obj_set_style_pad_all(s_icon_dimmer_btn, 0, 0);
    lv_obj_clear_flag(s_icon_dimmer_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_icon_dimmer_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_icon_dimmer_btn, card_dimmer_event_cb, LV_EVENT_ALL, NULL);

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

    s_lbl_dimmer_val = lv_label_create(s_card_dimmer);
    lv_obj_align(s_lbl_dimmer_val, LV_ALIGN_TOP_RIGHT, 0, 12);
    lv_obj_set_style_text_font(s_lbl_dimmer_val, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_dimmer_val, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_lbl_dimmer_val, "Tắt");

    s_lbl_dimmer_title = lv_label_create(s_card_dimmer);
    lv_obj_align(s_lbl_dimmer_title, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_obj_set_style_text_font(s_lbl_dimmer_title, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_dimmer_title, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_dimmer_title, "Đèn");

    s_slider_dimmer = lv_slider_create(s_card_dimmer);
    lv_obj_set_size(s_slider_dimmer, 116, 14);
    lv_obj_align(s_slider_dimmer, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_slider_set_range(s_slider_dimmer, 0, 100);
    lv_obj_set_style_pad_hor(s_slider_dimmer, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_dimmer, lv_color_hex(0xE5E5EA), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_dimmer, UI_COLOR_LIGHT_WARM, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_dimmer, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_border_color(s_slider_dimmer, UI_COLOR_LIGHT_WARM, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_slider_dimmer, 2, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider_dimmer, 6, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider_dimmer, dimmer_slider_event_cb, LV_EVENT_ALL, NULL);

    // ==================== Card 3: Thẻ '+ Thêm thiết bị' ====================
    s_card_add_dev = lv_obj_create(s_tab_devices);
    lv_obj_set_size(s_card_add_dev, 144, 120);
    lv_obj_align(s_card_add_dev, LV_ALIGN_TOP_LEFT, 0, 145);
    lv_obj_set_style_bg_color(s_card_add_dev, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_card_add_dev, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_card_add_dev, 1, 0);
    lv_obj_set_style_radius(s_card_add_dev, UI_RADIUS_CARD, 0);
    lv_obj_clear_flag(s_card_add_dev, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card_add_dev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_card_add_dev, LV_OBJ_FLAG_HIDDEN); // Mặc định ẩn trong Empty State
    lv_obj_add_event_cb(s_card_add_dev, btn_add_dev_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon_plus = lv_label_create(s_card_add_dev);
    lv_obj_align(icon_plus, LV_ALIGN_CENTER, 0, -12);
    lv_obj_set_style_text_font(icon_plus, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon_plus, UI_COLOR_ACCENT_BLUE, 0);
    lv_label_set_text(icon_plus, LV_SYMBOL_PLUS);

    lv_obj_t *lbl_add_title = lv_label_create(s_card_add_dev);
    lv_obj_align(lbl_add_title, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_text_font(lbl_add_title, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_add_title, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(lbl_add_title, "Thêm thiết bị");

    // -------------------------------------------------------------------------
    // TAB 2: MÔI TRƯỜNG (DHT11)
    // -------------------------------------------------------------------------
    s_card_temp = lv_obj_create(tab_env);
    lv_obj_set_size(s_card_temp, 144, 150);
    lv_obj_align(s_card_temp, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_card_temp, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_card_temp, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_card_temp, 1, 0);
    lv_obj_set_style_radius(s_card_temp, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(s_card_temp, 12, 0);
    lv_obj_clear_flag(s_card_temp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card_temp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_card_temp, card_sensor_event_cb, LV_EVENT_ALL, NULL);

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

    s_lbl_temp_val = lv_label_create(s_card_temp);
    lv_obj_align(s_lbl_temp_val, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_text_font(s_lbl_temp_val, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_temp_val, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_temp_val, "--.- °C");

    // Card Phải: Độ ẩm
    s_card_hum = lv_obj_create(tab_env);
    lv_obj_set_size(s_card_hum, 144, 150);
    lv_obj_align(s_card_hum, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(s_card_hum, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_card_hum, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_card_hum, 1, 0);
    lv_obj_set_style_radius(s_card_hum, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(s_card_hum, 12, 0);
    lv_obj_clear_flag(s_card_hum, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card_hum, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_card_hum, card_sensor_event_cb, LV_EVENT_ALL, NULL);

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

    s_lbl_hum_val = lv_label_create(s_card_hum);
    lv_obj_align(s_lbl_hum_val, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_text_font(s_lbl_hum_val, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_hum_val, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_hum_val, "-- %");

    // Warning Toast cho Tab Môi trường
    s_toast_env = lv_obj_create(tab_env);
    lv_obj_set_size(s_toast_env, 260, 36);
    lv_obj_align(s_toast_env, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(s_toast_env, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_color(s_toast_env, UI_COLOR_ACCENT_ORANGE, 0);
    lv_obj_set_style_border_width(s_toast_env, 1, 0);
    lv_obj_set_style_radius(s_toast_env, UI_RADIUS_BUTTON, 0);
    lv_obj_set_style_pad_all(s_toast_env, 0, 0);
    lv_obj_add_flag(s_toast_env, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *lbl_toast = lv_label_create(s_toast_env);
    lv_obj_center(lbl_toast);
    lv_obj_set_style_text_font(lbl_toast, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_toast, UI_COLOR_ACCENT_ORANGE, 0);
    lv_label_set_text(lbl_toast, LV_SYMBOL_WARNING " Chưa kết nối cảm biến DHT11!");

    // -------------------------------------------------------------------------
    // TAB 3: HỆ THỐNG
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
    lv_obj_set_style_text_font(s_lbl_sys_ip, &ui_font_vn_14, 0);
    lv_label_set_text(s_lbl_sys_ip, "IP: 192.168.?.?");

    s_lbl_sys_cloud = lv_label_create(card_sys);
    lv_obj_align(s_lbl_sys_cloud, LV_ALIGN_TOP_LEFT, 4, 20);
    lv_obj_set_style_text_font(s_lbl_sys_cloud, &ui_font_vn_14, 0);
    lv_label_set_text(s_lbl_sys_cloud, "ThingsBoard: Đã kết nối");

    s_lbl_sys_mesh = lv_label_create(card_sys);
    lv_obj_align(s_lbl_sys_mesh, LV_ALIGN_TOP_LEFT, 4, 40);
    lv_obj_set_style_text_font(s_lbl_sys_mesh, &ui_font_vn_14, 0);
    lv_label_set_text(s_lbl_sys_mesh, "Mạng Thread: 0 nút");

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

    // =========================================================================
    // 3. COMMISSIONING & APPROVAL MODAL DIALOG (Popup phê duyệt thiết bị)
    // =========================================================================
    s_modal_backdrop = lv_obj_create(s_screen_dash);
    lv_obj_set_size(s_modal_backdrop, UI_LCD_H_RES, UI_LCD_V_RES);
    lv_obj_center(s_modal_backdrop);
    lv_obj_set_style_bg_color(s_modal_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal_backdrop, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_modal_backdrop, 0, 0);
    lv_obj_set_style_pad_all(s_modal_backdrop, 0, 0);
    lv_obj_clear_flag(s_modal_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal_backdrop, LV_OBJ_FLAG_HIDDEN); // Mặc định ẩn

    s_modal_box = lv_obj_create(s_modal_backdrop);
    lv_obj_set_size(s_modal_box, 270, 195);
    lv_obj_center(s_modal_box);
    lv_obj_set_style_bg_color(s_modal_box, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(s_modal_box, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_modal_box, 1, 0);
    lv_obj_set_style_radius(s_modal_box, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(s_modal_box, 10, 0);
    lv_obj_clear_flag(s_modal_box, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_modal_title = lv_label_create(s_modal_box);
    lv_obj_align(s_lbl_modal_title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(s_lbl_modal_title, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_modal_title, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_modal_title, "Thêm thiết bị Thread");

    // Vùng Scanning (Spinner + Đếm ngược 60s)
    s_spinner_scan = lv_spinner_create(s_modal_box, 1000, 60);
    lv_obj_set_size(s_spinner_scan, 36, 36);
    lv_obj_align(s_spinner_scan, LV_ALIGN_CENTER, 0, -22);
    lv_obj_set_style_arc_color(s_spinner_scan, UI_COLOR_ACCENT_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spinner_scan, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spinner_scan, 3, LV_PART_MAIN);

    s_lbl_modal_countdown = lv_label_create(s_modal_box);
    lv_obj_align(s_lbl_modal_countdown, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_text_font(s_lbl_modal_countdown, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_modal_countdown, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_modal_countdown, "Đang tìm thiết bị... 60s");

    s_lbl_modal_sub = lv_label_create(s_modal_box);
    lv_obj_align(s_lbl_modal_sub, LV_ALIGN_CENTER, 0, 28);
    lv_obj_set_style_text_font(s_lbl_modal_sub, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_modal_sub, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_lbl_modal_sub, "Nhấn giữ nút cấu hình trên thiết bị...");

    // Vùng Discovered Device Card (Hiện khi phát hiện thiết bị chờ duyệt)
    s_cont_discovered = lv_obj_create(s_modal_box);
    lv_obj_set_size(s_cont_discovered, 250, 72);
    lv_obj_align(s_cont_discovered, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_color(s_cont_discovered, lv_color_hex(0xF9FAFB), 0);
    lv_obj_set_style_border_color(s_cont_discovered, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(s_cont_discovered, 1, 0);
    lv_obj_set_style_radius(s_cont_discovered, 10, 0);
    lv_obj_set_style_pad_all(s_cont_discovered, 6, 0);
    lv_obj_clear_flag(s_cont_discovered, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cont_discovered, LV_OBJ_FLAG_HIDDEN); // Mặc định ẩn

    s_icon_discovered = lv_label_create(s_cont_discovered);
    lv_obj_align(s_icon_discovered, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_font(s_icon_discovered, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_icon_discovered, UI_COLOR_ACCENT_BLUE, 0);
    lv_label_set_text(s_icon_discovered, LV_SYMBOL_SETTINGS);

    s_lbl_disc_name = lv_label_create(s_cont_discovered);
    lv_obj_align(s_lbl_disc_name, LV_ALIGN_TOP_LEFT, 36, 4);
    lv_obj_set_style_text_font(s_lbl_disc_name, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_disc_name, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_disc_name, "Thiết bị Thread");

    s_lbl_disc_info = lv_label_create(s_cont_discovered);
    lv_obj_align(s_lbl_disc_info, LV_ALIGN_TOP_LEFT, 36, 26);
    lv_obj_set_style_text_font(s_lbl_disc_info, &ui_font_vn_14, 0); // Dung font tieng Viet ui_font_vn_14 de hien thi chuan "Mạng:"
    lv_obj_set_style_text_color(s_lbl_disc_info, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_text(s_lbl_disc_info, "ID: ? • -45dBm");

    // Nút tròn tick xanh ( ✔ ) phê duyệt duy nhất (Không có nút đỏ)
    s_btn_approve = lv_btn_create(s_cont_discovered);
    lv_obj_set_size(s_btn_approve, 40, 40);
    lv_obj_align(s_btn_approve, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_radius(s_btn_approve, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_btn_approve, UI_COLOR_ACCENT_GREEN, 0);
    lv_obj_set_style_border_width(s_btn_approve, 0, 0);
    lv_obj_add_event_cb(s_btn_approve, btn_approve_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon_ok = lv_label_create(s_btn_approve);
    lv_obj_center(icon_ok);
    lv_obj_set_style_text_font(icon_ok, &lv_font_montserrat_16, 0);
    lv_label_set_text(icon_ok, LV_SYMBOL_OK);

    // Nút Hủy ở đáy modal
    s_btn_modal_cancel = lv_btn_create(s_modal_box);
    lv_obj_set_size(s_btn_modal_cancel, 110, 32);
    lv_obj_align(s_btn_modal_cancel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_btn_modal_cancel, lv_color_hex(0xE5E5EA), 0);
    lv_obj_set_style_radius(s_btn_modal_cancel, UI_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(s_btn_modal_cancel, btn_modal_cancel_event_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_modal_cancel = lv_label_create(s_btn_modal_cancel);
    lv_obj_center(s_lbl_modal_cancel);
    lv_obj_set_style_text_font(s_lbl_modal_cancel, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_modal_cancel, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(s_lbl_modal_cancel, "Hủy");

    // =========================================================================
    // 4. DEVICE DELETION CONFIRMATION MODAL (Hộp thoại xác nhận gỡ bỏ)
    // =========================================================================
    s_modal_del_backdrop = lv_obj_create(s_screen_dash);
    lv_obj_set_size(s_modal_del_backdrop, UI_LCD_H_RES, UI_LCD_V_RES);
    lv_obj_center(s_modal_del_backdrop);
    lv_obj_set_style_bg_color(s_modal_del_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal_del_backdrop, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_modal_del_backdrop, 0, 0);
    lv_obj_set_style_pad_all(s_modal_del_backdrop, 0, 0);
    lv_obj_clear_flag(s_modal_del_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_modal_del_backdrop, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *del_box = lv_obj_create(s_modal_del_backdrop);
    lv_obj_set_size(del_box, 260, 150);
    lv_obj_center(del_box);
    lv_obj_set_style_bg_color(del_box, UI_COLOR_CARD_BG, 0);
    lv_obj_set_style_border_color(del_box, UI_COLOR_CARD_BORDER, 0);
    lv_obj_set_style_border_width(del_box, 1, 0);
    lv_obj_set_style_radius(del_box, UI_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(del_box, 10, 0);
    lv_obj_clear_flag(del_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_del_hdr = lv_label_create(del_box);
    lv_obj_align(lbl_del_hdr, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(lbl_del_hdr, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_del_hdr, UI_COLOR_ACCENT_RED, 0);
    lv_label_set_text(lbl_del_hdr, "Gỡ bỏ thiết bị");

    s_lbl_del_msg = lv_label_create(del_box);
    lv_obj_align(s_lbl_del_msg, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_text_font(s_lbl_del_msg, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(s_lbl_del_msg, UI_COLOR_TEXT_MAIN, 0);
    lv_obj_set_style_text_align(s_lbl_del_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lbl_del_msg, "Bạn có chắc muốn xóa\nthiết bị khỏi mạng?");

    lv_obj_t *btn_del_ok = lv_btn_create(del_box);
    lv_obj_set_size(btn_del_ok, 105, 32);
    lv_obj_align(btn_del_ok, LV_ALIGN_BOTTOM_LEFT, 5, 0);
    lv_obj_set_style_bg_color(btn_del_ok, UI_COLOR_ACCENT_RED, 0);
    lv_obj_set_style_radius(btn_del_ok, UI_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(btn_del_ok, btn_del_confirm_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_del_ok = lv_label_create(btn_del_ok);
    lv_obj_center(lbl_del_ok);
    lv_obj_set_style_text_font(lbl_del_ok, &ui_font_vn_14, 0);
    lv_label_set_text(lbl_del_ok, "Xóa");

    lv_obj_t *btn_del_cancel = lv_btn_create(del_box);
    lv_obj_set_size(btn_del_cancel, 105, 32);
    lv_obj_align(btn_del_cancel, LV_ALIGN_BOTTOM_RIGHT, -5, 0);
    lv_obj_set_style_bg_color(btn_del_cancel, lv_color_hex(0xE5E5EA), 0);
    lv_obj_set_style_radius(btn_del_cancel, UI_RADIUS_BUTTON, 0);
    lv_obj_add_event_cb(btn_del_cancel, btn_del_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_del_cancel = lv_label_create(btn_del_cancel);
    lv_obj_center(lbl_del_cancel);
    lv_obj_set_style_text_font(lbl_del_cancel, &ui_font_vn_14, 0);
    lv_obj_set_style_text_color(lbl_del_cancel, UI_COLOR_TEXT_MAIN, 0);
    lv_label_set_text(lbl_del_cancel, "Hủy");

    return s_screen_dash;
}

static void place_card_in_grid_slot(lv_obj_t *card, int slot) {
    if (!card) return;
    lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
    if (slot == 0) {
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 0);
    } else if (slot == 1) {
        lv_obj_align(card, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else if (slot == 2) {
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, 145);
    } else {
        lv_obj_align(card, LV_ALIGN_TOP_RIGHT, 0, 145);
    }
}

// =============================================================================
// CẬP NHẬT TRẠNG THÁI TOÀN BỘ DASHBOARD
// =============================================================================

void ui_screen_dashboard_update_state(const gateway_device_state_t *state) {
    if (!state || !s_screen_dash) return;
    s_suppress_events = true;

    s_relay_registered_local = state->relay_registered;
    s_dimmer_registered_local = state->dimmer_registered;
    s_sensor_registered_local = state->sensor_registered;

    s_relay_online_local = state->relay_node_online;
    s_dimmer_online_local = state->dimmer_node_online;
    s_sensor_online_local = state->sensor_node_online;

    s_relay_state_local = state->relay_on;
    s_dimmer_state_local = state->dimmer_on;
    s_dimmer_bri_local = state->dimmer_brightness;
    s_sensor_valid_local = (state->sensor_node_online && state->sensor_valid);

    // Nếu đang trong quá trình chờ ghép nối mà có thiết bị mới online -> kích hoạt thành công!
    if (s_waiting_pairing && (state->relay_node_online || state->dimmer_node_online || state->sensor_node_online)) {
        ui_screen_dashboard_on_pairing_result(true);
    }

    // 1. Quản lý hiển thị bố cục Tab Thiết bị (Dựa trên thiết bị đã đăng ký trong mạng)
    bool has_relay  = state->relay_registered;
    bool has_dimmer = state->dimmer_registered;

    if (!has_relay && !has_dimmer) {
        // Chưa có thiết bị nào -> Hiển thị Empty State
        lv_obj_clear_flag(s_cont_empty_state, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_card_relay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_card_dimmer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_card_add_dev, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cont_empty_state, LV_OBJ_FLAG_HIDDEN);

        int next_slot = 0;
        if (has_relay && has_dimmer) {
            place_card_in_grid_slot(s_card_relay, 0);
            place_card_in_grid_slot(s_card_dimmer, 1);
            next_slot = 2; // Thẻ [+ Thêm thiết bị] xếp ở dòng 2 bên Trái
        } else if (has_relay) {
            place_card_in_grid_slot(s_card_relay, 0);
            lv_obj_add_flag(s_card_dimmer, LV_OBJ_FLAG_HIDDEN);
            next_slot = 1; // Thẻ [+ Thêm thiết bị] xếp ở ô 1 (Phải)
        } else {
            place_card_in_grid_slot(s_card_dimmer, 0);
            lv_obj_add_flag(s_card_relay, LV_OBJ_FLAG_HIDDEN);
            next_slot = 1; // Thẻ [+ Thêm thiết bị] xếp ở ô 1 (Phải)
        }

        // Đặt thẻ [+ Thêm thiết bị] luôn ở vị trí trống tiếp theo
        place_card_in_grid_slot(s_card_add_dev, next_slot);

        update_relay_ui(state->relay_node_online, state->relay_on);
        update_dimmer_ui(state->dimmer_node_online, state->dimmer_on, state->dimmer_brightness);
    }

    // 2. Cập nhật Môi trường DHT11 (2 Thẻ Card đối xứng)
    if (state->sensor_registered && s_sensor_valid_local) {
        lv_obj_set_style_opa(s_card_temp, LV_OPA_COVER, 0);
        lv_obj_set_style_opa(s_card_hum, LV_OPA_COVER, 0);

        char t_buf[24], h_buf[24];
        snprintf(t_buf, sizeof(t_buf), "%.1f °C", state->temperature);
        snprintf(h_buf, sizeof(h_buf), "%.0f %%", state->humidity);
        lv_label_set_text(s_lbl_temp_val, t_buf);
        lv_label_set_text(s_lbl_hum_val, h_buf);
    } else {
        lv_obj_set_style_opa(s_card_temp, LV_OPA_60, 0);
        lv_obj_set_style_opa(s_card_hum, LV_OPA_60, 0);
        lv_label_set_text(s_lbl_temp_val, "--.- °C");
        lv_label_set_text(s_lbl_hum_val, "-- %");
    }

    // 3. Header & System Mesh Count (Định dạng chuẩn "Mạng: X/Y kết nối" hoặc "Mạng: 0 thiết bị")
    uint8_t total_reg = (has_relay ? 1 : 0) + (has_dimmer ? 1 : 0) + (state->sensor_registered ? 1 : 0);
    uint8_t total_online = (state->relay_node_online ? 1 : 0) + (state->dimmer_node_online ? 1 : 0) + (state->sensor_node_online ? 1 : 0);
    char mesh_buf[32];
    if (total_reg == 0) {
        snprintf(mesh_buf, sizeof(mesh_buf), "Mạng: 0 thiết bị");
    } else {
        snprintf(mesh_buf, sizeof(mesh_buf), "Mạng: %u/%u kết nối", total_online, total_reg);
    }
    lv_label_set_text(s_lbl_header_mesh, mesh_buf);
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
