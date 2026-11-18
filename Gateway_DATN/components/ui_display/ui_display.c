#include "ui_display.h"
#include "st7789v_driver.h"
#include "xpt2046_driver.h"
#include "ui_theme.h"
#include "ui_screen_idle.h"
#include "ui_screen_dashboard.h"
#include "ui_voice_overlay.h"

#include "sys_core.h"
#include "iot_cloud.h"
#include "gateway_state_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <time.h>

static const char *TAG = "ui_display";

static SemaphoreHandle_t s_gui_mutex = NULL;
static QueueHandle_t s_ui_event_queue = NULL;

static lv_obj_t *s_scr_idle = NULL;
static lv_obj_t *s_scr_dash = NULL;

static volatile bool s_is_idle = true;
static int64_t s_last_interaction_us = 0;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static ui_display_abort_cb_t s_abort_cb = NULL;

// Sự kiện bất đồng bộ gửi tới GUI Task
typedef enum {
    UI_EVT_STATE_CHANGED,
    UI_EVT_VOICE_SHOW,
    UI_EVT_VOICE_HIDE,
    UI_EVT_VOICE_RMS,
    UI_EVT_COMMAND_DONE,
    UI_EVT_CLOUD_SPEAKING,
} ui_event_type_t;

typedef struct {
    ui_event_type_t type;
    union {
        gateway_device_state_t state;
        uint16_t rms;
        struct {
            char cmd_name[32];
            uint32_t latency;
        } cmd;
    } data;
} ui_event_msg_t;

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

static bool s_waking_up = false;

static void on_user_interaction(void) {
    s_last_interaction_us = esp_timer_get_time();
}

static void lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    int16_t tx = 0, ty = 0;
    bool pressed = xpt2046_read(&tx, &ty);
    if (pressed) {
        on_user_interaction();

        // Nếu màn hình đang ở chế độ chờ (Idle):
        // Cú chạm đầu tiên chỉ dùng để đánh thức và mở Tab Thiết bị, nuốt sự kiện để không ấn nhầm nút
        if (s_is_idle) {
            s_is_idle = false;
            s_waking_up = true;
            st7789v_set_backlight(100); // Bừng sáng màn hình 100%
            ui_screen_dashboard_select_tab(0); // Tự động chọn Tab Thiết bị đầu tiên
            lv_scr_load(s_scr_dash);
            ESP_LOGI(TAG, "Touch UI: Đánh thức từ Màn hình chờ -> Mở Tab Thiết bị (Nuốt sự kiện chạm)");
        }

        if (s_waking_up) {
            // Trong suốt thời gian ngón tay chưa nhấc ra từ cú chạm đánh thức, báo RELEASED để tránh click nhầm
            data->state = LV_INDEV_STATE_REL;
            return;
        }

        data->state = LV_INDEV_STATE_PR;
        data->point.x = tx;
        data->point.y = ty;
    } else {
        data->state = LV_INDEV_STATE_REL;
        s_waking_up = false; // Ngón tay đã nhấc ra, các lần chạm tiếp theo sẽ điều khiển Dashboard bình thường!
    }
}

static void lv_tick_task(void *arg) {
    (void)arg;
    lv_tick_inc(5); // 5ms tick
}

static void auto_hide_toast_cb(lv_timer_t *tmr) {
    ui_voice_overlay_hide();
    lv_timer_del(tmr);
}

static void on_gateway_state_callback(const gateway_device_state_t *new_state) {
    if (!s_ui_event_queue || !new_state) return;
    ui_event_msg_t msg;
    msg.type = UI_EVT_STATE_CHANGED;
    memcpy(&msg.data.state, new_state, sizeof(gateway_device_state_t));
    xQueueSend(s_ui_event_queue, &msg, 0);
}

