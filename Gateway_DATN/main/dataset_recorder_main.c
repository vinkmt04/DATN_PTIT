#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "sys_core.h"
#include "audio_hal.h"

static const char *TAG = "dataset_recorder";

#define RECORD_SAMPLE_RATE 16000
#define RECORD_CHANNELS 1
#define RECORD_BITS 16
#define BYTES_PER_SECOND (RECORD_SAMPLE_RATE * RECORD_CHANNELS * (RECORD_BITS / 8)) // 32000 bytes/s

typedef struct {
    char riff_tag[4];        // "RIFF"
    uint32_t riff_length;     // total length - 8
    char wave_tag[4];        // "WAVE"
    char fmt_tag[4];         // "fmt "
    uint32_t fmt_length;      // 16
    uint16_t audio_format;    // 1 (PCM)
    uint16_t num_channels;    // 1 (Mono)
    uint32_t sample_rate;     // 16000
    uint32_t byte_rate;       // 16000 * 1 * 2 = 32000
    uint16_t block_align;     // 1 * 2 = 2
    uint16_t bits_per_sample; // 16
    char data_tag[4];        // "data"
    uint32_t data_length;     // PCM bytes
} __attribute__((packed)) wav_header_t;

// Các hàm Stub thỏa mãn liên kết của audio_hal khi chạy riêng biệt không có ai_cloud
void ai_cloud_on_playback_idle(void) {}
void ai_cloud_abort(void) {}

// Lưu mẫu vừa thu gần nhất vào PSRAM để phát thử ra loa ESP32 khi cần
static uint8_t *s_last_wav_buf = NULL;
static size_t s_last_wav_size = 0;

