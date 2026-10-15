#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo I2S cho cả Microphone và Speaker
void audio_hal_init(void);

// Đọc dữ liệu từ Microphone (blocking)
// Trả về số byte thực tế đã đọc
size_t audio_hal_record_chunk(int16_t *buffer, size_t buffer_size_bytes);

// Phát dữ liệu ra Speaker (blocking)
// Trả về số byte thực tế đã phát
size_t audio_hal_play_chunk(const uint8_t *buffer, size_t buffer_size_bytes);

// Kích hoạt chế độ Jitter Buffering khi bắt đầu một câu trả lời mới
void audio_hal_start_buffering(void);

// Báo trạng thái lượt nói của Cloud Model (true: AI đang nói, false: AI đã nói xong)
void audio_hal_set_turn_active(bool active);

// Phát âm thanh báo thức (wake sound) từ flash
void audio_hal_play_wake_sound(void);

// Ép thoát trạng thái buffering nếu có sự cố
void audio_hal_force_start_playing(void);

// Phát tiếng báo lỗi (lỗi mạng, timeout)
void audio_hal_play_error_sound(void);

// Dừng phát âm thanh ngay lập tức và xả sạch bộ đệm (Dùng cho Barge-In ngắt lời)
void audio_hal_stop_playback(void);

// VAD - Tính toán cường độ âm thanh (RMS) của một chunk
// Trả về true nếu âm thanh vượt ngưỡng (Có tiếng)
bool audio_vad_is_speech(const int16_t *buffer, size_t num_samples);

#ifdef __cplusplus
}
#endif
