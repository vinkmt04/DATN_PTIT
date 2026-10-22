#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Khởi tạo AI Cloud component
void ai_cloud_init(void);

// Đảm bảo kết nối WebSocket đã sẵn sàng ngầm (gọi khi phát hiện Wake Word hoặc khi có Wi-Fi)
void ai_cloud_prewarm_session(void);

// Bắt đầu một phiên ghi âm người dùng (chuyển sang RECORDING, sẵn sàng stream âm thanh)
bool ai_cloud_start_turn(void);

// Gửi một chunk âm thanh PCM vào TX RingBuffer (Hoàn toàn bất đồng bộ, không bao giờ làm đơ luồng mic)
void ai_cloud_send_audio_chunk(const uint8_t *data, size_t size);

// Báo hiệu đã nói xong để Live API chốt câu lệnh và trả lời
void ai_cloud_end_turn(void);

// Báo hiệu khi loa đã phát xong toàn bộ dữ liệu âm thanh và chuyển về trạng thái nghỉ
void ai_cloud_on_playback_idle(void);

// Hủy phiên khẩn cấp khi có lỗi mạng (phát âm thanh lỗi và ngắt WebSocket)
void ai_cloud_abort(void);

// Hủy lượt êm ái khi người dùng không nói (không phát âm thanh lỗi, giữ nguyên WebSocket kết nối)
void ai_cloud_cancel_turn(void);

// Kiểm tra AI Cloud có đang bận xử lý lượt nói hoặc chờ phản hồi hay không
bool ai_cloud_is_busy(void);

// Kiểm tra kết nối Live API đã sẵn sàng hay chưa
bool ai_cloud_is_ready(void);

#ifdef __cplusplus
}
#endif
