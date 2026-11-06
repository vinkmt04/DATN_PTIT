#include "ai_cloud.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "audio_hal.h"
#include "mesh_controller.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

extern void ai_edge_stop_recording(void);

static const char *TAG = "ai_cloud";

// Live API (WebSockets) Configuration
#define GEMINI_API_KEY "...."
#define GEMINI_WS_URL "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=" GEMINI_API_KEY

// Kích thước tĩnh cho bộ đệm Base64 và JSON (cấp phát 1 lần trong PSRAM)
#define TX_BASE64_BUF_SIZE 8192
#define TX_JSON_BUF_SIZE   10240
#define WS_RX_BUF_SIZE     262144  // 256KB: Nuốt trọn mọi frame từ Google (40KB - 80KB) không bao giờ tràn
#define RX_PCM_BUF_SIZE    196608  // 192KB: Chứa tối đa ~4 giây âm thanh PCM 24kHz 16-bit giải mã từ Base64
static unsigned char *s_tx_base64_buf = NULL;
static char *s_tx_json_buf = NULL;
static char *s_ws_rx_buf = NULL;
static size_t s_ws_rx_len = 0;
static unsigned char *s_rx_pcm_buf = NULL;

// Timeouts
#define RESPONSE_TIMEOUT_MS  8000   // Chờ phản hồi đầu tiên từ Gemini: 8s
#define CHUNK_TIMEOUT_MS     5000   // Chờ chunk tiếp theo giữa chừng: 5s (phản ứng nhanh khi cloud ngừng cấp audio)
#define WS_KEEP_ALIVE_TIMEOUT_MS 15000 // Giữ kết nối ngầm 15 giây (Keep-Alive Idle Pool)

typedef enum {
    AI_STATE_IDLE = 0,
    AI_STATE_RECORDING,
    AI_STATE_WAITING_AI,
    AI_STATE_EXECUTING_TOOL,
    AI_STATE_WAITING_POST_TOOL_SPEECH,
    AI_STATE_SPEAKING,
} ai_gateway_state_t;

// Các cờ sự kiện điều khiển bất đồng bộ (Task Notification Bitmask) gửi tới cloud_tx_task
#define CLOUD_EVT_ABORT            (1 << 0) // Lỗi mạng hoặc ngắt khẩn cấp
#define CLOUD_EVT_CANCEL           (1 << 1) // Hủy do khớp lệnh cục bộ Model 2 hoặc No-speech
#define CLOUD_EVT_TIMEOUT          (1 << 2) // Hết hạn Response Timeout từ software timer
#define CLOUD_EVT_KEEP_ALIVE_EXP   (1 << 3) // Hết hạn 15s Keep-Alive pool
#define CLOUD_EVT_START_TURN       (1 << 4) // Bắt đầu lượt thu âm mới
#define CLOUD_EVT_END_TURN         (1 << 5) // Dứt câu nói, xả hết audio để gửi activityEnd

static volatile ai_gateway_state_t s_current_state = AI_STATE_IDLE;
static TimerHandle_t s_session_timer = NULL;
static TimerHandle_t s_keep_alive_timer = NULL;
static SemaphoreHandle_t ws_mutex = NULL;

static esp_websocket_client_handle_t ws_client = NULL;
static volatile bool s_is_ws_connected = false;
static volatile bool s_is_ws_connecting = false;
static volatile bool s_is_ws_stopping = false;
static volatile bool s_setup_complete = false;
static uint32_t s_last_disconnect_time_ms = 0;

// Cờ đồng bộ tín hiệu thoại: đảm bảo gửi activityStart trước audio, và activityEnd sau cùng khi xả hết buffer
static volatile bool s_need_send_activity_start = false;
static volatile bool s_turn_ending = false;
static volatile bool s_cmd_dispatched_in_turn = false;
static volatile bool s_mute_response_audio = false;

// RingBuffer TX: Chứa âm thanh mic từ ai_edge truyền sang (256KB trong PSRAM ~ 8 giây âm thanh)
// Giúp luồng thu âm mic KHÔNG BAO GIỜ BỊ BLOCK bởi mạng!
static RingbufHandle_t s_tx_audio_ringbuf = NULL;
static TaskHandle_t s_tx_task_handle = NULL;

static bool expecting_first_chunk = false;

void ai_cloud_abort(void) {
    ESP_LOGW(TAG, "ai_cloud_abort() requested -> Notifying worker task...");
    if (s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_ABORT, eSetBits);
    }
}

static void session_timer_cb(TimerHandle_t xTimer) {
    if (s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_TIMEOUT, eSetBits);
    }
}

