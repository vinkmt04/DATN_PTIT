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

// Kiểm tra tiếng chuông báo thức có đang phát hay không
bool audio_hal_is_wake_sound_playing(void);

// Kiểm tra đã đến thời điểm bắt đầu thu âm hay chưa (100ms trước khi chuông Ding kết thúc)
bool audio_hal_is_wake_recording_ready(void);

// Ép thoát trạng thái buffering nếu có sự cố
void audio_hal_force_start_playing(void);

// Phát tiếng báo lỗi (lỗi mạng, timeout)
void audio_hal_play_error_sound(void);

// Dừng phát âm thanh ngay lập tức và xả sạch bộ đệm (Dùng cho Barge-In ngắt lời)
void audio_hal_stop_playback(void);

// Xả sạch bộ đệm phát TX đồng bộ an toàn
void audio_hal_flush_tx(void);

// Xả sạch bộ đệm thu RX DMA để loại bỏ âm thanh cũ/tiếng dội tích tụ
void audio_hal_flush_rx(void);

// Reset trigger VAD chuẩn Espressif về trạng thái Silence
void audio_vad_reset_trigger(void);

// Kiểm tra loa có đang phát âm thanh hay không (Dùng cho Echo Guard chống loa dội vào mic)
bool audio_hal_is_playing(void);

// Áp dụng bộ lọc Biquad High-Pass Filter bậc 2 (~180Hz) để triệt tiêu DC offset & tiếng ù quạt/rung chấn
void audio_hal_apply_dc_filter(int16_t *buffer, size_t num_samples);

// Reset trạng thái bộ lọc DC (Biquad Filter state w1, w2) để tránh sốc biên độ khi ngắt/chuyển trạng thái
void audio_hal_reset_dc_filter(void);

// VAD Phần Cứng Espressif (WebRTC GMM Mode 3 Very Aggressive) kết hợp cổng năng lượng âm thanh
// Trả về true nếu phát hiện giọng nói người thật sự, đồng thời gán RMS qua out_rms (nếu out_rms != NULL)
bool audio_vad_is_speech_chunk(int16_t *buffer, size_t num_samples, int *out_rms);

// Tính toán cường độ âm thanh (RMS) của một chunk
int audio_vad_get_rms(const int16_t *buffer, size_t num_samples);

// Thuật toán Thích Ứng Bất Đối Xứng (Asymmetric Ambient Noise Tracker) cập nhật mức nhiễu nền môi trường
void audio_dsp_update_ambient_noise(int current_rms);

// Lấy mức nhiễu nền môi trường ước lượng hiện tại (RMS)
int audio_dsp_get_ambient_noise(void);

// Tương thích ngược: Alias cho các hàm noise floor cũ
void audio_vad_update_noise_floor(int current_rms);
int audio_vad_get_noise_floor(void);

// Tương thích ngược
bool audio_vad_is_speech_adaptive(const int16_t *buffer, size_t num_samples, bool is_already_speaking);
bool audio_vad_is_speech(const int16_t *buffer, size_t num_samples);

// [DEBUG TEST] Phát lại âm thanh 16kHz ra loa 24kHz để người dùng kiểm tra chất lượng thu
void audio_hal_debug_play_captured_audio(const int16_t *audio_16k, size_t num_samples);

#ifdef __cplusplus
}
#endif
