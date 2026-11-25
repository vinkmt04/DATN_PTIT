#include "include/ei_wrapper.h"
#include "model_command/model-parameters/model_metadata.h"
#include "edge-impulse-sdk/classifier/ei_run_dsp.h"
#include "model_command/model-parameters/model_variables.h"

using namespace ModelCommand;

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ei_command";

static const int16_t *s_raw_audio_ptr = nullptr;
static size_t s_raw_audio_len = 0;
static float s_command_gain = 2.5f;

// Bộ đệm 1.2s cho Model 2 (19.200 mẫu @ 16kHz) - Cấp phát động trong PSRAM chỉ khi gọi ei_command_slide_chunk để tiết kiệm 38.4KB DRAM
static int16_t *s_command_window = nullptr;
static size_t s_window_samples_fed = 0;

static int get_command_audio_signal_data(size_t offset, size_t length, float *out_ptr) {
    if (!s_raw_audio_ptr) return -1;
    for (size_t i = 0; i < length; i++) {
        size_t idx = offset + i;
        if (idx < s_raw_audio_len) {
            // Chuẩn hóa khuếch đại tự động (Auto-Gain Normalization) khớp với dữ liệu huấn luyện
            float val = (float)s_raw_audio_ptr[idx] * s_command_gain;
            if (val > 30000.0f) val = 30000.0f;
            else if (val < -30000.0f) val = -30000.0f;
            out_ptr[i] = val;
        } else {
            out_ptr[i] = 0.0f; // Zero-padding ngoài biên
        }
    }
    return 0;
}

bool ei_command_init(void) {
    ESP_LOGI(TAG, "Initializing Model 2: Smart Home Command Engine (1.2s Window, 400ms Stride)...");
    run_classifier_init(&ModelCommand::ei_default_impulse);
    ei_command_reset_window();

    ESP_LOGI(TAG, "Command Engine Ready -> Project ID: %lu | Window: %lu samples (%lums) | Labels: %lu",
             (unsigned long)ModelCommand::ei_default_impulse.impulse->project_id,
             (unsigned long)ModelCommand::ei_default_impulse.impulse->raw_sample_count,
             (unsigned long)((ModelCommand::ei_default_impulse.impulse->raw_sample_count * 1000.0f) / ModelCommand::ei_default_impulse.impulse->frequency),
             (unsigned long)ModelCommand::ei_default_impulse.impulse->label_count);
    return true;
}

void ei_command_reset_window(void) {
    if (s_command_window) {
        memset(s_command_window, 0, EI_COMMAND_WINDOW_SAMPLES * sizeof(int16_t));
    }
    s_window_samples_fed = 0;
    ESP_LOGD(TAG, "Command sliding window reset (zeroed).");
}

static bool is_valid_local_command(const char *label) {
    if (!label) return false;
    // Hệ thống thực tế chỉ giữ Đèn và Công tắc Relay 1 kênh (bỏ Quạt)
    return (strcmp(label, "bat_cong_tac") == 0 ||
            strcmp(label, "tat_cong_tac") == 0 ||
            strcmp(label, "bat_den") == 0 ||
            strcmp(label, "tat_den") == 0);
}

bool ei_command_slide_chunk(const int16_t *chunk_400ms, size_t num_samples,
                            float *out_confidence, const char **out_label) {
    if (chunk_400ms == nullptr || num_samples == 0) return false;

    if (!s_command_window) {
        s_command_window = (int16_t *)heap_caps_malloc(EI_COMMAND_WINDOW_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_command_window) {
            s_command_window = (int16_t *)malloc(EI_COMMAND_WINDOW_SAMPLES * sizeof(int16_t));
        }
        if (!s_command_window) return false;
        memset(s_command_window, 0, EI_COMMAND_WINDOW_SAMPLES * sizeof(int16_t));
    }

    // 1. Trượt cửa sổ: Dịch dữ liệu cũ sang trái, đưa chunk 400ms mới vào cuối
    if (num_samples >= EI_COMMAND_WINDOW_SAMPLES) {
        memcpy(s_command_window, chunk_400ms + (num_samples - EI_COMMAND_WINDOW_SAMPLES),
               EI_COMMAND_WINDOW_SAMPLES * sizeof(int16_t));
    } else {
        size_t shift_samples = EI_COMMAND_WINDOW_SAMPLES - num_samples;
        memmove(s_command_window, s_command_window + num_samples, shift_samples * sizeof(int16_t));
        memcpy(s_command_window + shift_samples, chunk_400ms, num_samples * sizeof(int16_t));
    }
    s_window_samples_fed += num_samples;

    // 2. Chạy suy luận trên toàn bộ cửa sổ 1.2s hiện tại
    return ei_command_classify(s_command_window, EI_COMMAND_WINDOW_SAMPLES, out_confidence, out_label);
}