static void parse_server_message(const char *json_string) {
    if (!json_string || json_string[0] == '\0') {
        return;
    }

    // 1. Kiểm tra xác nhận hoàn tất thiết lập từ Gemini Live API
    if (strstr(json_string, "\"setupComplete\"")) {
        ESP_LOGI(TAG, ">>> [GEMINI LIVE] Setup Complete! Connection Ready for Realtime Audio. <<<");
        s_setup_complete = true;
        return;
    }

    // 2. Bắt lỗi từ máy chủ Google API
    if (strstr(json_string, "\"error\"")) {
        ESP_LOGE(TAG, "Gemini API Server Error: %s", json_string);
        ai_cloud_abort();
        return;
    }

    // Chốt chặn 1 (State Guard): Nếu Gateway đang ở AI_STATE_IDLE (không có lượt nói nào diễn ra),
    // hủy bỏ ngay lập tức mọi gói tin trễ từ server để chống rò rỉ hoặc vòng lặp ngoài ý muốn.
    if (s_current_state == AI_STATE_IDLE) {
        return;
    }

    // 3. Xử lý nhận diện giọng nói Người Dùng (STT) nếu có
    const char *stt_tag = strstr(json_string, "\"inputTranscription\"");
    if (stt_tag) {
        const char *text_key = strstr(stt_tag, "\"text\"");
        if (text_key) {
            const char *val_start = strchr(text_key + 6, '\"');
            if (val_start) {
                val_start++;
                const char *val_end = strchr(val_start, '\"');
                while (val_end && *(val_end - 1) == '\\') {
                    val_end = strchr(val_end + 1, '\"');
                }
                if (val_end && val_end > val_start) {
                    char stt_buf[256];
                    size_t len = val_end - val_start;
                    if (len >= sizeof(stt_buf)) len = sizeof(stt_buf) - 1;
                    memcpy(stt_buf, val_start, len);
                    stt_buf[len] = '\0';
                    ESP_LOGW(TAG, "User Said (STT): \"%s\"", stt_buf);
                }
            }
        }
    }

    // 4. Trích xuất nội dung văn bản trả lời của Gemini (text)
    const char *text_source = strstr(json_string, "\"outputTranscription\"");
    if (!text_source) {
        text_source = strstr(json_string, "\"modelTurn\"");
    }
    if (text_source) {
        const char *text_key = strstr(text_source, "\"text\":");
        if (text_key) {
            const char *val_start = strchr(text_key + 7, '\"');
            if (val_start) {
                val_start++;
                const char *val_end = strchr(val_start, '\"');
                while (val_end && *(val_end - 1) == '\\') {
                    val_end = strchr(val_end + 1, '\"');
                }
                if (val_end && val_end > val_start) {
                    char reply_buf[256];
                    size_t len = val_end - val_start;
                    if (len >= sizeof(reply_buf)) len = sizeof(reply_buf) - 1;
                    memcpy(reply_buf, val_start, len);
                    reply_buf[len] = '\0';

                    // Lọc sạch các thẻ rác XML/HTML/SSML như \u003caudio src=\"say:...\"/\u003e
                    char *say_pos = strstr(reply_buf, "say:");
                    if (say_pos) {
                        say_pos += 4;
                        char *quote_end = strchr(say_pos, '\"');
                        if (quote_end) *quote_end = '\0';
                        ESP_LOGI(TAG, "Gemini Reply: %s", say_pos);
                    } else if (strstr(reply_buf, "\\u003c") || strstr(reply_buf, "<audio")) {
                        // Bỏ qua log thẻ rác XML
                    } else {
                        ESP_LOGI(TAG, "Gemini Reply: %s", reply_buf);
                    }
                }
            }
        }
    }

    // 4.1 Xử lý Function Calling chính thức từ Gemini Live (Gemini Tools)
    const char *tool_call = strstr(json_string, "\"toolCall\"");
    if (tool_call) {
        // Chốt chặn 2 (Duplicate Tool Abort): Nếu đã thực thi lệnh trong lượt này hoặc đang đợi phản hồi thoại sau hàm:
        // Phát hiện server gửi tin nhắn toolCall lặp lại bất thường -> Ngắt cứng WebSocket ngay để cắt đứt vòng lặp!
        if (s_cmd_dispatched_in_turn || s_current_state == AI_STATE_WAITING_POST_TOOL_SPEECH) {
            ESP_LOGW(TAG, ">>> [STATE GUARD] Phát hiện tin nhắn toolCall lặp lại bất thường! Ngắt kết nối WebSocket ngay lập tức để cắt đứt vòng lặp... <<<");
            ai_cloud_abort();
            return;
        }

        ESP_LOGI(TAG, ">>> [GEMINI TOOL CALL RECEIVED] <<<");
        s_current_state = AI_STATE_EXECUTING_TOOL;
        s_cmd_dispatched_in_turn = true;
        s_mute_response_audio = true; // Mute loa cho toàn bộ câu trả lời thoại sau lệnh điều khiển

        char func_responses_json[512] = {0};
        size_t resp_offset = 0;
        int call_count = 0;

        const char *fc_start = strstr(tool_call, "\"functionCalls\"");
        const char *arr_start = fc_start ? strchr(fc_start, '[') : NULL;
        const char *obj_search = arr_start ? arr_start : tool_call;

        while ((obj_search = strchr(obj_search, '{')) != NULL) {
            // Xác định phạm vi {...} của functionCall này bằng cách đếm ngoặc
            int depth = 0;
            const char *p = obj_search;
            const char *obj_end = NULL;
            while (*p) {
                if (*p == '{') depth++;
                else if (*p == '}') {
                    depth--;
                    if (depth == 0) {
                        obj_end = p;
                        break;
                    }
                }
                p++;
            }
            if (!obj_end) break;

            // Kiểm tra xem trong {...} này có chứa "name": không
            const char *name_key = strstr(obj_search, "\"name\":");
            if (!name_key || name_key > obj_end) {
                obj_search = obj_end + 1;
                continue;
            }

            // 1. Trích xuất fn_name
            char fn_name[64] = {0};
            const char *n_start = strchr(name_key + 7, '\"');
            if (n_start && n_start < obj_end) {
                n_start++;
                const char *n_end = strchr(n_start, '\"');
                if (n_end && n_end <= obj_end) {
                    size_t n_len = n_end - n_start;
                    if (n_len >= sizeof(fn_name)) n_len = sizeof(fn_name) - 1;
                    memcpy(fn_name, n_start, n_len);
                    fn_name[n_len] = '\0';
                }
            }

            // 2. Trích xuất call_id
            char call_id[64] = {0};
            const char *id_key = strstr(obj_search, "\"id\":");
            if (id_key && id_key < obj_end) {
                const char *id_start = strchr(id_key + 5, '\"');
                if (id_start && id_start < obj_end) {
                    id_start++;
                    const char *id_end = strchr(id_start, '\"');
                    if (id_end && id_end <= obj_end) {
                        size_t id_len = id_end - id_start;
                        if (id_len >= sizeof(call_id)) id_len = sizeof(call_id) - 1;
                        memcpy(call_id, id_start, id_len);
                        call_id[id_len] = '\0';
                    }
                }
            }

            // 3. Trích xuất action
            char action[32] = {0};
            const char *act_key = strstr(obj_search, "\"action\":");
            if (act_key && act_key < obj_end) {
                const char *a_start = strchr(act_key + 9, '\"');
                if (a_start && a_start < obj_end) {
                    a_start++;
                    const char *a_end = strchr(a_start, '\"');
                    if (a_end && a_end <= obj_end) {
                        size_t a_len = a_end - a_start;
                        if (a_len >= sizeof(action)) a_len = sizeof(action) - 1;
                        memcpy(action, a_start, a_len);
                        action[a_len] = '\0';
                    }
                }
            }

            // 4. Trích xuất brightness
            int brightness = -1;
            const char *b_key = strstr(obj_search, "\"brightness\":");
            if (b_key && b_key < obj_end) {
                const char *bp = b_key + 13;
                while (bp < obj_end && (*bp < '0' || *bp > '9') && *bp != '\"') bp++;
                if (bp < obj_end && *bp >= '0' && *bp <= '9') {
                    sscanf(bp, "%d", &brightness);
                }
            }

            ESP_LOGW(TAG, ">>> [TOOL PARSED #%d] Fn: '%s' | Action: '%s' | Bright: %d | ID: '%s' <<<",
                     call_count + 1, fn_name, action, brightness, call_id);

            char result_msg[128] = "Đã thực hiện";

            if (strcmp(fn_name, "control_dimmer") == 0) {
                if (strcmp(action, "set_brightness") == 0 || brightness >= 0) {
                    if (brightness < 0) brightness = 80;
                    if (brightness > 100) brightness = 100;
                    char cmd_buf[32];
                    snprintf(cmd_buf, sizeof(cmd_buf), "CMD:DIMMER:SET:%d", brightness);
                    ESP_LOGW(TAG, ">>> [TOOL EXEC -> THREAD MESH]: %s <<<", cmd_buf);
                    mesh_controller_send_command(cmd_buf);
                    snprintf(result_msg, sizeof(result_msg), "Đã chỉnh độ sáng đèn lên %d%%", brightness);
                } else if (strcmp(action, "turn_on") == 0 || strcmp(action, "on") == 0) {
                    ESP_LOGW(TAG, ">>> [TOOL EXEC -> THREAD MESH]: CMD:DIMMER:ON <<<");
                    mesh_controller_send_command("CMD:DIMMER:ON");
                    snprintf(result_msg, sizeof(result_msg), "Đã bật đèn");
                } else if (strcmp(action, "turn_off") == 0 || strcmp(action, "off") == 0) {
                    ESP_LOGW(TAG, ">>> [TOOL EXEC -> THREAD MESH]: CMD:DIMMER:OFF <<<");
                    mesh_controller_send_command("CMD:DIMMER:OFF");
                    snprintf(result_msg, sizeof(result_msg), "Đã tắt đèn");
                }
            } else if (strcmp(fn_name, "control_relay") == 0) {
                if (strcmp(action, "turn_on") == 0 || strcmp(action, "on") == 0) {
                    ESP_LOGW(TAG, ">>> [TOOL EXEC -> THREAD MESH]: CMD:RELAY:ON <<<");
                    mesh_controller_send_command("CMD:RELAY:ON");
                    snprintf(result_msg, sizeof(result_msg), "Đã bật công tắc");
                } else if (strcmp(action, "turn_off") == 0 || strcmp(action, "off") == 0) {
                    ESP_LOGW(TAG, ">>> [TOOL EXEC -> THREAD MESH]: CMD:RELAY:OFF <<<");
                    mesh_controller_send_command("CMD:RELAY:OFF");
                    snprintf(result_msg, sizeof(result_msg), "Đã tắt công tắc");
                }
            }

            // Ghi nhận functionResponse cho ID tương ứng
            if (call_id[0] != '\0') {
                char single_resp[256];
                int s_len = snprintf(single_resp, sizeof(single_resp),
                    "%s{\"response\":{\"output\":{\"result\":\"%s\"}},\"id\":\"%s\"}",
                    (call_count > 0 ? "," : ""), result_msg, call_id
                );
                if (resp_offset + s_len < sizeof(func_responses_json)) {
                    memcpy(func_responses_json + resp_offset, single_resp, s_len);
                    resp_offset += s_len;
                    func_responses_json[resp_offset] = '\0';
                }
            }

            call_count++;
            obj_search = obj_end + 1;
        }

        // Gửi toolResponse phản hồi cho Google Live API để hoàn tất giao thức
        if (call_count > 0 && ws_client && s_is_ws_connected) {
            char tool_resp_buf[768];
            snprintf(tool_resp_buf, sizeof(tool_resp_buf),
                "{\"toolResponse\":{\"functionResponses\":[%s]}}",
                func_responses_json
            );
            if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(300)) == pdTRUE) {
                esp_websocket_client_send_text(ws_client, tool_resp_buf, strlen(tool_resp_buf), pdMS_TO_TICKS(4000));
                ESP_LOGI(TAG, ">>> Sent toolResponse (%d functions) to Gemini Live <<<", call_count);
                xSemaphoreGive(ws_mutex);
            }
        }

        // [LOCAL-SYNC SILENT EXIT]: Điều khiển thiết bị xong -> Giữ loa 100% im lặng, chờ turnComplete kết thúc lượt
        ESP_LOGI(TAG, ">>> [LOCAL-SYNC SILENT EXIT] Lệnh điều khiển đã gửi -> Mute loa, chờ turnComplete... <<<");
        s_current_state = AI_STATE_WAITING_POST_TOOL_SPEECH;
        audio_hal_stop_playback();
        audio_hal_set_turn_active(false);

        // Khởi động lại Safeguard Timer 3 giây bảo vệ: nếu quá 3s Google không gửi turnComplete thì timeout đóng socket
        if (s_session_timer) {
            xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(3000), 0);
            xTimerStart(s_session_timer, 0);
        }
    }

    // 4.2 Trích xuất mã lệnh điều khiển Thread Mesh (<CMD:...> hoặc CMD:...) gửi tới ESP32-H2 (Dự phòng Fallback)
    if (!s_cmd_dispatched_in_turn) {
        const char *cmd_start = strstr(json_string, "CMD:");
        if (cmd_start) {
            if (cmd_start > json_string && *(cmd_start - 1) == '<') {
                cmd_start--;
            }
            const char *p = cmd_start;
            while (*p && *p != '>' && *p != '\"' && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\\') {
                p++;
            }
            if (*p == '>') p++;
            size_t cmd_len = p - cmd_start;
            if (cmd_len > 4 && cmd_len < 64) {
                char clean_cmd[64];
                memcpy(clean_cmd, cmd_start, cmd_len);
                clean_cmd[cmd_len] = '\0';
                s_cmd_dispatched_in_turn = true;
                s_mute_response_audio = true;
                ESP_LOGW(TAG, ">>> [THREAD MESH GATEWAY] Dispatching command to ESP32-H2: %s <<<", clean_cmd);
                mesh_controller_send_command(clean_cmd);
            }
        }
    }

    // 5. Fast-Path: Trích xuất Audio PCM (Base64) từ inlineData - ZERO MALLOC & ZERO HEAP CONTENTION
    const char *search_ptr = json_string;
    const char *inline_tag = NULL;
    while ((inline_tag = strstr(search_ptr, "\"inlineData\"")) != NULL) {
        const char *data_key = strstr(inline_tag, "\"data\":");
        if (!data_key) break;
        const char *val_start = strchr(data_key + 7, '\"');
        if (!val_start) break;
        val_start++;
        const char *val_end = strchr(val_start, '\"');
        if (!val_end || val_end <= val_start) break;
        size_t b64_len = val_end - val_start;
        search_ptr = val_end + 1;

        // Cập nhật trạng thái Gateway
        if (s_current_state == AI_STATE_WAITING_AI || s_current_state == AI_STATE_WAITING_POST_TOOL_SPEECH) {
            if (!s_mute_response_audio) {
                s_current_state = AI_STATE_SPEAKING;
            }
            if (s_session_timer) {
                xTimerStop(s_session_timer, 0);
            }
        } else if (s_current_state == AI_STATE_SPEAKING) {
            // Đang phát luồng âm thanh bình thường
        } else if (s_current_state == AI_STATE_RECORDING) {
            ESP_LOGI(TAG, ">>> [SERVER VAD] Gemini phát hiện dứt câu sớm & bắt đầu trả lời! Dừng thu âm.");
            if (!s_mute_response_audio) {
                s_current_state = AI_STATE_SPEAKING;
            }
            ai_edge_stop_recording();
            expecting_first_chunk = true;
            if (s_session_timer) {
                xTimerStop(s_session_timer, 0);
            }
        } else {
            ESP_LOGD(TAG, "Dropping lingering audio chunk in state %d", s_current_state);
            continue;
        }

        if (s_current_state == AI_STATE_SPEAKING && !s_mute_response_audio) {
            if (expecting_first_chunk) {
                expecting_first_chunk = false;
                audio_hal_start_buffering(); // Kích hoạt đệm trước 700ms (~33KB) chống giật
                audio_hal_set_turn_active(true);
            }

            size_t req_pcm_size = (b64_len / 4) * 3 + 4;
            if (s_rx_pcm_buf && req_pcm_size <= RX_PCM_BUF_SIZE) {
                size_t pcm_len = 0;
                int ret = mbedtls_base64_decode(s_rx_pcm_buf, RX_PCM_BUF_SIZE, &pcm_len,
                                                (const unsigned char *)val_start, b64_len);
                if (ret == 0 && pcm_len > 0) {
                    pcm_len &= ~1;

                    // Áp dụng Headroom 80% (-2 dBFS) chống méo tiếng / rè loa MAX98357A
                    int16_t *samples = (int16_t *)s_rx_pcm_buf;
                    size_t num_samples = pcm_len / 2;
                    for (size_t i = 0; i < num_samples; i++) {
                        samples[i] = (int16_t)(((int32_t)samples[i] * 80) / 100);
                    }

                    audio_hal_play_chunk(s_rx_pcm_buf, pcm_len);
                }
            } else {
                ESP_LOGE(TAG, "Audio chunk too large for PCM buffer! req=%u, max=%u", (unsigned int)req_pcm_size, (unsigned int)RX_PCM_BUF_SIZE);
            }
        }
    }

    // 6. Xử lý kết thúc lượt nói từ Model
    bool is_turn_finished = false;
    bool has_turn_complete = false;

    // 6.1 Kiểm tra finishReason (STOP, SAFETY, MAX_TOKENS, RECITATION, v.v.)
    const char *finish_tag = strstr(json_string, "\"finishReason\"");
    if (finish_tag) {
        char reason_str[32] = {0};
        const char *val_start = strchr(finish_tag + 14, '\"');
        if (val_start) {
            val_start++;
            const char *val_end = strchr(val_start, '\"');
            if (val_end && val_end > val_start) {
                size_t rlen = val_end - val_start;
                if (rlen >= sizeof(reason_str)) rlen = sizeof(reason_str) - 1;
                memcpy(reason_str, val_start, rlen);
                reason_str[rlen] = '\0';
            }
        }
        ESP_LOGW(TAG, ">>> [FINISH REASON] Gemini ket thuc phan hoi do: %s <<<", reason_str[0] ? reason_str : "UNKNOWN");
        is_turn_finished = true;
    }

    // 6.2 Kiểm tra turnComplete linh hoạt (miễn nhiễm mọi khoảng trắng / xuống dòng)
    const char *tc_tag = strstr(json_string, "\"turnComplete\"");
    if (tc_tag) {
        const char *p = tc_tag + 14;
        while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (strncmp(p, "true", 4) == 0) {
            has_turn_complete = true;
            is_turn_finished = true;
        }
    }

    // 6.3 Kiểm tra interrupted / generationComplete
    const char *intr_tag = strstr(json_string, "\"interrupted\"");
    if (intr_tag) {
        const char *p = intr_tag + 13;
        while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (strncmp(p, "true", 4) == 0) {
            is_turn_finished = true;
        }
    }
    const char *gc_tag = strstr(json_string, "\"generationComplete\"");
    if (gc_tag) {
        const char *p = gc_tag + 20;
        while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (strncmp(p, "true", 4) == 0) {
            is_turn_finished = true;
        }
    }

    // Nếu đang trong pha gọi hàm (EXECUTING_TOOL) hoặc đang chờ phản hồi thoại sau hàm (WAITING_POST_TOOL_SPEECH):
    // Bỏ qua generationComplete đơn lẻ, CHỈ chấp nhận turnComplete: true chính thức kết thúc toàn bộ lượt!
    if (s_current_state == AI_STATE_EXECUTING_TOOL) {
        is_turn_finished = false;
    } else if (s_current_state == AI_STATE_WAITING_POST_TOOL_SPEECH && !has_turn_complete) {
        is_turn_finished = false;
    }

    if (is_turn_finished && s_current_state != AI_STATE_IDLE) {
        ESP_LOGI(TAG, "Model turn complete. Waiting for next user input...");
        bool was_muted = s_mute_response_audio;
        s_current_state = AI_STATE_IDLE;
        s_cmd_dispatched_in_turn = false;
        s_mute_response_audio = false;
        if (s_session_timer) {
            xTimerStop(s_session_timer, 0);
        }
        if (!was_muted) {
            audio_hal_force_start_playing();
        }
        audio_hal_set_turn_active(false);
    }

    // 7. Log toan bo sieu du lieu (Metadata / Event) co y nghia tu Google (bo qua heartbeat rong {})
    if (!inline_tag && !text_source && !stt_tag && !strstr(json_string, "\"setupComplete\"")) {
        if (strcmp(json_string, "{}") != 0 &&
            !strstr(json_string, "\"serverContent\":{}") &&
            !strstr(json_string, "\"serverContent\": {}")) {
            char preview_buf[256];
            size_t slen = strlen(json_string);
            if (slen >= sizeof(preview_buf)) slen = sizeof(preview_buf) - 1;
            memcpy(preview_buf, json_string, slen);
            preview_buf[slen] = '\0';
            ESP_LOGI(TAG, "Gemini Server Event: %s", preview_buf);
        }
    }
}
static void send_setup_frame(void) {
    if (ws_client && s_is_ws_connected) {
        s_setup_complete = false;

        gateway_device_state_t st;
        gateway_state_get(&st);

        char prompt_buf[1536];
        snprintf(prompt_buf, sizeof(prompt_buf),
            "Bạn là trợ lý Nhà Thông Minh trên Gateway ESP32 điều khiển mạng Thread Mesh. "
            "TRẠNG THÁI THIẾT BỊ HIỆN TẠI TRONG NHÀ: "
            "1. Đèn Dimming: Trạng thái %s, Độ sáng %u%% (Mức nhớ gần nhất: %u%%). "
            "2. Công tắc Relay 1 kênh: Trạng thái %s. "
            "3. Cảm biến DHT11: Nhiệt độ phòng %.1f độ C, Độ ẩm %.1f%%. "
            "QUY TẮC BẮT BUỘC: "
            "1. Luôn dùng TIẾNG VIỆT tự nhiên, thân thiện. Tuyệt đối không dùng tiếng nước ngoài. "
            "2. Khi người dùng yêu cầu bật, tắt hoặc chỉnh độ sáng đèn: BẮT BUỘC gọi hàm control_dimmer. "
            "3. Khi người dùng yêu cầu bật hoặc tắt công tắc: BẮT BUỘC gọi hàm control_relay. "
            "4. TUYỆT ĐỐI KHÔNG gọi lại hàm lần thứ 2 trong cùng một lượt. "
            "5. Khi gọi hàm điều khiển thiết bị: Sau khi nhận kết quả hàm, chỉ cần xác nhận cực kỳ ngắn gọn (dưới 4 từ, ví dụ: 'Đã bật đèn.', 'Đã tắt quạt.') và kết thúc lượt ngay. "
            "6. Khi người dùng hỏi nhiệt độ, độ ẩm phòng (từ cảm biến DHT11) hoặc trò chuyện, hỏi đáp kiến thức: BẮT BUỘC trả lời tự nhiên bằng giọng nói qua loa, không gọi hàm. "
            "7. Nếu nghe không rõ hoặc âm thanh lạ, chỉ nói đúng: 'Tôi chưa nghe rõ, bạn vui lòng nói lại nhé.'. "
            "8. Tuyệt đối KHÔNG tự ý xuất bất kỳ thẻ XML, HTML hay SSML nào như <audio>.",
            st.dimmer_on ? "BẬT" : "TẮT",
            st.dimmer_brightness,
            st.dimmer_last_brightness > 0 ? st.dimmer_last_brightness : 80,
            st.relay_on ? "BẬT" : "TẮT",
            st.sensor_valid ? st.temperature : 28.0f,
            st.sensor_valid ? st.humidity : 65.0f
        );

        const char *tools_json = 
            "\"tools\":[{\"functionDeclarations\":["
                "{\"name\":\"control_dimmer\",\"description\":\"Điều khiển đèn Dimming. Bật, tắt hoặc chỉnh độ sáng 0-100%.\","
                "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
                    "\"action\":{\"type\":\"STRING\",\"description\":\"Hành động: 'turn_on', 'turn_off', hoặc 'set_brightness'.\"},"
                    "\"brightness\":{\"type\":\"INTEGER\",\"description\":\"Độ sáng từ 0 đến 100% khi action là set_brightness hoặc khi bật ở mức cụ thể.\"}"
                "},\"required\":[\"action\"]}},"
                "{\"name\":\"control_relay\",\"description\":\"Điều khiển công tắc Relay 1 kênh (bật hoặc tắt).\" ,"
                "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
                    "\"action\":{\"type\":\"STRING\",\"description\":\"Hành động: 'turn_on' hoặc 'turn_off'.\"}"
                "},\"required\":[\"action\"]}}"
            "]}],";

        char setup_buf[3584];
        snprintf(setup_buf, sizeof(setup_buf),
            "{\"setup\":{\"model\":\"models/gemini-3.8-live\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]},"
            "%s"
            "\"systemInstruction\":{\"parts\":[{\"text\":\"%s\"}]},"
            "\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":true}},\"inputAudioTranscription\":{}}}",
            tools_json,
            prompt_buf
        );

        esp_websocket_client_send_text(ws_client, setup_buf, strlen(setup_buf), pdMS_TO_TICKS(3000));
        ESP_LOGI(TAG, "Sent Dynamic Setup message with Gemini Tools (control_dimmer, control_relay). Waiting for setupComplete...");
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, ">>> WEBSOCKET_EVENT_CONNECTED <<<");
        s_is_ws_connected = true;
        s_is_ws_connecting = false;
        send_setup_frame();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        s_is_ws_connected = false;
        s_is_ws_connecting = false;
        s_setup_complete = false;
        s_last_disconnect_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        s_ws_rx_len = 0;
        if (s_current_state == AI_STATE_RECORDING || s_current_state == AI_STATE_WAITING_AI || s_current_state == AI_STATE_SPEAKING) {
            ESP_LOGE(TAG, "WebSocket disconnected mid-session! Aborting turn...");
            ai_cloud_abort();
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        s_is_ws_connected = false;
        s_is_ws_connecting = false;
        s_setup_complete = false;
        s_last_disconnect_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (s_current_state == AI_STATE_RECORDING || s_current_state == AI_STATE_WAITING_AI || s_current_state == AI_STATE_SPEAKING) {
            ESP_LOGE(TAG, "WebSocket disconnected mid-session! Aborting turn...");
            ai_cloud_abort();
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x08) {
            uint16_t close_code = 0;
            char reason_buf[128] = {0};
            if (data->data_ptr && data->data_len >= 2) {
                close_code = (((uint8_t)data->data_ptr[0]) << 8) | ((uint8_t)data->data_ptr[1]);
                int rlen = data->data_len - 2;
                if (rlen > 0) {
                    if (rlen >= sizeof(reason_buf)) rlen = sizeof(reason_buf) - 1;
                    memcpy(reason_buf, data->data_ptr + 2, rlen);
                    reason_buf[rlen] = '\0';
                }
            }
            ESP_LOGE(TAG, ">>> WS CLOSE FRAME from Google: Code %u | Reason: '%s' <<<", close_code, reason_buf);
            s_is_ws_connected = false;
            s_is_ws_connecting = false;
            s_setup_complete = false;
            s_last_disconnect_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            if (s_current_state == AI_STATE_RECORDING || s_current_state == AI_STATE_WAITING_AI || s_current_state == AI_STATE_SPEAKING) {
                ai_cloud_abort();
            }
            break;
        }

        if (data->op_code == 0x09) {
            // Ping Frame
            break;
        }

        if (data->data_ptr && data->data_len > 0) {
            // Reset timer ngay khi nhan bat ky manh du lieu Wi-Fi nao tu Google (xac nhan mang dang truyen tai)
            if (s_current_state == AI_STATE_SPEAKING && s_session_timer) {
                xTimerReset(s_session_timer, 0);
            }
            if (data->payload_offset == 0) {
                s_ws_rx_len = 0;
            } else if (data->payload_offset != s_ws_rx_len) {
                static uint32_t s_last_offset_err_ms = 0;
                uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                if (now_ms - s_last_offset_err_ms > 2000) {
                    s_last_offset_err_ms = now_ms;
                    ESP_LOGW(TAG, "WS payload offset mismatch! (Exp: %u, Got: %u) - Dropping frame",
                             (unsigned int)s_ws_rx_len, (unsigned int)data->payload_offset);
                }
                s_ws_rx_len = 0;
                break;
            }
            
            if (s_ws_rx_buf && (s_ws_rx_len + data->data_len < WS_RX_BUF_SIZE)) {
                memcpy(s_ws_rx_buf + s_ws_rx_len, data->data_ptr, data->data_len);
                s_ws_rx_len += data->data_len;
            } else {
                static uint32_t s_last_overflow_err_ms = 0;
                uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                if (now_ms - s_last_overflow_err_ms > 2000) {
                    s_last_overflow_err_ms = now_ms;
                    ESP_LOGE(TAG, "s_ws_rx_buf overflow! (len: %u + inc: %u > max: %u)",
                             (unsigned int)s_ws_rx_len, (unsigned int)data->data_len, (unsigned int)WS_RX_BUF_SIZE);
                }
                s_ws_rx_len = 0;
                break;
            }
            
            if (s_ws_rx_len == data->payload_len) {
                s_ws_rx_buf[s_ws_rx_len] = '\0';
                parse_server_message(s_ws_rx_buf);
                s_ws_rx_len = 0;
            }
        }
        break;
    }
}

