#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==========================================
// 1. WAKE WORD (Hey Cherry - Continuous 250ms)
// ==========================================
#define EI_WAKEWORD_SLICE_SAMPLES       4000    // 250ms @ 16kHz
#define EI_WAKEWORD_PEAK_THRESHOLD      0.72f   // Ngưỡng đỉnh kích hoạt đơn (72.0%)
#define EI_WAKEWORD_CANDIDATE_THRESHOLD 0.58f   // Ngưỡng tích lũy 2 lát (58.0%)
#define EI_WAKEWORD_AVERAGE_THRESHOLD   0.65f   // Điểm trung bình 2 lát (65.0%)
#define EI_WAKEWORD_MARGIN_THRESHOLD    0.15f   // Cách biệt tối thiểu với unknown (15.0%)

bool ei_wakeword_init(void);
void ei_wakeword_reset(void);
size_t ei_wakeword_get_slice_size(void);
bool ei_wakeword_process_slice(const int16_t *audio_chunk, size_t chunk_samples, 
                               float *out_confidence, const char **out_label);

// ==========================================
// 2. COMMAND CONTROL (8 Lệnh - Window 1.2s, Stride 400ms)
// Nhãn: bat_cong_tac, bat_den, bat_quat, noise, tat_cong_tac, tat_den, tat_quat, unknown
// ==========================================
#define EI_COMMAND_WINDOW_SAMPLES       19200   // 1.2 giây @ 16kHz (Window size)
#define EI_COMMAND_STRIDE_SAMPLES       6400    // 400ms @ 16kHz (Stride for Lần 2 tại 1.6s)
#define EI_COMMAND_THRESHOLD           0.70f   // Ngưỡng tin cậy chấp nhận lệnh cục bộ (70.0%)

bool ei_command_init(void);
void ei_command_reset_window(void);
bool ei_command_slide_chunk(const int16_t *chunk_400ms, size_t num_samples,
                            float *out_confidence, const char **out_label);
bool ei_command_classify(const int16_t *audio_buffer, size_t num_samples, 
                         float *out_confidence, const char **out_label);

#ifdef __cplusplus
}
#endif
