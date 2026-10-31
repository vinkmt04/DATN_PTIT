#include "include/ei_wrapper.h"
#include "model_wakeword/model-parameters/model_metadata.h"
#include "edge-impulse-sdk/classifier/ei_run_dsp.h"
#include "model_wakeword/model-parameters/model_variables.h"

using namespace ModelWakeWord;

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "ei_wakeword";

static int16_t s_boosted_slice_buffer[EI_WAKEWORD_SLICE_SAMPLES];
static float s_prev_hey_cherry_score = 0.0f;
static int s_consecutive_valid_slices = 0;

static int get_wakeword_audio_signal_data(size_t offset, size_t length, float *out_ptr) {
    for (size_t i = 0; i < length; i++) {
        out_ptr[i] = (float)s_boosted_slice_buffer[offset + i];
    }
    return 0;
}

bool ei_wakeword_init(void) {
    ESP_LOGI(TAG, "Initializing Model 1: 'Hey Cherry' Wake Word Engine (Continuous 250ms)...");
    run_classifier_init(&ModelWakeWord::ei_default_impulse);
    s_prev_hey_cherry_score = 0.0f;
    s_consecutive_valid_slices = 0;
    memset(s_boosted_slice_buffer, 0, sizeof(s_boosted_slice_buffer));

    ESP_LOGI(TAG, "Wake Word Engine Ready -> Project ID: %lu | Window: %lu samples (%lums) | Slice: %lu samples (%lums)",
             (unsigned long)ModelWakeWord::ei_default_impulse.impulse->project_id,
             (unsigned long)ModelWakeWord::ei_default_impulse.impulse->raw_sample_count,
             (unsigned long)((ModelWakeWord::ei_default_impulse.impulse->raw_sample_count * 1000.0f) / ModelWakeWord::ei_default_impulse.impulse->frequency),
             (unsigned long)ModelWakeWord::ei_default_impulse.impulse->slice_size,
             (unsigned long)((ModelWakeWord::ei_default_impulse.impulse->slice_size * 1000.0f) / ModelWakeWord::ei_default_impulse.impulse->frequency));
    return true;
}

size_t ei_wakeword_get_slice_size(void) {
    return EI_WAKEWORD_SLICE_SAMPLES;
}

void ei_wakeword_reset(void) {
    run_classifier_init(&ModelWakeWord::ei_default_impulse);
    s_prev_hey_cherry_score = 0.0f;
    s_consecutive_valid_slices = 0;
    ESP_LOGD(TAG, "Wake Word rolling buffer & state reset.");
}

bool ei_wakeword_process_slice(const int16_t *audio_chunk, size_t chunk_samples, 
                               float *out_confidence, const char **out_label) {
    if (audio_chunk == nullptr || chunk_samples == 0) return false;

    if (chunk_samples != EI_WAKEWORD_SLICE_SAMPLES) {
        ESP_LOGW(TAG, "Chunk size mismatch! Expected %d, got %zu", EI_WAKEWORD_SLICE_SAMPLES, chunk_samples);
        return false;
    }

    // 1. DIGITAL PRE-GAIN CỐ ĐỊNH 4.0X (+12 dB) KÈM SOFT LIMITER (-30000 -> +30000)
    int16_t raw_peak = 0;
    for (size_t i = 0; i < chunk_samples; i++) {
        int16_t abs_v = abs(audio_chunk[i]);
        if (abs_v > raw_peak) raw_peak = abs_v;

        int32_t val = (int32_t)audio_chunk[i] * 4;
        if (val > 30000) val = 30000;
        else if (val < -30000) val = -30000;
        s_boosted_slice_buffer[i] = (int16_t)val;
    }

    signal_t signal;
    signal.total_length = chunk_samples;
    signal.get_data = &get_wakeword_audio_signal_data;

    ei_impulse_result_t result = { 0 };
    // Chạy continuous MFCC trên lát cắt 250ms bằng impulse của ModelWakeWord
    EI_IMPULSE_ERROR r = run_classifier_continuous(&ModelWakeWord::ei_default_impulse, 
                                                   &signal, &result, false, false);
    if (r != EI_IMPULSE_OK) {
        ESP_LOGE(TAG, "run_classifier_continuous error: %d", r);
        return false;
    }

    float hey_cherry_score = 0.0f;
    float noise_score = 0.0f;
    float unknown_score = 0.0f;
    float max_score = 0.0f;
    const char *best_label = "unknown";

    for (size_t i = 0; i < ModelWakeWord::ei_default_impulse.impulse->label_count; i++) {
        float val = result.classification[i].value;
        const char *lbl = result.classification[i].label;

        if (strcmp(lbl, "hey_cherry") == 0) {
            hey_cherry_score = val;
        } else if (strcmp(lbl, "noise") == 0) {
            noise_score = val;
        } else if (strcmp(lbl, "unknown") == 0) {
            unknown_score = val;
        }

        if (val > max_score) {
            max_score = val;
            best_label = lbl;
        }
    }

    if (out_confidence) *out_confidence = hey_cherry_score;
    if (out_label) *out_label = best_label;

    bool is_top = (strcmp(best_label, "hey_cherry") == 0);
    float margin = hey_cherry_score - unknown_score;

    static int s_eval_log_counter = 0;
    s_eval_log_counter++;

    if (hey_cherry_score >= 0.20f || is_top || (s_eval_log_counter % 12 == 0)) {
        ESP_LOGI(TAG, "[EVAL] hey_cherry: %4.1f%% | unknown: %4.1f%% | noise: %4.1f%% (Peak: %d | Top: '%s')",
                 hey_cherry_score * 100.0f, unknown_score * 100.0f, noise_score * 100.0f, raw_peak, best_label);
    }

    // NHÁNH 1: SINGLE SLICE PEAK TRIGGER
    if (hey_cherry_score >= EI_WAKEWORD_PEAK_THRESHOLD && is_top && margin >= EI_WAKEWORD_MARGIN_THRESHOLD) {
        ESP_LOGW(TAG, ">>> [PEAK TRIGGER] WAKE WORD DETECTED! Score: %.1f%% (Margin: %.1f%%) <<<",
                 hey_cherry_score * 100.0f, margin * 100.0f);
        ei_wakeword_reset();
        return true;
    }

    // NHÁNH 2: DUAL SLICE ACCUMULATOR TRIGGER
    if (hey_cherry_score >= EI_WAKEWORD_CANDIDATE_THRESHOLD && is_top && margin >= EI_WAKEWORD_MARGIN_THRESHOLD) {
        s_consecutive_valid_slices++;

        if (s_prev_hey_cherry_score >= EI_WAKEWORD_CANDIDATE_THRESHOLD) {
            float avg_score = (s_prev_hey_cherry_score + hey_cherry_score) * 0.5f;
            if (avg_score >= EI_WAKEWORD_AVERAGE_THRESHOLD) {
                ESP_LOGW(TAG, ">>> [DUAL-SLICE TRIGGER] WAKE WORD DETECTED! Curr: %.1f%% | Prev: %.1f%% | Avg: %.1f%% <<<",
                         hey_cherry_score * 100.0f, s_prev_hey_cherry_score * 100.0f, avg_score * 100.0f);
                if (out_confidence) *out_confidence = avg_score;
                ei_wakeword_reset();
                return true;
            }
        }
        s_prev_hey_cherry_score = hey_cherry_score;
    } else {
        s_prev_hey_cherry_score = 0.0f;
        s_consecutive_valid_slices = 0;
    }

    return false;
}