// Luồng truyền âm thanh độc lập & FSM Worker (Consumer):
// Tiếp nhận các cờ sự kiện điều khiển (Task Notification Bitmask) và stream âm thanh từ RingBuffer TX.
static void cloud_tx_task(void *arg) {
    ESP_LOGI(TAG, "Cloud TX Task Started (Dedicated Asynchronous Streaming & FSM Worker).");

    while (1) {
        // 1. Kiểm tra và nhận tín hiệu điều khiển bất đồng bộ (Task Notification)
        uint32_t notify_bits = 0;
        BaseType_t got_notify = xTaskNotifyWait(0, 0xFFFFFFFF, &notify_bits, 
            (s_current_state == AI_STATE_IDLE && !s_turn_ending) ? pdMS_TO_TICKS(50) : 0);

        if (got_notify == pdTRUE) {
            // Xử lý ABORT (Lỗi mạng hoặc ngắt khẩn cấp)
            if (notify_bits & CLOUD_EVT_ABORT) {
                ESP_LOGW(TAG, "Worker handling CLOUD_EVT_ABORT (Stopping WebSocket & resetting FSM)...");
                bool was_tool = (s_current_state == AI_STATE_WAITING_POST_TOOL_SPEECH || s_mute_response_audio);
                s_current_state = AI_STATE_IDLE;
                s_turn_ending = false;
                s_need_send_activity_start = false;
                s_cmd_dispatched_in_turn = false;
                s_mute_response_audio = false;
                ai_edge_stop_recording();
                if (s_session_timer) xTimerStop(s_session_timer, 0);
                if (s_tx_audio_ringbuf) {
                    size_t dummy_sz;
                    uint8_t *dummy_it;
                    while ((dummy_it = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_sz, 0)) != NULL) {
                        vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_it);
                    }
                }
                audio_hal_stop_playback();
                audio_hal_set_turn_active(false);
                if (!was_tool) {
                    audio_hal_play_error_sound();
                }
                if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    if (ws_client) {
                        s_is_ws_stopping = true;
                        esp_websocket_client_stop(ws_client);
                        s_is_ws_stopping = false;
                    }
                    s_is_ws_connected = false;
                    s_is_ws_connecting = false;
                    s_setup_complete = false;
                    s_last_disconnect_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                    xSemaphoreGive(ws_mutex);
                }
                continue;
            }

            // Xử lý TIMEOUT từ Software Timer (chuyển về xử lý an toàn tại đây với Stack 8KB)
            if (notify_bits & CLOUD_EVT_TIMEOUT) {
                if (s_current_state == AI_STATE_WAITING_AI || s_current_state == AI_STATE_WAITING_POST_TOOL_SPEECH) {
                    bool was_tool = (s_current_state == AI_STATE_WAITING_POST_TOOL_SPEECH || s_mute_response_audio);
                    ESP_LOGW(TAG, "Response Timeout (waiting AI or post-tool ack). Resetting turn & closing socket...");
                    s_current_state = AI_STATE_IDLE;
                    s_turn_ending = false;
                    s_need_send_activity_start = false;
                    s_cmd_dispatched_in_turn = false;
                    s_mute_response_audio = false;
                    ai_edge_stop_recording();
                    if (s_session_timer) xTimerStop(s_session_timer, 0);
                    audio_hal_set_turn_active(false);
                    if (!was_tool) {
                        audio_hal_play_error_sound();
                    }
                    if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                        if (ws_client) {
                            s_is_ws_stopping = true;
                            esp_websocket_client_stop(ws_client);
                            s_is_ws_stopping = false;
                        }
                        s_is_ws_connected = false;
                        s_is_ws_connecting = false;
                        s_setup_complete = false;
                        s_last_disconnect_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                        xSemaphoreGive(ws_mutex);
                    }
                    continue;
                }
            }

            // Xử lý CANCEL (Khớp lệnh cục bộ Model 2 hoặc No-Speech) -> Thoát êm đẹp không kêu lỗi
            if (notify_bits & CLOUD_EVT_CANCEL) {
                ESP_LOGI(TAG, "Worker handling CLOUD_EVT_CANCEL (Local match or no-speech)...");
                s_current_state = AI_STATE_IDLE;
                s_turn_ending = false;
                s_need_send_activity_start = false;
                s_cmd_dispatched_in_turn = false;
                s_mute_response_audio = false;
                if (s_session_timer) xTimerStop(s_session_timer, 0);
                if (s_tx_audio_ringbuf) {
                    size_t dummy_sz;
                    uint8_t *dummy_it;
                    while ((dummy_it = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_sz, 0)) != NULL) {
                        vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_it);
                    }
                }
                if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    if (ws_client) {
                        s_is_ws_stopping = true;
                        esp_websocket_client_stop(ws_client);
                        s_is_ws_stopping = false;
                    }
                    s_is_ws_connected = false;
                    s_is_ws_connecting = false;
                    s_setup_complete = false;
                    s_last_disconnect_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                    xSemaphoreGive(ws_mutex);
                }
                continue;
            }

            // Xử lý KEEP_ALIVE_EXP
            if (notify_bits & CLOUD_EVT_KEEP_ALIVE_EXP) {
                if (s_current_state == AI_STATE_IDLE && !s_turn_ending) {
                    ESP_LOGI(TAG, ">>> [KEEP-ALIVE EXPIRED] Hết 15s không có câu lệnh mới -> Đóng WebSocket an toàn từ Worker Task. <<<");
                    if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                        if (ws_client) {
                            s_is_ws_stopping = true;
                            esp_websocket_client_stop(ws_client);
                            s_is_ws_stopping = false;
                        }
                        s_is_ws_connected = false;
                        s_is_ws_connecting = false;
                        s_setup_complete = false;
                        s_last_disconnect_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                        xSemaphoreGive(ws_mutex);
                    }
                    continue;
                }
            }

            // Xử lý START_TURN
            if (notify_bits & CLOUD_EVT_START_TURN) {
                s_current_state = AI_STATE_RECORDING;
                s_need_send_activity_start = true;
                s_cmd_dispatched_in_turn = false;
            }

            // Xử lý END_TURN
            if (notify_bits & CLOUD_EVT_END_TURN) {
                s_turn_ending = true;
            }
        }

        // 2. Khi ở trạng thái rảnh (IDLE) và không có lượt đang dứt:
        if (s_current_state == AI_STATE_IDLE && !s_turn_ending) {
            // Xả sạch triệt để TOÀN BỘ byte tồn dư nào còn sót lại trong RingBuffer TX
            if (s_tx_audio_ringbuf) {
                size_t dummy_size = 0;
                uint8_t *dummy_item = NULL;
                while ((dummy_item = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_size, 0)) != NULL) {
                    vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_item);
                }
            }
            continue;
        }

        // 2. Khi đang ghi âm hoặc đang chờ xả hết âm thanh (turn ending):
        // Nếu WebSocket chưa kết nối hoặc chưa hoàn tất Setup -> ĐỢI, TUYỆT ĐỐI KHÔNG LẤY RA XẢ BỎ AUDIO!
        if (!s_is_ws_connected || !s_setup_complete) {
            // Đảm bảo lệnh kết nối đã được phát động
            ai_cloud_prewarm_session();

            int wait_ticks = 0;
            // Chờ đến khi cả WebSocket kết nối VÀ Setup hoàn tất (hoặc tối đa 5 giây khi đang kết nối/chờ setup)
            while ((!s_is_ws_connected || !s_setup_complete) && 
                   wait_ticks < 50 && 
                   (s_current_state == AI_STATE_RECORDING || s_turn_ending)) {
                // Kiểm tra notification trong lúc chờ (ví dụ nếu có CANCEL/ABORT trong lúc chờ kết nối)
                uint32_t wait_bits = 0;
                if (xTaskNotifyWait(0, 0xFFFFFFFF, &wait_bits, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (wait_bits & (CLOUD_EVT_ABORT | CLOUD_EVT_CANCEL)) {
                        s_current_state = AI_STATE_IDLE;
                        s_turn_ending = false;
                        break;
                    }
                }
                wait_ticks++;
            }

            // NẾU TRONG LÚC CHỜ, LƯỢT NÓI ĐÃ BỊ HỦY HOẶC KẾT THÚC (ví dụ: khớp lệnh cục bộ Model 2 -> State về IDLE):
            // THOÁT NGAY VỀ ĐẦU VÒNG LẶP TRONG YÊN LẶNG (Silent Graceful Exit), TUYỆT ĐỐI KHÔNG BÁO LỖI!
            if (s_current_state == AI_STATE_IDLE && !s_turn_ending) {
                ESP_LOGI(TAG, "Turn cancelled or finished during connection wait (Local match or idle). Exiting cleanly.");
                continue;
            }

            if (!s_is_ws_connected || !s_setup_complete) {
                ESP_LOGE(TAG, "WebSocket connection failed or setup timeout! Aborting turn...");
                ai_cloud_abort();
                vTaskDelay(pdMS_TO_TICKS(500)); // Ngăn ngừa 100% spin loop nghẽn CPU
                continue;
            }
        }

        // Kiểm tra an toàn trước khi xử lý các bước tiếp theo
        if (s_current_state == AI_STATE_IDLE && !s_turn_ending) {
            continue;
        }

        // 3. Khi WebSocket đã sẵn sàng: Gửi activityStart trước nếu chưa gửi
        if (s_need_send_activity_start) {
            if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                if (ws_client && s_is_ws_connected && s_setup_complete) {
                    const char *start_msg = "{\"realtimeInput\":{\"activityStart\":{}}}";
                    int sent = esp_websocket_client_send_text(ws_client, start_msg, strlen(start_msg), pdMS_TO_TICKS(2000));
                    if (sent > 0) {
                        ESP_LOGI(TAG, "Sent activityStart");
                        s_need_send_activity_start = false;
                    }
                }
                xSemaphoreGive(ws_mutex);
            }
        }

        // Bắt buộc activityStart phải được gửi thành công trước khi gửi bất kỳ chunk audio nào
        if (s_need_send_activity_start) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // 4. Lấy từng chunk âm thanh từ RingBuffer để stream lên WebSocket (Chuẩn hóa 1920 bytes = 60ms để frame JSON ~2.7KB vừa khít 2 packet TCP MSS)
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(s_tx_audio_ringbuf, &item_size, pdMS_TO_TICKS(20), 1920);

        if (item && item_size > 0) {
            // Đảm bảo số bytes là bội số của 2 (16-bit PCM)
            if (item_size % 2 != 0) {
                item_size &= ~1;
            }

            if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (ws_client && s_is_ws_connected && s_setup_complete) {
                    if (s_tx_base64_buf && s_tx_json_buf) {
                        size_t base64_len = 0;
                        if (mbedtls_base64_encode(s_tx_base64_buf, TX_BASE64_BUF_SIZE - 1, &base64_len, item, item_size) == 0) {
                            s_tx_base64_buf[base64_len] = '\0';

                            int json_len = snprintf(s_tx_json_buf, TX_JSON_BUF_SIZE,
                                     "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}",
                                     (char *)s_tx_base64_buf);
                            
                            if (json_len > 0 && json_len < TX_JSON_BUF_SIZE) {
                                int sent = esp_websocket_client_send_text(ws_client, s_tx_json_buf, json_len, pdMS_TO_TICKS(6000));
                                if (sent <= 0) {
                                    ESP_LOGW(TAG, "Audio send_text poll timeout or write fail (sent=%d)", sent);
                                }
                            }
                        }
                    }
                }
                xSemaphoreGive(ws_mutex);
            }

            vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)item);

            // Điều tiết tốc độ (Traffic Pacing): Nghỉ 35ms sau mỗi frame 60ms PCM.
            // Vừa xả nhanh hơn thời gian thực (~1.7x), vừa bảo đảm TCP Send Buffer chịu được jitter/DELBA Wi-Fi!
            vTaskDelay(pdMS_TO_TICKS(35));
        } else {
            // RingBuffer đã rỗng hoàn toàn (toàn bộ âm thanh câu nói đã được gửi lên Google)
            if (s_turn_ending && s_current_state == AI_STATE_RECORDING) {
                s_turn_ending = false;
                s_current_state = AI_STATE_WAITING_AI;
                expecting_first_chunk = true;

                if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    if (ws_client != NULL && s_is_ws_connected) {
                        const char *activity_end_msg = "{\"realtimeInput\":{\"activityEnd\":{}}}";
                        int sent = esp_websocket_client_send_text(ws_client, activity_end_msg, strlen(activity_end_msg), pdMS_TO_TICKS(4000));
                        if (sent > 0) {
                            ESP_LOGI(TAG, "Sent activityEnd (turn complete, all audio flushed). Waiting for AI response...");
                            if (s_session_timer) {
                                xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS), 0);
                                xTimerStart(s_session_timer, 0);
                            }
                        } else {
                            ESP_LOGE(TAG, "Failed to send activityEnd! Aborting turn...");
                            ai_cloud_abort();
                        }
                    }
                    xSemaphoreGive(ws_mutex);
                }
            }
        }
    }
}

