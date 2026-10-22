#include "wake_word_engine.h"
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wake_word_engine";

static int16_t s_boosted_slice_buffer[EI_CLASSIFIER_SLICE_SIZE];
static float s_prev_hey_cherry_score = 0.0f;
static int s_consecutive_valid_slices = 0;

// Callback chuyển đổi int16 PCM sang float cho bộ DSP MFCC của Edge Impulse
static int get_audio_signal_data(size_t offset, size_t length, float *out_ptr) {
    for (size_t i = 0; i < length; i++) {
        out_ptr[i] = (float)s_boosted_slice_buffer[offset + i];
    }
    return 0;
}

bool wake_word_init(void) {
    ESP_LOGI(TAG, "Initializing Edge Impulse Wake Word Engine ('Hey Cherry' Continuous 250ms)...");
    run_classifier_init();
    s_prev_hey_cherry_score = 0.0f;
    s_consecutive_valid_slices = 0;
    memset(s_boosted_slice_buffer, 0, sizeof(s_boosted_slice_buffer));

    ESP_LOGI(TAG, "Wake Word Engine Ready -> Project ID: %d | Window: %d samples (%dms) | Slice: %d samples (%dms)",
             EI_CLASSIFIER_PROJECT_ID,
             EI_CLASSIFIER_RAW_SAMPLE_COUNT,
             (EI_CLASSIFIER_RAW_SAMPLE_COUNT * 1000) / EI_CLASSIFIER_FREQUENCY,
             EI_CLASSIFIER_SLICE_SIZE,
             (EI_CLASSIFIER_SLICE_SIZE * 1000) / EI_CLASSIFIER_FREQUENCY);
    return true;
}

size_t wake_word_get_slice_size(void) {
    return EI_CLASSIFIER_SLICE_SIZE;
}

void wake_word_reset(void) {
    // BẮT BUỘC gọi sau khi đã bắt được Wake-word hoặc khi quay lại IDLE từ AI_RESPONDING
    // Giúp xóa sạch dư âm "Hey Cherry" cũ trong bộ đệm trượt MFCC, tránh nổ kép (Double Trigger)
    run_classifier_init();
    s_prev_hey_cherry_score = 0.0f;
    s_consecutive_valid_slices = 0;
    ESP_LOGD(TAG, "Wake Word rolling buffer & state reset.");
}

bool wake_word_process_slice(const int16_t *audio_chunk, size_t chunk_samples, float *out_confidence, const char **out_label) {
    if (audio_chunk == nullptr || chunk_samples == 0) return false;

    // Đảm bảo số mẫu truyền vào khớp tuyệt đối với EI_CLASSIFIER_SLICE_SIZE (4000 mẫu = 250ms)
    if (chunk_samples != EI_CLASSIFIER_SLICE_SIZE) {
        ESP_LOGW(TAG, "Chunk size mismatch! Expected %d, got %zu", EI_CLASSIFIER_SLICE_SIZE, chunk_samples);
        return false;
    }

    // =========================================================================
    // 1. DIGITAL PRE-GAIN CỐ ĐỊNH 2.5X (+8 dB) KÈM SOFT LIMITER
    // Bảo toàn 100% tỷ lệ phổ âm sắc giữa phụ âm "Ch" và nguyên âm "ey",
    // chống méo hài âm MFCC, đồng thời bù độ nhạy cho mic INMP441
    // =========================================================================
    int16_t raw_peak = 0;
    for (size_t i = 0; i < chunk_samples; i++) {
        int16_t abs_v = abs(audio_chunk[i]);
        if (abs_v > raw_peak) raw_peak = abs_v;

        // Khuếch đại tuyến tính 2.5x (nhân 5 chia 2)
        int32_t val = ((int32_t)audio_chunk[i] * 5) / 2;
        if (val > 30000) val = 30000;
        else if (val < -30000) val = -30000;
        s_boosted_slice_buffer[i] = (int16_t)val;
    }

    signal_t signal;
    signal.total_length = chunk_samples;
    signal.get_data = &get_audio_signal_data;

    ei_impulse_result_t result = { 0 };
    // Chạy continuous MFCC trên lát cắt 250ms (CPU chỉ mất ~22ms, hoàn toàn không làm tràn DMA)
    // Tắt MAF (enable_maf = false) để lấy điểm xác suất tức thời gốc của mạng nơ-ron (không bị lát cắt âm lặng trước đó kéo tụt điểm)
    EI_IMPULSE_ERROR r = run_classifier_continuous(&signal, &result, false, false);
    if (r != EI_IMPULSE_OK) {
        ESP_LOGE(TAG, "run_classifier_continuous error: %d", r);
        return false;
    }

    float hey_cherry_score = 0.0f;
    float noise_score = 0.0f;
    float unknown_score = 0.0f;
    float max_score = 0.0f;
    const char *best_label = "unknown";

    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
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

    // Chỉ log khi điểm hey_cherry xuất hiện đáng kể (>= 20%) hoặc khi là nhãn cao nhất
    if (hey_cherry_score >= 0.20f || is_top) {
        ESP_LOGI(TAG, "[EVAL] hey_cherry: %4.1f%% | unknown: %4.1f%% | noise: %4.1f%% (Peak: %d)",
                 hey_cherry_score * 100.0f, unknown_score * 100.0f, noise_score * 100.0f, raw_peak);
    }

    // =========================================================================
    // 2. THUẬT TOÁN KÍCH HOẠT 2 NHÁNH (DUAL-BRANCH TRIGGER)
    // =========================================================================

    // NHÁNH 1: KÍCH HOẠT ĐƠN ĐIỂM ĐỈNH CỰC ĐẠI (SINGLE SLICE PEAK >= 75.0%)
    if (hey_cherry_score >= WAKE_WORD_PEAK_THRESHOLD && is_top && margin >= WAKE_WORD_MARGIN_THRESHOLD) {
        ESP_LOGW(TAG, ">>> [PEAK TRIGGER] WAKE WORD DETECTED! Score: %.1f%% (Margin: %.1f%%) <<<",
                 hey_cherry_score * 100.0f, margin * 100.0f);
        wake_word_reset();
        return true;
    }

    // NHÁNH 2: KÍCH HOẠT TÍCH LŨY 2 LÁT CẮT LIÊN TIẾP (DUAL SLICE ACCUMULATOR)
    // Dành cho trường hợp người dùng nói hơi nhanh hoặc hơi nhỏ (lát 1 >= 65%, lát 2 >= 65%, TB >= 70%)
    if (hey_cherry_score >= WAKE_WORD_CANDIDATE_THRESHOLD && is_top && margin >= WAKE_WORD_MARGIN_THRESHOLD) {
        s_consecutive_valid_slices++;

        if (s_prev_hey_cherry_score >= WAKE_WORD_CANDIDATE_THRESHOLD) {
            float avg_score = (s_prev_hey_cherry_score + hey_cherry_score) * 0.5f;
            if (avg_score >= WAKE_WORD_AVERAGE_THRESHOLD) {
                ESP_LOGW(TAG, ">>> [DUAL-SLICE TRIGGER] WAKE WORD DETECTED! Curr: %.1f%% | Prev: %.1f%% | Avg: %.1f%% <<<",
                         hey_cherry_score * 100.0f, s_prev_hey_cherry_score * 100.0f, avg_score * 100.0f);
                if (out_confidence) *out_confidence = avg_score;
                wake_word_reset();
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