static const char HTML_INDEX[] = 
"<!DOCTYPE html>"
"<html lang='vi'>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>ESP32-S3 Dataset Recorder</title>"
"<style>"
"  :root { --bg: #0f172a; --card: #1e293b; --primary: #38bdf8; --text: #f8fafc; --accent: #ef4444; --green: #22c55e; }"
"  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }"
"  body { background: var(--bg); color: var(--text); padding: 16px; display: flex; flex-direction: column; align-items: center; min-height: 100vh; }"
"  .container { max-width: 520px; width: 100%; }"
"  .header { text-align: center; margin-bottom: 20px; }"
"  .header h1 { font-size: 22px; color: var(--primary); margin-bottom: 6px; }"
"  .header p { font-size: 13px; color: #94a3b8; }"
"  .badge { display: inline-block; background: #0369a1; padding: 4px 12px; border-radius: 999px; font-size: 12px; font-weight: bold; margin-top: 6px; }"
"  .card { background: var(--card); border-radius: 16px; padding: 18px; margin-bottom: 14px; border: 1px solid #334155; box-shadow: 0 4px 12px rgba(0,0,0,0.3); }"
"  .section-title { font-size: 13px; font-weight: bold; color: #cbd5e1; margin-bottom: 8px; }"
"  .selector-grid { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 8px; margin-bottom: 14px; }"
"  .sel-btn { background: #334155; border: 2px solid transparent; color: #cbd5e1; padding: 8px 4px; border-radius: 10px; font-size: 12px; font-weight: bold; cursor: pointer; text-align: center; transition: all 0.2s; }"
"  .sel-btn.active { border-color: var(--primary); background: #0284c7; color: white; }"
"  .btn-group { display: flex; flex-direction: column; gap: 10px; }"
"  button.act-btn { border: none; padding: 14px 18px; border-radius: 12px; font-size: 15px; font-weight: bold; cursor: pointer; transition: all 0.2s; color: white; display: flex; align-items: center; justify-content: center; gap: 8px; }"
"  button.act-btn:active { transform: scale(0.98); }"
"  button:disabled { opacity: 0.5; cursor: not-allowed; }"
"  .btn-cherry { background: linear-gradient(135deg, #e11d48, #be123c); }"
"  .btn-noise  { background: linear-gradient(135deg, #475569, #334155); }"
"  .btn-other  { background: linear-gradient(135deg, #0284c7, #0369a1); }"
"  .btn-speaker { background: linear-gradient(135deg, #10b981, #059669); margin-top: 10px; width: 100%; display: none; }"
"  .status-box { text-align: center; font-size: 15px; font-weight: bold; min-height: 48px; display: flex; flex-direction: column; align-items: center; justify-content: center; color: var(--primary); margin: 8px 0; }"
"  .progress-bar-bg { width: 100%; height: 8px; background: #334155; border-radius: 999px; overflow: hidden; margin-top: 8px; display: none; }"
"  .progress-bar-fill { height: 100%; width: 0%; background: var(--green); transition: width 0.2s linear; }"
"  .audio-player { width: 100%; margin-top: 14px; display: none; }"
"  .tip-box { font-size: 12px; color: #94a3b8; background: #0f172a; padding: 10px 14px; border-radius: 10px; border-left: 3px solid var(--primary); line-height: 1.5; margin-top: 12px; }"
"  .history { font-size: 13px; color: #94a3b8; line-height: 1.6; max-height: 140px; overflow-y: auto; }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"  <div class='header'>"
"    <h1>🎙️ ESP32-S3 Audio Recorder</h1>"
"    <p>Thu âm trực tiếp từ micro INMP441 cho Edge Impulse</p>"
"    <span class='badge'>Chuẩn 16kHz Mono 16-bit PCM</span>"
"  </div>"
"  <div class='card'>"
"    <div class='section-title'>1. THỜI LƯỢNG MẪU:</div>"
"    <div class='selector-grid'>"
"      <div class='sel-btn dur-btn active' onclick='setDuration(60, this)'>⏱️ 1 Phút<br><small style='font-size:10px;font-weight:normal;'>Nói ~30 lần</small></div>"
"      <div class='sel-btn dur-btn' onclick='setDuration(30, this)'>⚡ 30 Giây<br><small style='font-size:10px;font-weight:normal;'>Nói ~15 lần</small></div>"
"      <div class='sel-btn dur-btn' onclick='setDuration(75, this)'>🔥 1 Phút 15s<br><small style='font-size:10px;font-weight:normal;'>Nói ~38 lần</small></div>"
"    </div>"
"    <div class='section-title'>2. KHUẾCH ĐẠI ÂM LƯỢNG (MIC GAIN):</div>"
"    <div class='selector-grid' style='grid-template-columns: 1.2fr 1fr 1fr;'>"
"      <div class='sel-btn gain-btn active' onclick=\"setGain('auto', this)\">⚡ Tự động (Auto)<br><small style='font-size:10px;font-weight:normal;'>Chuẩn hóa to rõ</small></div>"
"      <div class='sel-btn gain-btn' onclick=\"setGain('4x', this)\">🔊 Tăng 4x<br><small style='font-size:10px;font-weight:normal;'>+12 dB</small></div>"
"      <div class='sel-btn gain-btn' onclick=\"setGain('1x', this)\">🔈 Gốc (1x)<br><small style='font-size:10px;font-weight:normal;'>Không chỉnh</small></div>"
"    </div>"
"    <div class='section-title'>3. CHỌN NHÃN ĐỂ THU ÂM:</div>"
"    <div class='btn-group'>"
"      <button class='act-btn btn-cherry' onclick=\"startRecord('hey_cherry')\">🔴 Thu 'Hey Cherry'</button>"
"      <button class='act-btn btn-noise'  onclick=\"startRecord('noise')\">🌪️ Thu 'Noise' (Tiếng ồn / Quạt)</button>"
"      <button class='act-btn btn-other'  onclick=\"startRecord('unknown')\">💬 Thu 'Unknown' (Từ khác)</button>"
"    </div>"
"    <div class='status-box' id='status-box'>"
"      <div id='status-text'>Sẵn sàng. Hãy chọn nhãn để thu!</div>"
"      <div class='progress-bar-bg' id='progress-bg'><div class='progress-bar-fill' id='progress-fill'></div></div>"
"    </div>"
"    <audio id='player' class='audio-player' controls></audio>"
"    <button id='btn-play-esp32' class='act-btn btn-speaker' onclick='playOnESP32()'>🔊 Phát lại ra loa MAX98357 của ESP32</button>"
"    <div class='tip-box' id='tip-text'>"
"      💡 <b>Chế độ Tự động (Auto)</b>: Đã được bật sẵn giúp tự động khuếch đại giọng nói to rõ, triệt tiêu hiện tượng mic nhỏ trên máy tính/điện thoại!"
"    </div>"
"  </div>"
"  <div class='card'>"
"    <div class='section-title'>📁 CÁC FILE ĐÃ THU TRONG PHIÊN NÀY:</div>"
"    <div class='history' id='history'>Chưa có file nào.</div>"
"  </div>"
"</div>"
"<script>"
"  let selectedDuration = 60;"
"  let selectedGain = 'auto';"
"  let count = 0;"
"  function setDuration(sec, el) {"
"    selectedDuration = sec;"
"    document.querySelectorAll('.dur-btn').forEach(b => b.classList.remove('active'));"
"    el.classList.add('active');"
"  }"
"  function setGain(g, el) {"
"    selectedGain = g;"
"    document.querySelectorAll('.gain-btn').forEach(b => b.classList.remove('active'));"
"    el.classList.add('active');"
"  }"
"  async function startRecord(label) {"
"    const statusText = document.getElementById('status-text');"
"    const progBg = document.getElementById('progress-bg');"
"    const progFill = document.getElementById('progress-fill');"
"    const btns = document.querySelectorAll('button.act-btn');"
"    const player = document.getElementById('player');"
"    const speakerBtn = document.getElementById('btn-play-esp32');"
"    btns.forEach(b => b.disabled = true);"
"    statusText.innerText = 'Chuẩn bị trong 2 giây...';"
"    statusText.style.color = '#f59e0b';"
"    await new Promise(r => setTimeout(r, 1500));"
"    statusText.innerText = '🎙️ NÓI VÀO MIC NGAY! (Thu ' + selectedDuration + 's)';"
"    statusText.style.color = '#ef4444';"
"    progBg.style.display = 'block'; progFill.style.width = '0%';"
"    let elapsed = 0;"
"    const timer = setInterval(() => {"
"      elapsed += 0.2;"
"      let percent = Math.min(100, (elapsed / selectedDuration) * 100);"
"      progFill.style.width = percent + '%';"
"    }, 200);"
"    try {"
"      const res = await fetch('/record?label=' + label + '&duration=' + selectedDuration + '&gain=' + selectedGain);"
"      clearInterval(timer); progFill.style.width = '100%';"
"      if (!res.ok) throw new Error('ESP32 bận hoặc lỗi');"
"      const blob = await res.blob();"
"      const url = URL.createObjectURL(blob);"
"      player.src = url; player.style.display = 'block'; player.play();"
"      speakerBtn.style.display = 'flex';"
"      count++;"
"      const filename = label + '_' + selectedDuration + 's_' + String(count).padStart(3, '0') + '.wav';"
"      const a = document.createElement('a'); a.href = url; a.download = filename; a.click();"
"      statusText.innerText = '✅ Đã lưu file: ' + filename + ' (' + (blob.size/1024).toFixed(1) + ' KB)';"
"      statusText.style.color = '#22c55e';"
"      const hist = document.getElementById('history');"
"      if (count === 1) hist.innerHTML = '';"
"      hist.innerHTML = '<div>• <b>' + filename + '</b> - ' + (blob.size/1024).toFixed(1) + ' KB (' + new Date().toLocaleTimeString() + ')</div>' + hist.innerHTML;"
"    } catch(e) {"
"      clearInterval(timer);"
"      statusText.innerText = '❌ Lỗi thu âm: ' + e.message; statusText.style.color = '#ef4444';"
"    } finally {"
"      btns.forEach(b => b.disabled = false);"
"      setTimeout(() => { progBg.style.display = 'none'; }, 2000);"
"    }"
"  }"
"  async function playOnESP32() {"
"    const statusText = document.getElementById('status-text');"
"    statusText.innerText = '🔊 Đang phát qua loa MAX98357 trên ESP32...';"
"    statusText.style.color = '#38bdf8';"
"    try {"
"      const res = await fetch('/play');"
"      if (!res.ok) throw new Error('Chưa có mẫu âm thanh để phát');"
"      statusText.innerText = '✅ Đã phát xong ra loa ESP32!';"
"      statusText.style.color = '#22c55e';"
"    } catch(e) {"
"      statusText.innerText = '❌ Lỗi phát loa: ' + e.message;"
"      statusText.style.color = '#ef4444';"
"    }"
"  }"
"</script>"
"</body>"
"</html>";

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, HTML_INDEX, HTTPD_RESP_USE_STRLEN);
}