static void ws_keep_alive_timer_cb(TimerHandle_t xTimer) {
    if (s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_KEEP_ALIVE_EXP, eSetBits);
    }
}

void ai_cloud_prewarm_session(void) {
    // 1. Dừng bộ đếm Keep-Alive nếu đang chạy (người dùng gọi tiếp câu lệnh)
    if (s_keep_alive_timer) {
        xTimerStop(s_keep_alive_timer, 0);
    }

    if (s_is_ws_stopping) {
        ESP_LOGD(TAG, "WebSocket client is currently stopping. Skipping prewarm.");
        return;
    }

    // Rate-limit backoff: Chờ tối thiểu 2.0s sau khi đứt kết nối / hủy trước khi cho phép kết nối lại
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_last_disconnect_time_ms > 0 && (now - s_last_disconnect_time_ms < 2000)) {
        ESP_LOGD(TAG, "WebSocket reconnect cooldown active (%lu ms remaining)...",
                 (unsigned long)(2000 - (now - s_last_disconnect_time_ms)));
        return;
    }

    if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (ws_client == NULL) {
            ESP_LOGI(TAG, "Initializing WebSocket connection to Gemini Live...");
            esp_websocket_client_config_t websocket_cfg = {
                .uri = GEMINI_WS_URL,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .buffer_size = 16384, // 16KB: Tiết kiệm 16KB Internal SRAM cho phần cứng DMA AES
                .task_stack = 8192,
                .task_core_id_set = true,
                .task_core_id = 0,    // Ghim luồng WebSocket cố định vào Core 0 (Nhân Mạng)
                .task_prio = 5,
                .network_timeout_ms = 10000,
                .reconnect_timeout_ms = 0,
                .disable_auto_reconnect = true,
            };
            ws_client = esp_websocket_client_init(&websocket_cfg);
            esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)ws_client);
            s_is_ws_connecting = true;
            esp_websocket_client_start(ws_client);
        } else if (s_is_ws_connected && s_setup_complete) {
            ESP_LOGI(TAG, ">>> [KEEP-ALIVE POOL HIT] WebSocket đã kết nối & Setup sẵn sàng (0ms độ trễ TLS)! <<<");
        } else if (!s_is_ws_connected && !s_is_ws_connecting && !s_is_ws_stopping) {
            ESP_LOGI(TAG, "Starting WebSocket in background...");
            s_is_ws_connecting = true;
            esp_err_t err = esp_websocket_client_start(ws_client);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_websocket_client_start failed: 0x%x -> Resetting client", err);
                esp_websocket_client_stop(ws_client);
                s_is_ws_connecting = false;
            }
        }
        xSemaphoreGive(ws_mutex);
    }
}

