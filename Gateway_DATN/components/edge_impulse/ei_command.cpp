#include "include/ei_wrapper.h"
#include "model_command/model-parameters/model_metadata.h"
#include "edge-impulse-sdk/classifier/ei_run_dsp.h"
#include "model_command/model-parameters/model_variables.h"

using namespace ModelCommand;

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ei_command";

static const int16_t *s_raw_audio_ptr = nullptr;
static size_t s_raw_audio_len = 0;
static float s_command_gain = 2.5f;

// Bộ đệm 1.2s cho Model 2 (19.200 mẫu @ 16kHz)
static int16_t s_command_window[EI_COMMAND_WINDOW_SAMPLES] = {0};
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
    memset(s_command_window, 0, sizeof(s_command_window));
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

    // Chuẩn hóa đỉnh âm thanh (Peak Normalization) khớp với dữ liệu huấn luyện Studio (Target ~24,000)
    int16_t max_peak = 0;
    for (size_t i = 0; i < num_samples; i++) {
        int16_t abs_v = abs(audio_buffer[i]);
        if (abs_v > max_peak) max_peak = abs_v;
    }

    // Chuẩn hóa đỉnh âm thanh thông minh (Target Peak Normalization ~24,000)
    // Giúp mạng nơ-ron INT8 lượng tử hóa đạt dải động tối ưu (tránh suy biến về 'unknown')
    // Giới hạn dải Gain an toàn từ 1.0x đến 10.0x (+20dB) kèm Soft Limiter (-30000 -> +30000)
    float target_peak = 24000.0f;
    if (max_peak > 600) {
        s_command_gain = target_peak / (float)max_peak;
        if (s_command_gain > 10.0f) s_command_gain = 10.0f;
        if (s_command_gain < 1.0f) s_command_gain = 1.0f;
    } else {
        s_command_gain = 4.0f;
    }

    ESP_LOGI(TAG, "[COMMAND GAIN] Peak: %d (x%.2f = %d, Target ~24000, Soft Limit 30000)", 
             max_peak, s_command_gain, (int)(max_peak * s_command_gain));

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

    char score_summary[128] = {0};
    int off = 0;
    for (size_t i = 0; i < ModelCommand::ei_default_impulse.impulse->label_count; i++) {
        float val = result.classification[i].value;
        const char *lbl = result.classification[i].label;

        if (val > max_score) {
            max_score = val;
            best_label = lbl;
        }

        if (val >= 0.05f && off < (int)sizeof(score_summary) - 25) {
            off += snprintf(score_summary + off, sizeof(score_summary) - off, " %s:%.1f%%", lbl, val * 100.0f);
        }
    }

    if (out_confidence) *out_confidence = max_score;
    if (out_label) *out_label = best_label;

    ESP_LOGI(TAG, ">>> [EVAL COMMAND] Best: '%s' (%.1f%%) | Detail:%s | DSP: %dms, NN: %dms <<<",
             best_label, max_score * 100.0f, score_summary, result.timing.dsp, result.timing.classification);

    bool is_command = is_valid_local_command(best_label);
    if (is_command && max_score >= EI_COMMAND_THRESHOLD) {
        return true;
    }

    return false;
}
