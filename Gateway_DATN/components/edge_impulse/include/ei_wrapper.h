#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==========================================
// 1. WAKE WORD (Hey Cherry - Continuous 250ms)
// Nhãn: hey_cherry, noise, similar, unknown (4 classes)
// ==========================================
#define EI_WAKEWORD_SLICE_SAMPLES          4000    // 250ms @ 16kHz
#define EI_WAKEWORD_PEAK_THRESHOLD         0.75f   // Ngưỡng đỉnh kích hoạt tức thì khi trúng tâm cửa sổ (75.0%)
#define EI_WAKEWORD_PEAK_MARGIN            0.40f   // Cách biệt áp đảo so với similar (40.0%)
#define EI_WAKEWORD_CANDIDATE_THRESHOLD    0.50f   // Ngưỡng sàn tối thiểu cho từng lát cắt (50.0%)
#define EI_WAKEWORD_AVERAGE_THRESHOLD      0.68f   // Điểm trung bình 2 lát liên tiếp >= 500ms (68.0% - Chặn đứng 'Hey Siri' 61.1%)
#define EI_WAKEWORD_DUAL_MARGIN            0.35f   // Cách biệt tối thiểu với similar ở nhịp 2 (35.0%)
#define EI_WAKEWORD_SIMILAR_MAX            0.10f   // Giới hạn trần similar khi chốt nhịp 2 (10.0% - Chặn từ bẫy đồng âm)
#define EI_WAKEWORD_VETO_SIMILAR_THRESH    0.38f   // Ngưỡng kích hoạt Veto Lockout khi similar >= 38%
#define EI_WAKEWORD_VETO_CHERRY_CEIL       0.40f   // Ngưỡng trần cherry < 40% để xác nhận từ bẫy similar
#define EI_WAKEWORD_VETO_DURATION_MS       650     // Khóa Veto 650ms (chặn triệt để chu kỳ trượt tiếp theo)

bool ei_wakeword_init(void);
void ei_wakeword_reset(void);
size_t ei_wakeword_get_slice_size(void);
bool ei_wakeword_process_slice(const int16_t *audio_chunk, size_t chunk_samples, 
                               float *out_confidence, const char **out_label);

// ==========================================
// 2. COMMAND CONTROL (6 Lệnh - Buffer RAM 1.5s, Căn Tâm Năng Lượng & Quét 3 Lát Cắt)
// Nhãn: bat_cong_tac, bat_den, noise, tat_cong_tac, tat_den, unknown
// ==========================================
#define EI_COMMAND_WINDOW_SAMPLES          19200   // 1.2 giây @ 16kHz (Window size của Model 2)
#define EI_COMMAND_BUFFER_SAMPLES          24000   // 1.5 giây @ 16kHz (Tổng buffer thu âm trọn gói vào RAM)
#define EI_COMMAND_SLICE2_OFFSET           4800    // Offset Lát cắt 2 (300ms @ 16kHz)
#define EI_COMMAND_THRESHOLD               0.65f   // Ngưỡng tin cậy chấp nhận lệnh cục bộ (65.0%)
#define EI_COMMAND_MARGIN_UNKNOWN          0.20f   // Cách biệt tối thiểu giữa lệnh và unknown (20.0%)

bool ei_command_init(void);
void ei_command_reset_window(void);
bool ei_command_slide_chunk(const int16_t *chunk_400ms, size_t num_samples,
                            float *out_confidence, const char **out_label);
bool ei_command_classify(const int16_t *audio_buffer, size_t num_samples, 
                         float *out_confidence, const char **out_label);

#ifdef __cplusplus
}
#endif