void ai_cloud_init(void) {
    ws_mutex = xSemaphoreCreateMutex();

    s_tx_base64_buf = (unsigned char *)heap_caps_malloc(TX_BASE64_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_tx_json_buf = (char *)heap_caps_malloc(TX_JSON_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_ws_rx_buf = (char *)heap_caps_malloc(WS_RX_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_rx_pcm_buf = (unsigned char *)heap_caps_malloc(RX_PCM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_tx_base64_buf || !s_tx_json_buf || !s_ws_rx_buf || !s_rx_pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate static PSRAM buffers for TX/RX!");
    }

    // Khởi tạo TX Audio RingBuffer 256KB trong PSRAM (chứa được ~8 giây âm thanh PCM 16kHz không bao giờ tràn)
    s_tx_audio_ringbuf = xRingbufferCreateWithCaps(256 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!s_tx_audio_ringbuf) {
        ESP_LOGE(TAG, "Failed to create 256KB TX RingBuffer in PSRAM! Fallback to 128KB...");
        s_tx_audio_ringbuf = xRingbufferCreateWithCaps(128 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    }

    // Tạo luồng phát âm thanh mạng độc lập & FSM Worker (Core 0, Priority 5, Stack 8KB)
    xTaskCreatePinnedToCore(cloud_tx_task, "cloud_tx_task", 8192, NULL, 5, &s_tx_task_handle, 0);

    s_session_timer = xTimerCreate("ai_session_tmr", pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS), pdFALSE, (void *)0, session_timer_cb);
    s_keep_alive_timer = xTimerCreate("ai_ws_keep_alive", pdMS_TO_TICKS(WS_KEEP_ALIVE_TIMEOUT_MS), pdFALSE, (void *)0, ws_keep_alive_timer_cb);
    ESP_LOGI(TAG, "ai_cloud initialized (Clean FSM + Zero-Drop Audio Architecture + 15s Keep-Alive Ready).");
}

bool ai_cloud_start_turn(void) {
    // 1. Dừng ngay âm thanh cũ đang phát ra loa (Hỗ trợ Barge-In / ngắt lời)
    audio_hal_stop_playback();

    if (s_session_timer) {
        xTimerStop(s_session_timer, 0);
    }

    // 2. Xóa sạch mọi dữ liệu ghi âm tồn đọng từ lượt trước trong RingBuffer TX
    if (s_tx_audio_ringbuf) {
        size_t dummy_size;
        uint8_t *dummy_item;
        while ((dummy_item = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_size, 0)) != NULL) {
            vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_item);
        }
    }

    // 3. Đảm bảo kết nối đã được kích hoạt
    ai_cloud_prewarm_session();

    // 4. Chuyển sang trạng thái ghi âm NGAY LẬP TỨC (Zero Latency - không chờ đợi!)
    s_current_state = AI_STATE_RECORDING;
    s_need_send_activity_start = true;
    s_turn_ending = false;
    s_cmd_dispatched_in_turn = false;
    s_mute_response_audio = false;

    if (s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_START_TURN, eSetBits);
    }

    return true;
}

