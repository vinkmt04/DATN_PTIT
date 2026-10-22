#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Các hằng số kiến trúc nhận diện Wake Word 'Hey Cherry' (Edge Impulse Continuous Engine 250ms)
#define WAKE_WORD_PEAK_THRESHOLD      0.78f  // Ngưỡng kích hoạt đỉnh cực đại 1 lát cắt (78.0% - bắt nhạy mọi lần nói, thường đạt 85% - 99%)
#define WAKE_WORD_CANDIDATE_THRESHOLD 0.65f  // Ngưỡng ứng viên lát cắt tiềm năng (65.0%)
#define WAKE_WORD_AVERAGE_THRESHOLD   0.70f  // Ngưỡng trung bình cộng 2 lát cắt liên tiếp (70.0%)
#define WAKE_WORD_MARGIN_THRESHOLD    0.15f  // Khoảng cách biên an toàn tối thiểu so với nhãn 'unknown' (15.0%)

/**
 * @brief Khởi tạo mô hình Edge Impulse Wake Word 'Hey Cherry'
 * @return true nếu thành công
 */
bool wake_word_init(void);

/**
 * @brief Lấy kích thước lát cắt chuẩn (số mẫu âm thanh 16kHz)
 * 4000 mẫu = 250ms
 */
size_t wake_word_get_slice_size(void);

/**
 * @brief Chạy suy luận trên một lát cắt âm thanh (Continuous Sliding Window 250ms)
 * Có tích hợp Digital Pre-Gain 2.5x và Soft Limiter để bù âm lượng cho mic INMP441
 * 
 * @param audio_chunk Mảng dữ liệu âm thanh 16-bit PCM Mono 16kHz
 * @param chunk_samples Số lượng mẫu âm thanh trong chunk (phải bằng EI_CLASSIFIER_SLICE_SIZE = 4000)
 * @param out_confidence Con trỏ nhận xác suất của nhãn 'hey_cherry' (có thể NULL)
 * @param out_label Con trỏ nhận tên nhãn có điểm cao nhất (có thể NULL)
 * @return true nếu 'hey_cherry' đạt điều kiện kích hoạt
 */
bool wake_word_process_slice(const int16_t *audio_chunk, size_t chunk_samples, float *out_confidence, const char **out_label);

/**
 * @brief Xóa trạng thái bộ đệm trượt (reset)
 */
void wake_word_reset(void);

#ifdef __cplusplus
}
#endif
