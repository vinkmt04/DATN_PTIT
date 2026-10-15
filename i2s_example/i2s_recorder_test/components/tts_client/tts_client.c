#include "tts_client.h"
#include "audio_hal.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "esp_crt_bundle.h"
#include <string.h>
#include <ctype.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static const char *TAG = "tts_client";

static mp3dec_t mp3d;
static uint8_t *mp3_buf = NULL;
static size_t mp3_buf_len = 0;
#define MP3_BUF_MAX 16384 // 16KB buffer for incoming MP3 data

// A simple URL encoder
static char *url_encode(const char *str) {
    const char *hex = "0123456789ABCDEF";
    char *encoded = (char*)heap_caps_malloc(strlen(str) * 3 + 1, MALLOC_CAP_SPIRAM);
    if (!encoded) return NULL;
    char *p = encoded;
    while (*str) {
        if (isalnum((unsigned char)*str) || *str == '-' || *str == '_' || *str == '.' || *str == '~') {
            *p++ = *str;
        } else if (*str == ' ') {
            *p++ = '+';
        } else {
            *p++ = '%';
            *p++ = hex[(*str >> 4) & 15];
            *p++ = hex[*str & 15];
        }
        str++;
    }
    *p = '\0';
    return encoded;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA: {
            if (evt->data_len > 0 && mp3_buf != NULL) {
                if (mp3_buf_len + evt->data_len > MP3_BUF_MAX) {
                    ESP_LOGE(TAG, "MP3 buffer overflow");
                    return ESP_FAIL;
                }
                memcpy(mp3_buf + mp3_buf_len, evt->data, evt->data_len);
                mp3_buf_len += evt->data_len;

                mp3dec_frame_info_t info;
                short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

                while (mp3_buf_len > 0) {
                    int samples = mp3dec_decode_frame(&mp3d, mp3_buf, mp3_buf_len, pcm, &info);
                    if (info.frame_bytes > 0) {
                        if (samples > 0) {
                            audio_hal_play_chunk((const uint8_t*)pcm, samples * info.channels * sizeof(short));
                        }
                        memmove(mp3_buf, mp3_buf + info.frame_bytes, mp3_buf_len - info.frame_bytes);
                        mp3_buf_len -= info.frame_bytes;
                    } else {
                        break;
                    }
                }
            }
        }
        break;
        default:
            break;
    }
    return ESP_OK;
}

void tts_client_play(const char *text) {
    if (!text) return;
    
    char *encoded_text = url_encode(text);
    if (!encoded_text) return;

    // Google Translate TTS endpoint
    char url[1024];
    snprintf(url, sizeof(url), "http://translate.google.com/translate_tts?ie=UTF-8&q=%s&tl=vi&client=tw-ob", encoded_text);
    heap_caps_free(encoded_text);

    ESP_LOGI(TAG, "Playing TTS: %s", text);

    mp3_buf = (uint8_t *)heap_caps_malloc(MP3_BUF_MAX, MALLOC_CAP_SPIRAM);
    if (!mp3_buf) {
        ESP_LOGE(TAG, "Failed to allocate MP3 buffer");
        return;
    }
    mp3_buf_len = 0;
    mp3dec_init(&mp3d);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = _http_event_handler,
        .buffer_size = 4096,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        heap_caps_free(mp3_buf);
        mp3_buf = NULL;
        return;
    }
    
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TTS HTTP GET Status = %d", esp_http_client_get_status_code(client));
    } else {
        ESP_LOGE(TAG, "TTS HTTP GET failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    heap_caps_free(mp3_buf);
    mp3_buf = NULL;
}