// Hàm chuẩn hóa khuếch đại âm thanh (Digital Auto-Gain Normalization)
static void process_audio_gain(int16_t *samples, size_t num_samples, const char *gain_mode) {
    if (num_samples == 0 || samples == NULL) return;

    if (strcmp(gain_mode, "1x") == 0) {
        return; // Giữ nguyên gốc
    }

    if (strcmp(gain_mode, "4x") == 0) {
        // Tăng cố định 4x (+12 dB) với bảo vệ bão hòa clipping
        for (size_t i = 0; i < num_samples; i++) {
            int32_t val = (int32_t)samples[i] * 4;
            if (val > 32767) val = 32767;
            else if (val < -32768) val = -32768;
            samples[i] = (int16_t)val;
        }
        return;
    }

    // Mặc định: "auto" -> Tự động chuẩn hóa đỉnh âm thanh (Peak Normalization tới ~75% thang đo = 24500)
    int16_t max_peak = 0;
    for (size_t i = 0; i < num_samples; i++) {
        int16_t abs_v = abs(samples[i]);
        if (abs_v > max_peak) max_peak = abs_v;
    }

    ESP_LOGI(TAG, "Đỉnh âm thanh gốc thu được (Raw Peak): %d / 32767", max_peak);

    // Nếu có tiếng nói (peak > 150) và chưa chạm đỉnh
    const int16_t TARGET_PEAK = 24500;
    if (max_peak > 150 && max_peak < TARGET_PEAK) {
        float gain = (float)TARGET_PEAK / (float)max_peak;
        // Giới hạn hệ số khuếch đại tối đa 14.0x để không tăng tiếng ồn nền quá mức
        if (gain > 14.0f) gain = 14.0f;
        ESP_LOGI(TAG, "Áp dụng hệ số khuếch đại tự động: x%.2f", gain);

        for (size_t i = 0; i < num_samples; i++) {
            int32_t val = (int32_t)(samples[i] * gain);
            if (val > 32767) val = 32767;
            else if (val < -32768) val = -32768;
            samples[i] = (int16_t)val;
        }
    }
}