bool ei_command_classify(const int16_t *audio_buffer, size_t num_samples, 
                         float *out_confidence, const char **out_label) {
    if (audio_buffer == nullptr || num_samples == 0) return false;

    // Chuẩn hóa đỉnh âm thanh đa khung (Multi-Frame Robust Peak Normalization)
    // Chia 19,200 samples thành 20 frames (mỗi frame 960 samples = 60ms)
    // Lấy 3-point median peak của từng frame, sắp xếp tăng dần và chọn frame_peaks[18] (đỉnh cao thứ 2, ~95th percentile)
    // Loại bỏ 100% tiếng va chạm, tiếng click pop đột biến và không bị phụ thuộc vào 1 đỉnh giả
    const size_t NUM_FRAMES = 20;
    const size_t FRAME_SIZE = 960; // 20 * 960 = 19200 = EI_COMMAND_WINDOW_SAMPLES
    int16_t frame_peaks[NUM_FRAMES] = {0};

    for (size_t f = 0; f < NUM_FRAMES; f++) {
        size_t start_idx = f * FRAME_SIZE;
        size_t end_idx = start_idx + FRAME_SIZE;
        if (end_idx > num_samples) end_idx = num_samples;
        if (start_idx >= end_idx) break;

        int16_t f_peak = 0;
        if (end_idx - start_idx >= 3) {
            for (size_t i = start_idx + 1; i < end_idx - 1; i++) {
                int16_t v0 = abs(audio_buffer[i - 1]);
                int16_t v1 = abs(audio_buffer[i]);
                int16_t v2 = abs(audio_buffer[i + 1]);
                int16_t med = (v0 < v1) ? ((v1 < v2) ? v1 : ((v0 < v2) ? v2 : v0))
                                        : ((v0 < v2) ? v0 : ((v1 < v2) ? v2 : v1));
                if (med > f_peak) f_peak = med;
            }
        } else {
            for (size_t i = start_idx; i < end_idx; i++) {
                int16_t abs_v = abs(audio_buffer[i]);
                if (abs_v > f_peak) f_peak = abs_v;
            }
        }
        frame_peaks[f] = f_peak;
    }

    // Sắp xếp đơn giản frame_peaks (insertion sort 20 phần tử)
    for (size_t i = 1; i < NUM_FRAMES; i++) {
        int16_t key = frame_peaks[i];
        int j = (int)i - 1;
        while (j >= 0 && frame_peaks[j] > key) {
            frame_peaks[j + 1] = frame_peaks[j];
            j--;
        }
        frame_peaks[j + 1] = key;
    }

    // Chọn robust_peak = frame_peaks[18] (vị trí 19/20, loại bỏ outlier cao nhất)
    int16_t robust_peak = frame_peaks[NUM_FRAMES - 2]; // index 18

    const int16_t TARGET_PEAK = 24500;
    const float max_gain = (robust_peak < 220) ? 6.0f : 14.0f;
    if (robust_peak > 150) {
        s_command_gain = (float)TARGET_PEAK / (float)robust_peak;
        if (s_command_gain > max_gain) s_command_gain = max_gain;
        if (s_command_gain < 1.0f) s_command_gain = 1.0f;
    } else {
        s_command_gain = 4.0f; // Mức cơ sở khi im lặng / sàn ồn
    }

    ESP_LOGI(TAG, "[COMMAND AUTO-GAIN] RobustPeak(95%%): %d, MaxPeak: %d, Gain: x%.2f -> Boosted: %d (Target ~24500, Max x%.1f)", 
             robust_peak, frame_peaks[NUM_FRAMES - 1], s_command_gain, (int)(robust_peak * s_command_gain), max_gain);

    s_raw_audio_ptr = audio_buffer;
    s_raw_audio_len = num_samples;

    signal_t signal;
    signal.total_length = EI_COMMAND_WINDOW_SAMPLES;
    signal.get_data = &get_command_audio_signal_data;

    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR r = run_classifier(&ModelCommand::ei_default_impulse, 
                                        &signal, &result, false);

    s_raw_audio_ptr = nullptr;
    s_raw_audio_len = 0;

    if (r != EI_IMPULSE_OK) {
        ESP_LOGE(TAG, "run_classifier error: %d", r);
        return false;
    }

    float max_score = 0.0f;
    const char *best_label = "unknown";
    float score_bat_cong_tac = 0.0f;
    float score_bat_den = 0.0f;
    float score_noise = 0.0f;
    float score_tat_cong_tac = 0.0f;
    float score_tat_den = 0.0f;
    float score_unknown = 0.0f;

    for (size_t i = 0; i < ModelCommand::ei_default_impulse.impulse->label_count; i++) {
        float val = result.classification[i].value;
        const char *lbl = result.classification[i].label;

        if (strcmp(lbl, "bat_cong_tac") == 0) score_bat_cong_tac = val;
        else if (strcmp(lbl, "bat_den") == 0) score_bat_den = val;
        else if (strcmp(lbl, "noise") == 0) score_noise = val;
        else if (strcmp(lbl, "tat_cong_tac") == 0) score_tat_cong_tac = val;
        else if (strcmp(lbl, "tat_den") == 0) score_tat_den = val;
        else if (strcmp(lbl, "unknown") == 0) score_unknown = val;

        if (val > max_score) {
            max_score = val;
            best_label = lbl;
        }
    }

    if (out_confidence) *out_confidence = max_score;
    if (out_label) *out_label = best_label;

    ESP_LOGI(TAG, ">>> [CMD_EVAL] bat_den: %.1f%% | tat_den: %.1f%% | bat_cong_tac: %.1f%% | tat_cong_tac: %.1f%% | unknown: %.1f%% | noise: %.1f%% (DSP: %dms, NN: %dms) <<<",
             score_bat_den * 100.0f, score_tat_den * 100.0f,
             score_bat_cong_tac * 100.0f, score_tat_cong_tac * 100.0f,
             score_unknown * 100.0f, score_noise * 100.0f,
             result.timing.dsp, result.timing.classification);

    bool is_command = is_valid_local_command(best_label);
    bool meets_threshold = (max_score >= EI_COMMAND_THRESHOLD);
    bool beats_unknown = ((max_score - score_unknown) >= EI_COMMAND_MARGIN_UNKNOWN);
    bool beats_noise = ((max_score - score_noise) >= EI_COMMAND_MARGIN_UNKNOWN);

    if (is_command && meets_threshold && beats_unknown && beats_noise) {
        return true;
    }

    return false;
}
