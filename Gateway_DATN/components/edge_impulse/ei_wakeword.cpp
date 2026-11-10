#include "include/ei_wrapper.h"
#include "model_wakeword/model-parameters/model_metadata.h"
#include "edge-impulse-sdk/classifier/ei_run_dsp.h"
#include "model_wakeword/model-parameters/model_variables.h"

using namespace ModelWakeWord;

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "ei_wakeword";

static int16_t s_boosted_slice_buffer[EI_WAKEWORD_SLICE_SAMPLES];
static float s_prev_hey_cherry_score = 0.0f;
static int64_t s_veto_until_ms = 0;

// Bộ đệm lưu đỉnh âm thanh của 4 lát cắt gần nhất (4 * 250ms = 1000ms) để tính Window-Peak chuẩn hóa
static int16_t s_slice_peaks[4] = {0, 0, 0, 0};
static int s_peak_idx = 0;
static float s_smooth_gain = 4.0f;

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
    s_veto_until_ms = 0;
    memset(s_slice_peaks, 0, sizeof(s_slice_peaks));
    s_peak_idx = 0;
    s_smooth_gain = 4.0f;
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
    s_veto_until_ms = 0;
    memset(s_slice_peaks, 0, sizeof(s_slice_peaks));
    s_peak_idx = 0;
    s_smooth_gain = 4.0f;
    ESP_LOGD(TAG, "Wake Word rolling buffer & state reset.");
}