static esp_err_t record_get_handler(httpd_req_t *req) {
    char query[64] = {0};
    int duration_sec = 60;
    char label[32] = "sample";
    char gain_mode[16] = "auto";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16] = {0};
        if (httpd_query_key_value(query, "duration", val, sizeof(val)) == ESP_OK) {
            duration_sec = atoi(val);
            if (duration_sec < 1) duration_sec = 1;
            if (duration_sec > 120) duration_sec = 120;
        }
        httpd_query_key_value(query, "label", label, sizeof(label));
        httpd_query_key_value(query, "gain", gain_mode, sizeof(gain_mode));
    }

    size_t pcm_bytes = (size_t)BYTES_PER_SECOND * duration_sec;
    size_t total_wav_size = sizeof(wav_header_t) + pcm_bytes;

    ESP_LOGI(TAG, "Thu âm %ds (%u bytes), nhãn: '%s', gain: '%s'...", duration_sec, (unsigned int)total_wav_size, label, gain_mode);

    // Giải phóng bộ đệm cũ nếu có
    if (s_last_wav_buf != NULL) {
        free(s_last_wav_buf);
        s_last_wav_buf = NULL;
        s_last_wav_size = 0;
    }

    // Cấp phát trong PSRAM (SPIRAM)
    uint8_t *wav_buf = heap_caps_malloc(total_wav_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav_buf) {
        wav_buf = malloc(total_wav_size);
    }
    if (!wav_buf) {
        ESP_LOGE(TAG, "Không đủ RAM cấp phát %u bytes!", (unsigned int)total_wav_size);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Đóng gói WAV Header 16kHz Mono 16-bit
    wav_header_t *hdr = (wav_header_t *)wav_buf;
    memcpy(hdr->riff_tag, "RIFF", 4);
    hdr->riff_length = total_wav_size - 8;
    memcpy(hdr->wave_tag, "WAVE", 4);
    memcpy(hdr->fmt_tag, "fmt ", 4);
    hdr->fmt_length = 16;
    hdr->audio_format = 1; // PCM
    hdr->num_channels = RECORD_CHANNELS;
    hdr->sample_rate = RECORD_SAMPLE_RATE;
    hdr->byte_rate = BYTES_PER_SECOND;
    hdr->block_align = RECORD_CHANNELS * (RECORD_BITS / 8);
    hdr->bits_per_sample = RECORD_BITS;
    memcpy(hdr->data_tag, "data", 4);
    hdr->data_length = pcm_bytes;

    // Đọc PCM từ mic INMP441
    int16_t *pcm_out = (int16_t *)(wav_buf + sizeof(wav_header_t));
    size_t total_recorded = 0;
    size_t chunk_bytes = 2048;

    while (total_recorded < pcm_bytes) {
        size_t to_read = pcm_bytes - total_recorded;
        if (to_read > chunk_bytes) to_read = chunk_bytes;

        size_t read_bytes = audio_hal_record_chunk((int16_t *)((uint8_t *)pcm_out + total_recorded), to_read);
        if (read_bytes > 0) {
            total_recorded += read_bytes;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    // Tự động chuẩn hóa khuếch đại âm lượng to rõ
    size_t num_samples = pcm_bytes / sizeof(int16_t);
    process_audio_gain(pcm_out, num_samples, gain_mode);

    // Lưu vào con trỏ phát lại
    s_last_wav_buf = wav_buf;
    s_last_wav_size = total_wav_size;

    ESP_LOGI(TAG, "Thu âm hoàn tất! Gửi file WAV về Client...");

    // Gửi trả file WAV về Client
    char disposition[96];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s_%ds.wav\"", label, duration_sec);

    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, (const char *)wav_buf, total_wav_size);
}