static void ui_display_task(void *pvParameters) {
    ESP_LOGI(TAG, "GUI Main Task started on Core 0 (Priority 4)");
    gateway_state_register_callback(on_gateway_state_callback);

    s_last_interaction_us = esp_timer_get_time();
    int sec_counter = 0;

    while (1) {
        // 1. Nhận và xử lý các sự kiện bất đồng bộ từ các module khác
        ui_event_msg_t msg;
        while (xQueueReceive(s_ui_event_queue, &msg, 0) == pdTRUE) {
            if (xSemaphoreTake(s_gui_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                switch (msg.type) {
                    case UI_EVT_STATE_CHANGED:
                        ui_screen_dashboard_update_state(&msg.data.state);
                        ui_screen_idle_update_sensors(msg.data.state.temperature, msg.data.state.humidity, msg.data.state.sensor_valid);
                        break;
                    case UI_EVT_VOICE_SHOW:
                        st7789v_set_backlight(100);
                        ui_voice_overlay_show();
                        break;
                    case UI_EVT_VOICE_HIDE:
                        ui_voice_overlay_hide();
                        break;
                    case UI_EVT_VOICE_RMS:
                        ui_voice_overlay_set_rms(msg.data.rms);
                        break;
                    case UI_EVT_COMMAND_DONE: {
                        st7789v_set_backlight(100);
                        if (s_is_idle) {
                            s_is_idle = false;
                            lv_scr_load(s_scr_dash);
                        }
                        char toast_buf[64];
                        snprintf(toast_buf, sizeof(toast_buf), "Đã thực hiện • %lums (Cục bộ)", (unsigned long)msg.data.cmd.latency);
                        ui_voice_overlay_set_status(toast_buf, UI_COLOR_ACCENT_GREEN);
                        lv_timer_create(auto_hide_toast_cb, 2000, NULL);
                        s_last_interaction_us = esp_timer_get_time();
                        break;
                    }
                    case UI_EVT_CLOUD_SPEAKING:
                        st7789v_set_backlight(100);
                        ui_voice_overlay_set_status("Gemini Live đang trả lời...", UI_COLOR_SIRI_PURPLE);
                        break;
                }
                xSemaphoreGive(s_gui_mutex);
            }
        }

        // 2. Chạy LVGL handler
        if (xSemaphoreTake(s_gui_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_gui_mutex);
        }

        // 3. Chu kỳ định kỳ mỗi 1 giây: Cập nhật giờ SNTP, Wi-Fi và kiểm tra Idle Timeout
        sec_counter += 10;
        if (sec_counter >= 1000) {
            sec_counter = 0;

            time_t now;
            struct tm timeinfo;
            time(&now);
            localtime_r(&now, &timeinfo);

            char ip_str[24];
            sys_core_get_ip_str(ip_str, sizeof(ip_str));
            int8_t rssi = sys_core_get_wifi_rssi();
            bool wifi_ok = sys_core_is_wifi_connected();
            bool cloud_ok = iot_cloud_is_connected();

            if (xSemaphoreTake(s_gui_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // Cập nhật giờ và thông số mạng
                ui_screen_idle_update_time(&timeinfo);
                ui_screen_idle_update_status(wifi_ok, cloud_ok, 3);
                ui_screen_dashboard_update_time(&timeinfo);
                ui_screen_dashboard_update_network(wifi_ok, ip_str, rssi, cloud_ok);

                // Kiểm tra Idle Timeout: 30 giây không chạm và không có voice
                int64_t now_us = esp_timer_get_time();
                if (!s_is_idle && !ui_voice_overlay_is_visible()) {
                    if ((now_us - s_last_interaction_us) > ((int64_t)UI_IDLE_TIMEOUT_SECONDS * 1000000LL)) {
                        s_is_idle = true;
                        lv_scr_load(s_scr_idle);
                        st7789v_set_backlight(30); // Giảm đèn nền xuống 30% để tiết kiệm điện
                        ESP_LOGI(TAG, "UI: 30s Timeout -> Chuyen sang man hinh Cho (Idle Screen) & Dimming 30%%");
                    }
                }
                xSemaphoreGive(s_gui_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void ui_display_init(void) {
    ESP_LOGI(TAG, "Initializing UI Display Component (ST7789V 2.8\" + XPT2046 + LVGL v8)...");

    s_gui_mutex = xSemaphoreCreateMutex();
    s_ui_event_queue = xQueueCreate(16, sizeof(ui_event_msg_t));

    // 1. Khởi tạo LVGL
    lv_init();

    // Đăng ký Font Tiếng Việt có dấu làm fallback cho Montserrat 14
    ((lv_font_t *)&lv_font_montserrat_14)->fallback = &ui_font_vn_14;

    // 2. Khởi tạo phần cứng màn hình ST7789V
    esp_err_t ret = st7789v_driver_init(&s_panel_handle, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ST7789V driver!");
        return;
    }

    // 3. Khởi tạo cảm ứng XPT2046
    ret = xpt2046_driver_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize XPT2046 driver!");
    }

    // 4. Cấp phát Double Buffer cho LVGL (16 dòng = 10.2KB x 2 = 20.4KB, tiết kiệm tối đa Internal SRAM)
    size_t buf_size = UI_LCD_H_RES * 16 * sizeof(lv_color_t);
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate DMA buffers for LVGL! Falling back to 10 lines.");
        if (buf1) free(buf1);
        if (buf2) free(buf2);
        buf_size = UI_LCD_H_RES * 10 * sizeof(lv_color_t);
        buf1 = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        buf2 = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }

    static lv_disp_draw_buf_t disp_buf;
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_size / sizeof(lv_color_t));

    // Đăng ký Display Driver với LVGL
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = UI_LCD_H_RES;
    disp_drv.ver_res = UI_LCD_V_RES;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = s_panel_handle;
    lv_disp_drv_register(&disp_drv);

    // Đăng ký Touch Input Driver với LVGL
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    // 5. Cấu hình định thời 5ms cho LVGL Tick bằng esp_timer
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "lv_tick_timer"
    };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 5000)); // 5ms

    // 6. Xây dựng giao diện đồ họa (Apple Clean Modern Theme)
    s_scr_idle = ui_screen_idle_init();
    s_scr_dash = ui_screen_dashboard_init();
    ui_voice_overlay_init();

    // Mặc định nạp màn hình Idle và đặt độ sáng đèn nền
    lv_scr_load(s_scr_idle);
    st7789v_set_backlight(100); // Khởi động 100% trước, sau 30s tự giảm xuống 30%

    // 7. Tạo GUI Task trên Core 0 (độ ưu tiên 4)
    xTaskCreatePinnedToCore(ui_display_task, "ui_display_task", 6144, NULL, 4, NULL, 0);

    ESP_LOGI(TAG, "UI Display Component successfully initialized and running!");
}

void ui_display_show_voice_overlay(void) {
    if (!s_ui_event_queue) return;
    ui_event_msg_t msg = {.type = UI_EVT_VOICE_SHOW};
    xQueueSend(s_ui_event_queue, &msg, 0);
}

void ui_display_update_audio_rms(uint16_t rms_level) {
    if (!s_ui_event_queue) return;
    ui_event_msg_t msg = {.type = UI_EVT_VOICE_RMS, .data.rms = rms_level};
    xQueueSend(s_ui_event_queue, &msg, 0);
}

void ui_display_notify_command_executed(const char *cmd_name, uint32_t latency_ms) {
    if (!s_ui_event_queue) return;
    ui_event_msg_t msg;
    msg.type = UI_EVT_COMMAND_DONE;
    if (cmd_name) {
        snprintf(msg.data.cmd.cmd_name, sizeof(msg.data.cmd.cmd_name), "%s", cmd_name);
    } else {
        msg.data.cmd.cmd_name[0] = '\0';
    }
    msg.data.cmd.latency = latency_ms;
    xQueueSend(s_ui_event_queue, &msg, 0);
}

void ui_display_notify_cloud_speaking(void) {
    if (!s_ui_event_queue) return;
    ui_event_msg_t msg = {.type = UI_EVT_CLOUD_SPEAKING};
    xQueueSend(s_ui_event_queue, &msg, 0);
}

void ui_display_hide_voice_overlay(void) {
    if (!s_ui_event_queue) return;
    ui_event_msg_t msg = {.type = UI_EVT_VOICE_HIDE};
    xQueueSend(s_ui_event_queue, &msg, 0);
}

bool ui_display_is_idle(void) {
    return s_is_idle;
}

void ui_display_wake_up(void) {
    on_user_interaction();
}

void ui_display_set_abort_callback(ui_display_abort_cb_t cb) {
    s_abort_cb = cb;
}

void ui_display_trigger_abort(void) {
    if (s_abort_cb) {
        s_abort_cb();
    }
}