void ai_cloud_send_audio_chunk(const uint8_t *data, size_t size) {
    if (s_current_state != AI_STATE_RECORDING || !data || size == 0 || s_turn_ending) {
        return;
    }

    if (!s_tx_audio_ringbuf) return;

    // Cắt dữ liệu lớn thành các chunk nhỏ 1920 bytes (~60ms) để không vượt quá khả năng đệm 1 frame
    const size_t max_chunk = 1920;
    size_t offset = 0;
    while (offset < size) {
        size_t to_send = size - offset;
        if (to_send > max_chunk) to_send = max_chunk;

        if (xRingbufferSend(s_tx_audio_ringbuf, (void *)(data + offset), to_send, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "TX Audio Ringbuffer full! Dropped %u bytes (offset %u/%u)",
                     (unsigned int)to_send, (unsigned int)offset, (unsigned int)size);
            break;
        }
        offset += to_send;
    }
}

void ai_cloud_end_turn(void) {
    ESP_LOGI(TAG, ">>> [CLIENT VAD] Dứt câu nói. Đang xả toàn bộ audio còn lại trong RingBuffer để gửi activityEnd... <<<");
    s_turn_ending = true;
    if (s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_END_TURN, eSetBits);
    }
}

void ai_cloud_stop_session(void) {
    bool was_incomplete = (s_current_state != AI_STATE_IDLE || s_turn_ending);

    if (was_incomplete) {
        // Chưa hoàn tất (bị ngắt giữa chừng do local match hoặc timeout) -> Yêu cầu Worker Task dọn dẹp & đóng WebSocket
        if (s_tx_task_handle) {
            xTaskNotify(s_tx_task_handle, CLOUD_EVT_CANCEL, eSetBits);
        }
    } else {
        // Lượt nói đã hoàn tất trọn vẹn: Khởi động timer Keep-Alive 15s an toàn
        if (s_is_ws_connected && s_setup_complete) {
            if (s_keep_alive_timer) {
                xTimerStop(s_keep_alive_timer, 0);
                xTimerStart(s_keep_alive_timer, 0);
                ESP_LOGI(TAG, ">>> [KEEP-ALIVE POOL] Giữ WebSocket kết nối sống trong 15s sẵn sàng cho lượt tiếp theo... <<<");
            }
        }
    }
}

void ai_cloud_cancel_turn(void) {
    ESP_LOGI(TAG, "Hủy lượt nói (khớp lệnh cục bộ hoặc no-speech) -> Thông báo worker task...");
    if (s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_CANCEL, eSetBits);
    }
}

void ai_cloud_on_playback_idle(void) {
    ESP_LOGD(TAG, "Audio playback idle notification received.");
}

bool ai_cloud_is_busy(void) {
    return (s_current_state != AI_STATE_IDLE);
}

bool ai_cloud_is_ready(void) {
    return (s_is_ws_connected && s_setup_complete);
}