// Endpoint phát lại mẫu vừa thu ra loa MAX98357 của ESP32
static esp_err_t play_get_handler(httpd_req_t *req) {
    if (s_last_wav_buf == NULL || s_last_wav_size <= sizeof(wav_header_t)) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Bắt đầu phát mẫu âm thanh vừa thu ra loa MAX98357 của ESP32...");

    audio_hal_set_turn_active(true);

    uint8_t *pcm_data = s_last_wav_buf + sizeof(wav_header_t);
    size_t pcm_len = s_last_wav_size - sizeof(wav_header_t);
    size_t offset = 0;
    const size_t chunk_size = 2048;

    while (offset < pcm_len) {
        size_t to_send = pcm_len - offset;
        if (to_send > chunk_size) to_send = chunk_size;

        audio_hal_play_chunk(pcm_data + offset, to_send);
        offset += to_send;
    }

    // Đợi 500ms rồi trả về hoàn tất
    vTaskDelay(pdMS_TO_TICKS(500));
    audio_hal_set_turn_active(false);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240;
    config.max_uri_handlers = 8;

    ESP_LOGI(TAG, "Khởi động HTTP Server trên cổng %d...", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_index = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = index_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_index);

        httpd_uri_t uri_record = {
            .uri       = "/record",
            .method    = HTTP_GET,
            .handler   = record_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_record);

        httpd_uri_t uri_play = {
            .uri       = "/play",
            .method    = HTTP_GET,
            .handler   = play_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &uri_play);

        return server;
    }

    ESP_LOGE(TAG, "Lỗi khởi động HTTP Server!");
    return NULL;
}

void app_main(void) {
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " ESP32-S3 AUDIO DATASET RECORDER (EDGE IMPULSE)   ");
    ESP_LOGI(TAG, "==================================================");

    // 1. Kết nối Wi-Fi
    sys_core_init();

    // 2. Khởi tạo phần cứng Mic INMP441 và Loa
    audio_hal_init();

    // 3. Khởi động Web Server
    start_webserver();

    ESP_LOGI(TAG, "Hệ thống đã sẵn sàng!");
    ESP_LOGI(TAG, "Mở trình duyệt gõ địa chỉ IP của ESP32 để bắt đầu thu âm.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
