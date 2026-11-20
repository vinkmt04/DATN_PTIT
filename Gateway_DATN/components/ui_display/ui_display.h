#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// CẤU HÌNH PHẦN CỨNG MÀN HÌNH ST7789V & CẢM ỨNG XPT2046 (ESP32-S3)
// =============================================================================
// Chế độ Cân chỉnh Cảm ứng:
// 0: Sử dụng thông số chuẩn 2.8" đã hiệu chỉnh + Bộ lọc Median 3 điểm (Khởi động chạy ngay)
// 1: Mở màn hình Chấm 4 góc (Calibration Screen) để đo mẫu ADC thực tế của tấm kính
#define UI_TOUCH_CALIBRATION_MODE       0

// Kích thước hiển thị (Chế độ Ngang - Landscape)
#define UI_LCD_H_RES                    320
#define UI_LCD_V_RES                    240

// Thời gian không thao tác để tự động về Màn hình Chờ (Idle Screen)
#define UI_IDLE_TIMEOUT_SECONDS         90

// Định nghĩa các chân LCD ST7789V (SPI2_HOST DMA) - Cụm Header J1 (Trái)
#define LCD_PIN_SCLK                    12
#define LCD_PIN_MOSI                    11
#define LCD_PIN_CS                      10
#define LCD_PIN_DC                      9
#define LCD_PIN_RST                     8
#define LCD_PIN_BL                      3   // Điều khiển PWM đèn nền (5kHz)

// Định nghĩa các chân Touch XPT2046 (SPI3_HOST)
#define TOUCH_PIN_CS                    2
#define TOUCH_PIN_IRQ                   48  // Ngắt chạm phần cứng (Active LOW) - Đổi từ 42 sang 48
#define TOUCH_PIN_DO                    47  // MISO từ XPT2046 - Đổi từ 41 sang 47
#define TOUCH_PIN_DIN                   40  // MOSI tới XPT2046
#define TOUCH_PIN_CLK                   39  // Xung nhịp SPI Touch (2MHz)

// =============================================================================
// CÁC HÀM GIAO TIẾP VỚI HỆ THỐNG (API)
// =============================================================================

// Khởi tạo toàn bộ hệ thống hiển thị: Driver ST7789V, Driver XPT2046, LVGL, và Tasks
void ui_display_init(void);

// Bật lớp phủ giọng nói Siri khi nghe 'Hey Cherry'
void ui_display_show_voice_overlay(void);

// Cập nhật biên độ sóng âm giọng nói cho Audio Visualizer (lấy từ Mic RMS hoặc PCM loa)
void ui_display_update_audio_rms(uint16_t rms_level);

// Báo nhận diện lệnh cục bộ thành công (Edge AI) để hiện Toast & gạt công tắc trên màn hình
void ui_display_notify_command_executed(const char *cmd_name, uint32_t latency_ms);

// Báo chuyển sang luồng trả lời Gemini Live API (Cloud AI)
void ui_display_notify_cloud_speaking(void);

// Tắt / Ẩn lớp phủ Siri Voice Overlay
void ui_display_hide_voice_overlay(void);

// Đăng ký callback khi người dùng chạm huỷ lượt trên màn hình (để ngắt ai_edge mà không bị circular dependency)
typedef void (*ui_display_abort_cb_t)(void);
void ui_display_set_abort_callback(ui_display_abort_cb_t cb);
void ui_display_trigger_abort(void);

#include "thread_commissioner.h"

// Lấy trạng thái màn hình hiện tại đang là Idle (true) hay Dashboard (false)
bool ui_display_is_idle(void);

// Báo thiết bị Thread mới phát hiện để hiện trên popup phê duyệt
void ui_display_notify_discovery(commissioner_dev_type_t type, const char *name, const char *id_str, int8_t rssi);

// Báo kết quả ghép nối thiết bị Thread (thành công/thất bại)
void ui_display_notify_pair_result(bool success, const char *id_str);

// Đánh thức màn hình từ Idle lên Dashboard (100% đèn nền)
void ui_display_wake_up(void);

#ifdef __cplusplus
}
#endif
