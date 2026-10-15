#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo AI Cloud component
void ai_cloud_init(void);

// Bắt đầu một phiên Live API (mở websocket, gửi setup)
bool ai_cloud_start_turn(void);

// Gửi một chunk âm thanh PCM (16kHz, 16bit, mono) lên Live API theo thời gian thực
void ai_cloud_send_activity_start(void);
void ai_cloud_send_audio_chunk(const uint8_t *data, size_t size);

// Báo hiệu đã nói xong để Live API trả về kết quả
void ai_cloud_end_turn(void);

// Báo hiệu khi loa đã phát xong toàn bộ dữ liệu âm thanh và chuyển về trạng thái nghỉ
void ai_cloud_on_playback_idle(void);

#ifdef __cplusplus
}
#endif