bool ei_wakeword_process_slice(const int16_t *audio_chunk, size_t chunk_samples, 
                               float *out_confidence, const char **out_label) {
    if (audio_chunk == nullptr || chunk_samples == 0) return false;

    if (chunk_samples != EI_WAKEWORD_SLICE_SAMPLES) {
        ESP_LOGW(TAG, "Chunk size mismatch! Expected %d, got %zu", EI_WAKEWORD_SLICE_SAMPLES, chunk_samples);
        return false;
    }

    // 1. DIGITAL AUTO-GAIN THEO CỬA SỔ 1000MS (4 LÁT CẮT GẦN NHẤT) KHỚP 100% DATASET_RECORDER_MAIN.C
    // Lọc 3-point median để loại bỏ gai nhiễu điện tử đơn lẻ
    int16_t raw_peak = 0;
    if (chunk_samples >= 3) {
        for (size_t i = 1; i < chunk_samples - 1; i++) {
            int16_t v0 = abs(audio_chunk[i - 1]);
            int16_t v1 = abs(audio_chunk[i]);
            int16_t v2 = abs(audio_chunk[i + 1]);
            int16_t med = (v0 < v1) ? ((v1 < v2) ? v1 : ((v0 < v2) ? v2 : v0))
                                    : ((v0 < v2) ? v0 : ((v1 < v2) ? v2 : v1));
            if (med > raw_peak) raw_peak = med;
        }
    } else {
        for (size_t i = 0; i < chunk_samples; i++) {
            int16_t abs_v = abs(audio_chunk[i]);
            if (abs_v > raw_peak) raw_peak = abs_v;
        }
    }

    // Cập nhật đỉnh lát hiện tại vào mảng 4 lát (1000ms)
    s_slice_peaks[s_peak_idx] = raw_peak;
    s_peak_idx = (s_peak_idx + 1) % 4;

    // Tìm đỉnh lớn nhất trên toàn bộ cửa sổ 1000ms để giữ nguyên vẹn bao hình động (dynamic envelope)
    int16_t window_peak = 0;
    for (int p = 0; p < 4; p++) {
        if (s_slice_peaks[p] > window_peak) window_peak = s_slice_peaks[p];
    }

    const int16_t TARGET_PEAK = 24500;
    float target_gain = 4.0f; // Mức khuếch đại cơ sở khi im lặng / sàn ồn
    if (window_peak > 150) {
        target_gain = (float)TARGET_PEAK / (float)window_peak;
        if (target_gain > 14.0f) target_gain = 14.0f; // Giới hạn trần 14.0x (+23 dB) khớp dataset_recorder.c
        if (target_gain < 1.0f) target_gain = 1.0f;
    }
    // Làm mượt nhẹ hệ số gain tránh giật bước nhảy giữa các lát
    s_smooth_gain = 0.7f * s_smooth_gain + 0.3f * target_gain;
    float gain = s_smooth_gain;

    int16_t boosted_peak = 0;
    for (size_t i = 0; i < chunk_samples; i++) {
        int32_t val = (int32_t)((float)audio_chunk[i] * gain);
        if (val > 30000) val = 30000;
        else if (val < -30000) val = -30000;
        s_boosted_slice_buffer[i] = (int16_t)val;
        int16_t abs_b = abs(s_boosted_slice_buffer[i]);
        if (abs_b > boosted_peak) boosted_peak = abs_b;
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
    float similar_score = 0.0f;
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
        } else if (strcmp(lbl, "similar") == 0) {
            similar_score = val;
        }

        if (val > max_score) {
            max_score = val;
            best_label = lbl;
        }
    }

    if (out_confidence) *out_confidence = hey_cherry_score;
    if (out_label) *out_label = best_label;

    bool is_top = (strcmp(best_label, "hey_cherry") == 0);

    static int s_eval_log_counter = 0;
    s_eval_log_counter++;

    if (hey_cherry_score >= 0.20f || is_top || (s_eval_log_counter % 12 == 0)) {
        ESP_LOGI(TAG, "[EVAL] hey_cherry: %4.1f%% | similar: %4.1f%% | unknown: %4.1f%% | noise: %4.1f%% (RawPk: %d, x%.1f->%d | Top: '%s') | DSP: %ums, NN: %ums",
                 hey_cherry_score * 100.0f, similar_score * 100.0f, unknown_score * 100.0f, noise_score * 100.0f, 
                 raw_peak, gain, boosted_peak, best_label,
                 (unsigned int)result.timing.dsp, (unsigned int)result.timing.classification);
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    // 1. KIỂM TRA VETO LOCKOUT 650MS (ƯU TIÊN SỐ 1):
    // Khóa 650ms nếu phát hiện dấu hiệu từ tương đồng (Hey Siri, Berry, Harry...)
    if ((similar_score >= 0.38f && similar_score > hey_cherry_score) ||
        (similar_score >= 0.25f && hey_cherry_score < 0.40f) ||
        (similar_score >= 0.60f)) {
        s_veto_until_ms = now_ms + EI_WAKEWORD_VETO_DURATION_MS;
        s_prev_hey_cherry_score = 0.0f; // BẮT BUỘC: Reset ngay tích lũy để không cộng dồn sang nhịp sau
        ESP_LOGW(TAG, ">>> [VETO LOCK] Phát hiện dấu hiệu từ đồng âm (similar: %.1f%%, cherry: %.1f%%) -> Khóa %dms! <<<",
                 similar_score * 100.0f, hey_cherry_score * 100.0f, EI_WAKEWORD_VETO_DURATION_MS);
        return false;
    }

    // 2. NẾU ĐANG TRONG THỜI GIAN KHÓA VETO:
    if (now_ms < s_veto_until_ms) {
        if (hey_cherry_score >= EI_WAKEWORD_PEAK_THRESHOLD) {
            ESP_LOGW(TAG, ">>> [VETO BLOCKED] Bỏ qua hey_cherry %.1f%% do dính đuôi từ similar trước đó (còn %lld ms) <<<",
                     hey_cherry_score * 100.0f, (long long)(s_veto_until_ms - now_ms));
        }
        s_prev_hey_cherry_score = 0.0f;
        return false;
    }

    // 3. KIỂM TRA KÍCH HOẠT HYBRID (Chỉ thực thi khi now_ms >= s_veto_until_ms):

    // Nhánh 1 (Peak Trigger - Bắt tức thì câu nói nằm trọn tâm cửa sổ):
    // Phân tầng kiểm tra độ tinh khiết (purity) để chặn đứng "Hey Siri" vọt 75-90%:
    // - Trường hợp A: Điểm tuyệt đối cực cao và sạch: >= 92% và similar < 6%
    // - Trường hợp B: Điểm từ 75% -> 91% (vùng nhạy cảm dễ trùng tông Siri): BẮT BUỘC similar <= 4% và unknown <= 10%
    // Nếu đạt 75% - 90% nhưng similar > 4% (nghi vấn "Hey Siri"), KHÔNG kích hoạt ở Nhánh 1,
    // mà để nó rơi xuống Nhánh 2 (Dual-Slice Accumulator) chờ lát cắt 250ms tiếp theo xác nhận!
    bool is_clean_ultra_peak = (hey_cherry_score >= 0.92f) &&
                               (similar_score < 0.06f) &&
                               (hey_cherry_score > unknown_score);

    bool is_clean_standard_peak = (hey_cherry_score >= EI_WAKEWORD_PEAK_THRESHOLD && hey_cherry_score < 0.92f) &&
                                  (similar_score <= 0.04f) &&
                                  (unknown_score <= 0.10f) &&
                                  ((hey_cherry_score - similar_score) >= 0.50f) &&
                                  (hey_cherry_score > unknown_score);

    bool is_peak_trigger = is_clean_ultra_peak || is_clean_standard_peak;

    if (is_peak_trigger) {
        ESP_LOGW(TAG, ">>> [PEAK TRIGGER] WAKE WORD DETECTED! Score: %.1f%% (similar: %.1f%%, unknown: %.1f%%) <<<",
                 hey_cherry_score * 100.0f, similar_score * 100.0f, unknown_score * 100.0f);
        s_prev_hey_cherry_score = 0.0f;
        ei_wakeword_reset();
        return true;
    }

    // Nhánh 2 (Dual-Slice Accumulator - Bắt câu nói lướt bị cắt đôi giữa 2 nhịp 250ms):
    if (s_prev_hey_cherry_score >= EI_WAKEWORD_CANDIDATE_THRESHOLD &&
        hey_cherry_score >= EI_WAKEWORD_CANDIDATE_THRESHOLD) {

        float avg_score = (s_prev_hey_cherry_score + hey_cherry_score) * 0.5f;
        bool is_dual_trigger = (avg_score >= EI_WAKEWORD_AVERAGE_THRESHOLD) &&
                               ((hey_cherry_score - similar_score) >= EI_WAKEWORD_DUAL_MARGIN) &&
                               (similar_score < EI_WAKEWORD_SIMILAR_MAX) &&
                               (hey_cherry_score > unknown_score);

        if (is_dual_trigger) {
            ESP_LOGW(TAG, ">>> [DUAL-SLICE TRIGGER] WAKE WORD DETECTED! Curr: %.1f%% | Prev: %.1f%% | Avg: %.1f%% | Similar: %.1f%% (Natural Speech) <<<",
                     hey_cherry_score * 100.0f, s_prev_hey_cherry_score * 100.0f, avg_score * 100.0f, similar_score * 100.0f);
            if (out_confidence) *out_confidence = avg_score;
            s_prev_hey_cherry_score = 0.0f;
            ei_wakeword_reset();
            return true;
        }
    }

    // 4. CẬP NHẬT CHO CHU KỲ 250MS TIẾP THEO:
    s_prev_hey_cherry_score = hey_cherry_score;
    return false;
}
