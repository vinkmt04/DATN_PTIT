#include "ai_cloud.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "audio_hal.h"
#include "mesh_controller.h"
#include "gateway_state_manager.h"
#include "esp_timer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "freertos/ringbuf.h"

extern void ai_edge_stop_recording(void);

static const char *TAG = "ai_cloud";

#if __has_include("app_secrets.h")
#include "app_secrets.h"
#endif

// Live API (WebSockets) Configuration
#ifndef GEMINI_API_KEY
#define GEMINI_API_KEY "YOUR_GEMINI_API_KEY"
#endif
#define GEMINI_WS_URL "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=" GEMINI_API_KEY

// Kích thước tĩnh cho bộ đệm PCM gom gói 180ms, Base64 và JSON (cấp phát 1 lần trong PSRAM)
#define TX_PCM_BATCH_BYTES 5760    // 180ms @ 16kHz 16-bit -> Base64 = 7680 bytes (< 8192)
#define TX_BASE64_BUF_SIZE 8192
#define TX_JSON_BUF_SIZE   10240
#define WS_RX_BUF_SIZE     262144  // 256KB: Nuốt trọn mọi frame từ Google (40KB - 80KB) không bao giờ tràn
#define RX_PCM_BUF_SIZE    196608  // 192KB: Chứa tối đa ~4 giây âm thanh PCM 24kHz 16-bit giải mã từ Base64
static uint8_t *s_tx_pcm_buf = NULL;
static size_t s_tx_pcm_len = 0;
static int64_t s_tx_pcm_first_ms = 0;
static unsigned char *s_tx_base64_buf = NULL;
static char *s_tx_json_buf = NULL;
static char *s_ws_rx_buf = NULL;
static size_t s_ws_rx_len = 0;
static unsigned char *s_rx_pcm_buf = NULL;

// Timeouts
#define RESPONSE_TIMEOUT_MS  8000   // Chờ phản hồi đầu tiên từ Gemini: 8s
#define CHUNK_TIMEOUT_MS     5000   // Chờ chunk tiếp theo giữa chừng: 5s (phản ứng nhanh khi cloud ngừng cấp audio)
#define WS_KEEP_ALIVE_TIMEOUT_MS 15000 // Giữ kết nối ngầm 15 giây (Keep-Alive Idle Pool)
#define WS_RECONNECT_COOLDOWN_MS 300   // Cooldown kết nối lại 300ms (đủ cho lwIP TCP socket teardown, không chặn câu lệnh liên tiếp)

typedef enum {
    AI_STATE_IDLE = 0,
    AI_STATE_BUFFERING,
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
#define CLOUD_EVT_START_TURN       (1 << 4) // Bắt đầu lượt thu âm mới (BUFFERING)
#define CLOUD_EVT_END_TURN         (1 << 5) // Dứt câu nói, xả hết audio để gửi activityEnd
#define CLOUD_EVT_WS_CLEANUP       (1 << 6) // Đóng sạch WebSocket bị lỗi/lệch khung mà không cắt ngang Jitter Buffer loa
#define CLOUD_EVT_WS_CONNECTED     (1 << 7) // WebSocket kết nối -> gửi Setup Frame từ cloud_tx_task
#define CLOUD_EVT_SEND_TOOL_RESP   (1 << 8) // Tool response sẵn sàng -> gửi từ cloud_tx_task
#define CLOUD_EVT_COMMIT_TURN      (1 << 9) // Chuyển từ BUFFERING sang RECORDING (bắt đầu stream cloud)

static volatile ai_gateway_state_t s_current_state = AI_STATE_IDLE;
static TimerHandle_t s_session_timer = NULL;
static TimerHandle_t s_keep_alive_timer = NULL;
static SemaphoreHandle_t ws_mutex = NULL;
static SemaphoreHandle_t s_tx_rb_mutex = NULL;
static SemaphoreHandle_t s_tool_resp_mutex = NULL;
static char s_pending_tool_resp[1024] = {0};

static esp_websocket_client_handle_t ws_client = NULL;
static volatile bool s_ws_task_active = false;
static volatile bool s_is_ws_connected = false;
static volatile bool s_is_ws_connecting = false;
static volatile bool s_is_ws_stopping = false;
static volatile bool s_setup_complete = false;
static uint32_t s_last_disconnect_time_ms = 0;

// Cờ theo dõi trạng thái đã truyền audio trong lượt (phục vụ keep-alive / clean session khi hủy)
static volatile bool s_audio_sent_in_turn = false;
static volatile bool s_turn_ending = false;
static volatile bool s_server_activity_started = false;
static volatile bool s_manual_end_turn = false;
static int64_t s_first_audio_sent_ms = 0;
static volatile int s_tool_call_count = 0;
static volatile bool s_mute_response_audio = false;
static volatile bool s_ws_rx_reset_req = false;
static int64_t s_tool_resp_sent_ms = 0;

// RingBuffer TX: Chứa âm thanh mic từ ai_edge truyền sang (256KB trong PSRAM ~ 8 giây âm thanh)
// Giúp luồng thu âm mic KHÔNG BAO GIỜ BỊ BLOCK bởi mạng!
static RingbufHandle_t s_tx_audio_ringbuf = NULL;
static TaskHandle_t s_tx_task_handle = NULL;

// RingBuffer RX JSON (RINGBUF_TYPE_NOSPLIT trong PSRAM) & Consumer Task trên Core 1:
// Tách biệt hoàn toàn luồng đọc WebSocket TLS (Core 0) khỏi luồng parse JSON / decode Base64 PCM (Core 1).
static RingbufHandle_t s_rx_json_ringbuf = NULL;
static TaskHandle_t s_rx_task_handle = NULL;
static volatile bool s_rx_flush_requested = false;

static volatile bool expecting_first_chunk = false;

void ai_cloud_abort(void) {
    ESP_LOGW(TAG, "ai_cloud_abort() requested -> Notifying worker task...");
    if (s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_ABORT, eSetBits);
    }
}

// Cầu chì phục hồi khi WebSocket đứt đột ngột / lệch khung:
// - Hạ ngay cờ bận (s_current_state = AI_STATE_IDLE) để ai_edge không bao giờ bị treo ở AI_RESPONDING
// - Nếu đang trong pha SPEAKING: cho phép audio_hal phát hết nốt phần âm thanh còn trong Jitter Buffer (không ngắt ngang)
static void recover_from_ws_failure(const char *reason, bool force_close_socket) {
    s_is_ws_connected = false;
    s_is_ws_connecting = false;
    s_setup_complete = false;
    s_last_disconnect_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_ws_rx_reset_req = true;
    s_rx_flush_requested = true;
    if (s_keep_alive_timer) {
        xTimerStop(s_keep_alive_timer, 0);
    }

    if (s_current_state == AI_STATE_SPEAKING) {
        ESP_LOGW(TAG, ">>> [WS RECOVERY] %s khi đang phát loa -> Hạ cờ bận ngay để phát hết Jitter Buffer & về IDLE! <<<", reason);
        bool was_muted = s_mute_response_audio;
        s_current_state = AI_STATE_IDLE;
        s_turn_ending = false;
        s_manual_end_turn = false;
        s_audio_sent_in_turn = false;
        s_server_activity_started = false;
        s_first_audio_sent_ms = 0;
        s_tx_pcm_len = 0;
        s_tx_pcm_first_ms = 0;
        s_tool_call_count = 0;
        s_mute_response_audio = false;
        if (s_session_timer) {
            xTimerStop(s_session_timer, 0);
        }
        if (!was_muted) {
            audio_hal_force_start_playing();
        }
        audio_hal_set_turn_active(false);
        if (force_close_socket && s_tx_task_handle) {
            xTaskNotify(s_tx_task_handle, CLOUD_EVT_WS_CLEANUP, eSetBits);
        }
    } else if (s_current_state == AI_STATE_BUFFERING) {
        ESP_LOGW(TAG, ">>> [WS RECOVERY] Kết nối Keep-Alive cũ vừa đóng (%s) khi đang BUFFERING -> Giữ nguyên lượt thu âm hiện tại! <<<", reason);
        if (force_close_socket && s_tx_task_handle) {
            xTaskNotify(s_tx_task_handle, CLOUD_EVT_WS_CLEANUP, eSetBits);
        }
    } else if (s_current_state != AI_STATE_IDLE) {
        ESP_LOGE(TAG, ">>> [WS RECOVERY] %s giữa lượt (state=%d) -> Abort lượt nói! <<<", reason, (int)s_current_state);
        if (s_current_state == AI_STATE_WAITING_POST_TOOL_SPEECH || s_current_state == AI_STATE_EXECUTING_TOOL) {
            s_mute_response_audio = true; // Giữ im lặng sau khi vừa thực thi tool xong
        }
        s_turn_ending = false;
        s_manual_end_turn = false;
        s_audio_sent_in_turn = false;
        s_server_activity_started = false;
        s_first_audio_sent_ms = 0;
        s_tx_pcm_len = 0;
        s_tx_pcm_first_ms = 0;
        s_tool_call_count = 0;
        if (s_tx_task_handle) {
            ai_cloud_abort(); // Để CLOUD_EVT_ABORT xử lý âm báo lỗi rồi mới hạ AI_STATE_IDLE
        } else {
            s_current_state = AI_STATE_IDLE;
        }
    } else if (force_close_socket && s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_WS_CLEANUP, eSetBits);
    }
}

static void session_timer_cb(TimerHandle_t xTimer) {
    if (s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_TIMEOUT, eSetBits);
    }
}

static const char *find_json_key_val(const char *obj_start, const char *obj_end, const char *quoted_key) {
    if (!obj_start || !obj_end || !quoted_key || obj_start >= obj_end) return NULL;
    size_t key_len = strlen(quoted_key);
    const char *cur = obj_start;
    while (cur < obj_end) {
        const char *found = strstr(cur, quoted_key);
        if (!found || found + key_len > obj_end) return NULL;
        const char *p = found + key_len;
        while (p < obj_end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
            p++;
        }
        if (p < obj_end && *p == ':') {
            p++;
            while (p < obj_end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
                p++;
            }
            return (p < obj_end) ? p : NULL;
        }
        cur = found + key_len;
    }
    return NULL;
}

static void parse_server_message(const char *json_string) {
    if (!json_string || json_string[0] == '\0') {
        return;
    }

    // Chốt chặn JSON (Frame Desync Guard): Mọi thông điệp hợp lệ từ Gemini Live API đều là JSON Object bắt đầu bằng '{'
    const char *trim = json_string;
    while (*trim == ' ' || *trim == '\t' || *trim == '\r' || *trim == '\n') {
        trim++;
    }
    if (*trim != '{') {
        ESP_LOGE(TAG, ">>> [FRAME DESYNC GUARD] Phát hiện thông điệp không phải JSON hợp lệ ('%.24s') -> Phục hồi & đóng phiên! <<<", trim);
        recover_from_ws_failure("Invalid non-JSON message", true);
        return;
    }

    // 1. Kiểm tra xác nhận hoàn tất thiết lập từ Gemini Live API
    if (strstr(json_string, "\"setupComplete\"")) {
        ESP_LOGI(TAG, ">>> [GEMINI LIVE] Setup Complete! Connection Ready for Realtime Audio. <<<");
        s_setup_complete = true;
        return;
    }

    // 2. Bắt lỗi từ máy chủ Google API (Chỉ kích hoạt nếu không phải trường error nằm trong serverContent hoặc toolCall)
    if (!strstr(json_string, "\"serverContent\"") && !strstr(json_string, "\"toolCall\"") && strstr(json_string, "\"error\"")) {
        ESP_LOGE(TAG, "Gemini API Server Error: %s", json_string);
        ai_cloud_abort();
        return;
    }

    // Chốt chặn 1 (State Guard): Nếu Gateway đang ở AI_STATE_IDLE (không có lượt nói nào diễn ra),
    // hủy bỏ ngay lập tức mọi gói tin trễ từ server để chống rò rỉ hoặc vòng lặp ngoài ý muốn.
    if (s_current_state == AI_STATE_IDLE) {
        return;
    }

    if (!s_server_activity_started &&
        ((strstr(json_string, "\"voiceActivity\"") && strstr(json_string, "ACTIVITY_START")) ||
         strstr(json_string, "\"inputTranscription\"") ||
         strstr(json_string, "\"toolCall\"") ||
         strstr(json_string, "\"inlineData\""))) {
        s_server_activity_started = true;
    }

    // 3. Xử lý nhận diện giọng nói Người Dùng (STT) nếu có
    const char *stt_tag = strstr(json_string, "\"inputTranscription\"");
    if (stt_tag) {
        if (s_current_state == AI_STATE_WAITING_AI && s_session_timer) {
            xTimerReset(s_session_timer, 0);
        }
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
        const char *text_key = strstr(text_source, "\"text\"");
        if (text_key) {
            const char *colon = strchr(text_key + 6, ':');
            if (colon) {
                const char *val_start = strchr(colon, '\"');
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
    }

    // 4.1 Xử lý Function Calling chính thức từ Gemini Live (Gemini Tools)
    const char *tool_call = strstr(json_string, "\"toolCall\"");
    if (tool_call) {
        // Chốt chặn 2: Cho phép chặn mềm 1 lần nếu Gemini lỡ thử gọi lại sau lỗi thiết bị; >= 2 lần mới ngắt cứng WS
        if (s_tool_call_count >= 2) {
            ESP_LOGW(TAG, ">>> [STATE GUARD] Phát hiện tin nhắn toolCall lặp lại lần %d! Ngắt kết nối WebSocket ngay lập tức để cắt đứt vòng lặp... <<<",
                     s_tool_call_count + 1);
            ai_cloud_abort();
            return;
        }

        bool is_retry_reject = (s_tool_call_count == 1);
        if (is_retry_reject) {
            ESP_LOGW(TAG, ">>> [SOFT TOOL GUARD] Gemini thử gọi lại toolCall (lần 2) -> Không chạy phần cứng, trả error_permanent + do_not_retry! <<<");
        } else {
            ESP_LOGI(TAG, ">>> [GEMINI TOOL CALL RECEIVED] <<<");
        }

        s_current_state = AI_STATE_EXECUTING_TOOL;
        s_tool_call_count++;
        bool has_tool_error = is_retry_reject;

        char func_responses_json[768] = {0};
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

            // Kiểm tra xem trong {...} này có chứa "name" : ... không
            const char *name_val = find_json_key_val(obj_search, obj_end, "\"name\"");
            if (!name_val) {
                obj_search = obj_end + 1;
                continue;
            }

            // 1. Trích xuất fn_name
            char fn_name[64] = {0};
            if (*name_val == '\"') {
                const char *n_start = name_val + 1;
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
            const char *id_val = find_json_key_val(obj_search, obj_end, "\"id\"");
            if (id_val && *id_val == '\"') {
                const char *id_start = id_val + 1;
                const char *id_end = strchr(id_start, '\"');
                if (id_end && id_end <= obj_end) {
                    size_t id_len = id_end - id_start;
                    if (id_len >= sizeof(call_id)) id_len = sizeof(call_id) - 1;
                    memcpy(call_id, id_start, id_len);
                    call_id[id_len] = '\0';
                }
            }

            char result_msg[128] = "Đã thực hiện";
            bool item_error = is_retry_reject;

            if (!is_retry_reject) {
                // 3. Trích xuất action
                char action[32] = {0};
                const char *act_val = find_json_key_val(obj_search, obj_end, "\"action\"");
                if (act_val && *act_val == '\"') {
                    const char *a_start = act_val + 1;
                    const char *a_end = strchr(a_start, '\"');
                    if (a_end && a_end <= obj_end) {
                        size_t a_len = a_end - a_start;
                        if (a_len >= sizeof(action)) a_len = sizeof(action) - 1;
                        memcpy(action, a_start, a_len);
                        action[a_len] = '\0';
                    }
                }

                // 4. Trích xuất brightness
                int brightness = -1;
                const char *b_val = find_json_key_val(obj_search, obj_end, "\"brightness\"");
                if (b_val) {
                    const char *bp = b_val;
                    if (*bp == '\"') bp++;
                    if (bp < obj_end && *bp >= '0' && *bp <= '9') {
                        sscanf(bp, "%d", &brightness);
                    }
                }

                ESP_LOGW(TAG, ">>> [TOOL PARSED #%d] Fn: '%s' | Action: '%s' | Bright: %d | ID: '%s' <<<",
                         call_count + 1, fn_name, action, brightness, call_id);

                gateway_device_state_t cur_st;
                gateway_state_get(&cur_st);

                if (strcmp(fn_name, "control_dimmer") == 0) {
                    if (!cur_st.dimmer_registered) {
                        ESP_LOGW(TAG, ">>> [TOOL GUARD]: control_dimmer - Đèn chưa đăng ký vào mạng Thread <<<");
                        snprintf(result_msg, sizeof(result_msg), "Lỗi: Đèn chưa được kết nối vào hệ thống");
                        item_error = true;
                    } else if (!cur_st.dimmer_node_online) {
                        ESP_LOGW(TAG, ">>> [TOOL GUARD]: control_dimmer - Đèn hiện đang ngoại tuyến <<<");
                        snprintf(result_msg, sizeof(result_msg), "Lỗi: Đèn hiện đang ngoại tuyến, vui lòng kiểm tra nguồn");
                        item_error = true;
                    } else {
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
                    }
                } else if (strcmp(fn_name, "control_relay") == 0) {
                    if (!cur_st.relay_registered) {
                        ESP_LOGW(TAG, ">>> [TOOL GUARD]: control_relay - Relay chưa đăng ký vào mạng Thread <<<");
                        snprintf(result_msg, sizeof(result_msg), "Lỗi: Công tắc chưa được kết nối vào hệ thống");
                        item_error = true;
                    } else if (!cur_st.relay_node_online) {
                        ESP_LOGW(TAG, ">>> [TOOL GUARD]: control_relay - Relay hiện đang ngoại tuyến <<<");
                        snprintf(result_msg, sizeof(result_msg), "Lỗi: Công tắc hiện đang ngoại tuyến, vui lòng kiểm tra nguồn");
                        item_error = true;
                    } else {
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
                }
            } else {
                ESP_LOGW(TAG, ">>> [RETRY REJECTED #%d] Fn: '%s' | ID: '%s' <<<", call_count + 1, fn_name, call_id);
            }

            if (item_error) {
                has_tool_error = true;
            }

            // Ghi nhận functionResponse cho ID tương ứng
            if (call_id[0] != '\0') {
                char single_resp[512];
                int s_len = 0;
                if (item_error) {
                    const char *err_text = is_retry_reject
                        ? "Thiết bị hiện đang ngoại tuyến hoặc không khả dụng. KHÔNG gọi lại hàm nữa, hãy thông báo ngay cho người dùng bằng giọng nói tiếng Việt"
                        : result_msg;
                    s_len = snprintf(single_resp, sizeof(single_resp),
                        "%s{\"id\":\"%s\",\"name\":\"%s\",\"response\":{\"status\":\"error_permanent\",\"do_not_retry\":true,\"result\":\"%s. TUYỆT ĐỐI KHÔNG gọi lại hàm, hãy đọc nguyên văn câu báo lỗi này bằng tiếng Việt ngay.\"}}",
                        (call_count > 0 ? "," : ""), call_id, fn_name, err_text
                    );
                } else {
                    s_len = snprintf(single_resp, sizeof(single_resp),
                        "%s{\"id\":\"%s\",\"name\":\"%s\",\"response\":{\"status\":\"ok\",\"result\":\"%s\"}}",
                        (call_count > 0 ? "," : ""), call_id, fn_name, result_msg
                    );
                }
                if (s_len > 0 && resp_offset + (size_t)s_len < sizeof(func_responses_json)) {
                    memcpy(func_responses_json + resp_offset, single_resp, (size_t)s_len);
                    resp_offset += (size_t)s_len;
                    func_responses_json[resp_offset] = '\0';
                }
            }

            call_count++;
            obj_search = obj_end + 1;
        }

        // Gửi toolResponse phản hồi cho Google Live API để hoàn tất giao thức
        // Kiến trúc Single-Writer TX: Copy vào s_pending_tool_resp và kích hoạt CLOUD_EVT_SEND_TOOL_RESP cho cloud_tx_task
        if (call_count > 0) {
            if (s_tool_resp_mutex && xSemaphoreTake(s_tool_resp_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                snprintf(s_pending_tool_resp, sizeof(s_pending_tool_resp),
                    "{\"toolResponse\":{\"functionResponses\":[%s]}}",
                    func_responses_json
                );
                s_tool_resp_sent_ms = esp_timer_get_time() / 1000;
                xSemaphoreGive(s_tool_resp_mutex);
                if (s_tx_task_handle) {
                    xTaskNotify(s_tx_task_handle, CLOUD_EVT_SEND_TOOL_RESP, eSetBits);
                }
            }
        }

        // [LOCAL-SYNC SILENT EXIT / ERROR ALERT]
        if (has_tool_error) {
            s_mute_response_audio = false; // Bật loa để Gemini nói to câu cảnh báo lỗi thiết bị cho người dùng nghe!
            expecting_first_chunk = true;  // Đảm bảo luôn mở jitter buffer mới cho audio báo lỗi
            ESP_LOGW(TAG, ">>> [TOOL ERROR] Thiết bị ngoại tuyến hoặc chưa kết nối -> Bật loa để AI nói lỗi! <<<");
        } else {
            // Điều khiển thiết bị thành công -> Giữ loa 100% im lặng, chờ turnComplete kết thúc lượt
            s_mute_response_audio = true;
            ESP_LOGI(TAG, ">>> [LOCAL-SYNC SILENT EXIT] Lệnh điều khiển thành công -> Mute loa, chờ turnComplete... <<<");
            audio_hal_stop_playback();
            audio_hal_set_turn_active(false);
        }
        s_current_state = AI_STATE_WAITING_POST_TOOL_SPEECH;

        // Khởi tạo lại Safeguard Timer (6s khi có lỗi tool để Gemini kịp sinh audio báo lỗi, 4s khi bình thường)
        if (s_session_timer) {
            uint32_t tool_wait_ms = has_tool_error ? 6000 : 4000;
            xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(tool_wait_ms), 0);
            xTimerStart(s_session_timer, 0);
        }
    }

    // 4.2 Server Neural VAD (ACTIVITY_END): Dừng thu âm mic ngay lập tức & chờ phản hồi (hoặc gia hạn timer nếu đang WAITING_AI)
    if (strstr(json_string, "\"voiceActivity\"") && strstr(json_string, "ACTIVITY_END")) {
        if (s_current_state == AI_STATE_RECORDING) {
            s_current_state = AI_STATE_WAITING_AI;
            s_turn_ending = false;
            expecting_first_chunk = true;
            if (s_session_timer) {
                xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS), 0);
                xTimerStart(s_session_timer, 0);
            }
            ESP_LOGI(TAG, ">>> [SERVER VAD] Gemini detected ACTIVITY_END -> Stopping mic stream & waiting for response! <<<");
        } else if (s_current_state == AI_STATE_WAITING_AI && s_session_timer) {
            xTimerReset(s_session_timer, 0);
        }
    }

    // 5. Fast-Path: Trích xuất Audio PCM (Base64) từ inlineData - ZERO MALLOC & ZERO HEAP CONTENTION
    const char *search_ptr = json_string;
    const char *inline_tag = NULL;
    while ((inline_tag = strstr(search_ptr, "\"inlineData\"")) != NULL) {
        const char *data_key = strstr(inline_tag, "\"data\"");
        if (!data_key) break;
        const char *colon = strchr(data_key + 6, ':');
        if (!colon) break;
        const char *val_start = strchr(colon, '\"');
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
                xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(CHUNK_TIMEOUT_MS), 0);
                xTimerReset(s_session_timer, 0);
            }
        } else if (s_current_state == AI_STATE_SPEAKING) {
            // Đang phát luồng âm thanh bình thường: Reset chunk watchdog timer 5s
            if (s_session_timer) {
                xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(CHUNK_TIMEOUT_MS), 0);
                xTimerReset(s_session_timer, 0);
            }
        } else if (s_current_state == AI_STATE_RECORDING) {
            ESP_LOGI(TAG, ">>> [SERVER VAD] Gemini phát hiện dứt câu sớm & bắt đầu trả lời! Dừng thu âm.");
            if (!s_mute_response_audio) {
                s_current_state = AI_STATE_SPEAKING;
            }
            expecting_first_chunk = true;
            if (s_session_timer) {
                xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(CHUNK_TIMEOUT_MS), 0);
                xTimerReset(s_session_timer, 0);
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
    bool has_gen_complete = false;
    const char *gc_tag = strstr(json_string, "\"generationComplete\"");
    if (gc_tag) {
        const char *p = gc_tag + 20;
        while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (strncmp(p, "true", 4) == 0) {
            has_gen_complete = true;
        }
    }

    // Nếu nhận generationComplete: true lúc đang phát loa (chưa có turnComplete):
    // Không ngắt ngay (tránh rơi gói audio cuối), chỉ hạ session timer xuống 1500ms làm cầu chì kết thúc êm nếu mất turnComplete
    if (has_gen_complete && !is_turn_finished && s_current_state == AI_STATE_SPEAKING && s_session_timer) {
        xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(1500), 0);
        xTimerReset(s_session_timer, 0);
    }

    // Nếu đang trong pha gọi hàm (EXECUTING_TOOL) hoặc đang chờ phản hồi thoại sau hàm (WAITING_POST_TOOL_SPEECH):
    // Bỏ qua generationComplete đơn lẻ, CHỈ chấp nhận turnComplete: true chính thức kết thúc toàn bộ lượt!
    if (s_current_state == AI_STATE_EXECUTING_TOOL) {
        is_turn_finished = false;
    } else if (s_current_state == AI_STATE_WAITING_POST_TOOL_SPEECH) {
        if (!s_mute_response_audio && (esp_timer_get_time() / 1000 - s_tool_resp_sent_ms) < 350) {
            // Chặn gói turnComplete/generationComplete trễ của lượt toolCall cũ (đến sau < 350ms), tiếp tục chờ Gemini gửi audio giải thích lỗi
            is_turn_finished = false;
        } else if (!has_turn_complete) {
            is_turn_finished = false;
        }
    }

    if (is_turn_finished && s_current_state != AI_STATE_IDLE) {
        ESP_LOGI(TAG, "Model turn complete. Waiting for next user input...");
        bool was_muted = s_mute_response_audio;
        s_current_state = AI_STATE_IDLE;
        s_turn_ending = false;
        s_audio_sent_in_turn = false;
        s_server_activity_started = false;
        s_manual_end_turn = false;
        s_first_audio_sent_ms = 0;
        s_tx_pcm_len = 0;
        s_tx_pcm_first_ms = 0;
        s_tool_call_count = 0;
        s_mute_response_audio = false;
        if (s_session_timer) {
            xTimerStop(s_session_timer, 0);
        }
        if (!was_muted) {
            audio_hal_force_start_playing();
        }
        audio_hal_set_turn_active(false);
    }

    // 7. Log toan bo sieu du lieu (Metadata / Event) co y nghia tu Google (bo qua heartbeat rong {} ke ca co khoang trang/xuong dong)
    if (!inline_tag && !text_source && !stt_tag && !strstr(json_string, "\"setupComplete\"")) {
        bool has_meaningful_content = false;
        for (const char *p = json_string; *p != '\0'; p++) {
            if (*p != '{' && *p != '}' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                has_meaningful_content = true;
                break;
            }
        }
        if (has_meaningful_content &&
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

        char *prompt_buf = (char *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!prompt_buf) prompt_buf = (char *)malloc(4096);
        char *setup_buf = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!setup_buf) setup_buf = (char *)malloc(8192);

        if (!prompt_buf || !setup_buf) {
            ESP_LOGE(TAG, "Failed to allocate memory for Setup Frame!");
            if (prompt_buf) free(prompt_buf);
            if (setup_buf) free(setup_buf);
            return;
        }

        int p_len = snprintf(prompt_buf, 4096,
            "Bạn là trợ lý Nhà Thông Minh trên Gateway ESP32 điều khiển mạng Thread Mesh. "
            "TRẠNG THÁI THIẾT BỊ TRONG NHÀ: "
        );

        int dev_idx = 1;
        if (st.dimmer_registered) {
            p_len += snprintf(prompt_buf + p_len, 4096 - p_len,
                "%d. Đèn Dimming: %s, độ sáng %u%% (mức nhớ: %u%%). ",
                dev_idx++, st.dimmer_on ? "BẬT" : "TẮT", st.dimmer_brightness,
                st.dimmer_last_brightness > 0 ? st.dimmer_last_brightness : 80
            );
        }

        if (st.relay_registered) {
            p_len += snprintf(prompt_buf + p_len, 4096 - p_len,
                "%d. Công tắc Relay 1 kênh: %s. ",
                dev_idx++, st.relay_on ? "BẬT" : "TẮT"
            );
        }

        if (st.sensor_registered) {
            p_len += snprintf(prompt_buf + p_len, 4096 - p_len,
                "%d. Cảm biến DHT11: Nhiệt độ %.1f độ C, Độ ẩm %.1f%%. ",
                dev_idx++, st.sensor_valid ? st.temperature : 28.0f, st.sensor_valid ? st.humidity : 65.0f
            );
        }

        if (dev_idx == 1) {
            p_len += snprintf(prompt_buf + p_len, 4096 - p_len,
                "Hiện chưa có thiết bị nào kết nối trong mạng. "
            );
        }

        p_len += snprintf(prompt_buf + p_len, 4096 - p_len,
            "THIẾT BỊ KHÔNG CÓ: %s%s%sQuạt, tivi, điều hòa, bình nóng lạnh, máy bơm. ",
            !st.dimmer_registered ? "Đèn chiếu sáng; " : "",
            !st.relay_registered ? "Công tắc Relay; " : "",
            !st.sensor_registered ? "Cảm biến nhiệt ẩm DHT11; " : ""
        );

        p_len += snprintf(prompt_buf + p_len, 4096 - p_len,
            "QUY TẮC BẮT BUỘC: "
            "1. Luôn dùng TIẾNG VIỆT tự nhiên, thân thiện. "
            "2. NHẬN DIỆN LỆNH NHÀ THÔNG MINH TIẾNG VIỆT (ƯU TIÊN CAO): Người dùng giao tiếp bằng TIẾNG VIỆT. Nếu nghe các cụm từ hoặc âm gần giống lệnh điều khiển (ví dụ: 'bật đèn', 'bật đền', 'but then' -> bật đèn; 'tắt đèn', 'tắt đền' -> tắt đèn; 'bật công tắc', 'bật công tác', 'mạch công tắc' -> bật công tắc; 'tắt công tắc', 'tắt công tác', 'tonta' -> tắt công tắc), hãy gọi ngay hàm điều khiển tương ứng. "
            "3. CHỐNG SUY DIỄN & BỊA CHUYỆN: Nếu âm thanh chỉ là một mảnh từ ngắn 1-3 chữ rời rạc không khớp lệnh điều khiển và không phải câu hỏi hoàn chỉnh (hoặc chỉ là tiếng ồn, tiếng thở), TUYỆT ĐỐI KHÔNG tự bịa ra câu chuyện hay hội thoại giả định; BẮT BUỘC chỉ trả lời duy nhất: 'Tôi chưa nghe rõ, bạn vui lòng nói lại nhé.'. Chỉ nói 'Nhà hiện tại không có thiết bị này.' khi người dùng gọi đích danh thiết bị không có trong nhà (như 'quạt', 'tivi', 'điều hòa', 'máy bơm'%s%s). "
            "%s%s"
            "6. Tuyệt đối không tự suy diễn câu lệnh của thiết bị này sang thiết bị khác, không gọi hàm lần thứ 2 trong cùng lượt (kể cả khi gặp lỗi thiết bị). "
            "7. Sau khi gọi hàm điều khiển: Phản hồi cực ngắn (dưới 4 từ, ví dụ: 'Đã bật đèn.', 'Đã bật công tắc.') và kết thúc lượt ngay. Nếu toolResponse trả về error_permanent hoặc do_not_retry: true (thiết bị ngoại tuyến hoặc chưa kết nối), TUYỆT ĐỐI KHÔNG gọi lại bất kỳ hàm nào nữa; hãy lập tức đọc nguyên văn câu báo lỗi bằng tiếng Việt qua loa cho người dùng nghe. "
            "%s"
            "9. Tuyệt đối không xuất thẻ XML, HTML hay SSML như <audio>.",
            !st.relay_registered ? ", 'công tắc'" : "",
            !st.dimmer_registered ? ", 'đèn'" : "",
            st.dimmer_registered ? "4. Hàm control_dimmer CHỈ gọi khi người dùng nói 'đèn' hoặc 'ánh sáng'. Tuyệt đối không gọi cho 'công tắc', 'quạt'. " : "",
            st.relay_registered ? "5. Hàm control_relay CHỈ gọi khi người dùng nói 'công tắc'. Tuyệt đối không gọi cho 'đèn', 'quạt'. " : "",
            st.sensor_registered ? "8. Khi hỏi nhiệt độ, độ ẩm phòng (DHT11) hoặc trò chuyện, hỏi đáp kiến thức: Trả lời tự nhiên qua loa, không gọi hàm. "
                                 : "8. Khi người dùng trò chuyện, hỏi đáp kiến thức: Trả lời tự nhiên qua loa, không gọi hàm. "
        );

        if (p_len >= 4096) {
            ESP_LOGE(TAG, "Prompt buffer overflow! p_len=%d >= 4096", p_len);
        }

        // Sinh động trường tools_json: chỉ khai báo tool cho thiết bị đã đăng ký trong hệ thống (cấp phát PSRAM)
        char *tools_json = (char *)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tools_json) tools_json = (char *)malloc(2048);
        if (tools_json) {
            tools_json[0] = '\0';
            if (st.dimmer_registered && st.relay_registered) {
                snprintf(tools_json, 2048, "%s",
                    "\"tools\":[{\"functionDeclarations\":["
                        "{\"name\":\"control_dimmer\",\"description\":\"CHỈ dùng để điều khiển bóng đèn (bật đèn, tắt đèn, chỉnh độ sáng đèn). TUYỆT ĐỐI KHÔNG gọi hàm này khi người dùng nói công tắc, quạt hoặc thiết bị khác.\","
                        "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
                            "\"action\":{\"type\":\"STRING\",\"description\":\"Hành động: 'turn_on', 'turn_off', hoặc 'set_brightness'.\"},"
                            "\"brightness\":{\"type\":\"INTEGER\",\"description\":\"Độ sáng từ 0 đến 100% khi action là set_brightness hoặc khi bật ở mức cụ thể.\"}"
                        "},\"required\":[\"action\"]}},"
                        "{\"name\":\"control_relay\",\"description\":\"CHỈ dùng để điều khiển công tắc Relay (bật hoặc tắt công tắc điện). TUYỆT ĐỐI KHÔNG gọi hàm này cho đèn, quạt hoặc thiết bị khác.\","
                        "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
                            "\"action\":{\"type\":\"STRING\",\"description\":\"Hành động: 'turn_on' hoặc 'turn_off'.\"}"
                        "},\"required\":[\"action\"]}}"
                    "]}],");
            } else if (st.dimmer_registered) {
                snprintf(tools_json, 2048, "%s",
                    "\"tools\":[{\"functionDeclarations\":["
                        "{\"name\":\"control_dimmer\",\"description\":\"CHỈ dùng để điều khiển bóng đèn (bật đèn, tắt đèn, chỉnh độ sáng đèn). TUYỆT ĐỐI KHÔNG gọi hàm này khi người dùng nói công tắc, quạt hoặc thiết bị khác.\","
                        "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
                            "\"action\":{\"type\":\"STRING\",\"description\":\"Hành động: 'turn_on', 'turn_off', hoặc 'set_brightness'.\"},"
                            "\"brightness\":{\"type\":\"INTEGER\",\"description\":\"Độ sáng từ 0 đến 100% khi action là set_brightness hoặc khi bật ở mức cụ thể.\"}"
                        "},\"required\":[\"action\"]}}"
                    "]}],");
            } else if (st.relay_registered) {
                snprintf(tools_json, 2048, "%s",
                    "\"tools\":[{\"functionDeclarations\":["
                        "{\"name\":\"control_relay\",\"description\":\"CHỈ dùng để điều khiển công tắc Relay (bật hoặc tắt công tắc điện). TUYỆT ĐỐI KHÔNG gọi hàm này cho đèn, quạt hoặc thiết bị khác.\","
                        "\"parameters\":{\"type\":\"OBJECT\",\"properties\":{"
                            "\"action\":{\"type\":\"STRING\",\"description\":\"Hành động: 'turn_on' hoặc 'turn_off'.\"}"
                        "},\"required\":[\"action\"]}}"
                    "]}],");
            }
        }

        int setup_len = snprintf(setup_buf, 8192,
            "{\"setup\":{\"model\":\"models/gemini-3.8-live\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],\"speechConfig\":{\"languageCode\":\"vi-VN\"}},"
            "%s"
            "\"systemInstruction\":{\"parts\":[{\"text\":\"%s\"}]},"
            "\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":false,\"endOfSpeechSensitivity\":\"END_SENSITIVITY_LOW\",\"silenceDurationMs\":1200}},\"inputAudioTranscription\":{}}}",
            tools_json ? tools_json : "",
            prompt_buf
        );

        if (setup_len >= 8192) {
            ESP_LOGE(TAG, "Setup buffer overflow! setup_len=%d >= 8192", setup_len);
        }

        if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            esp_websocket_client_send_text(ws_client, setup_buf, strlen(setup_buf), pdMS_TO_TICKS(1500));
            xSemaphoreGive(ws_mutex);
        }
        ESP_LOGI(TAG, "Sent Dynamic Setup message (len: %d bytes, DIMMER=%d, RELAY=%d, DHT11=%d). Waiting for setupComplete...",
                 setup_len, st.dimmer_registered, st.relay_registered, st.sensor_registered);

        if (tools_json) free(tools_json);
        free(prompt_buf);
        free(setup_buf);
    }
}

static void stop_ws_client_safe(void) {
    esp_websocket_client_handle_t client_to_stop = NULL;
    if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (ws_client && (s_ws_task_active || s_is_ws_connected || s_is_ws_connecting)) {
            s_is_ws_stopping = true;
            client_to_stop = ws_client;
        }
        s_is_ws_connected = false;
        s_is_ws_connecting = false;
        s_setup_complete = false;
        s_last_disconnect_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        xSemaphoreGive(ws_mutex);
    }

    if (client_to_stop) {
        // Giải phóng ws_mutex trước khi chờ esp_websocket_client_stop() để không block luồng khác
        esp_websocket_client_stop(client_to_stop);
        s_ws_rx_len = 0;
        if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_is_ws_stopping = false;
            s_last_disconnect_time_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            xSemaphoreGive(ws_mutex);
        } else {
            s_is_ws_stopping = false;
        }
    }
}

static void enqueue_rx_json_message(void) {
    if (!s_ws_rx_buf || s_ws_rx_len == 0) {
        s_ws_rx_len = 0;
        return;
    }

    s_ws_rx_buf[s_ws_rx_len] = '\0';
    size_t msg_size = s_ws_rx_len + 1;
    s_ws_rx_len = 0;

    if (!s_rx_json_ringbuf) {
        return;
    }

    // Đẩy ngay chuỗi JSON hoàn chỉnh sang Consumer (Core 1), không giữ chân websocket_task (Core 0)
    // Timeout pdMS_TO_TICKS(1500). Nếu đầy, chỉ drop và log cảnh báo chứ TUYỆT ĐỐI KHÔNG tự sát gọi recover_from_ws_failure
    if (xRingbufferSend(s_rx_json_ringbuf, (void *)s_ws_rx_buf, msg_size, pdMS_TO_TICKS(1500)) != pdTRUE) {
        ESP_LOGW(TAG, "RX JSON RingBuffer full/timeout (%u bytes) -> Dropped message (no suicide)",
                 (unsigned int)msg_size);
    }
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_BEGIN:
        s_ws_task_active = true;
        break;

    case WEBSOCKET_EVENT_BEFORE_CONNECT:
        s_is_ws_connecting = true;
        break;

    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, ">>> WEBSOCKET_EVENT_CONNECTED <<<");
        s_is_ws_connected = true;
        s_is_ws_connecting = false;
        // Kiến trúc Single-Writer TX: Gửi CLOUD_EVT_WS_CONNECTED tới cloud_tx_task để cloud_tx_task gửi setup frame
        if (s_tx_task_handle) {
            xTaskNotify(s_tx_task_handle, CLOUD_EVT_WS_CONNECTED, eSetBits);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        recover_from_ws_failure("WEBSOCKET_EVENT_DISCONNECTED", false);
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WEBSOCKET_EVENT_CLOSED");
        recover_from_ws_failure("WEBSOCKET_EVENT_CLOSED", false);
        break;

    case WEBSOCKET_EVENT_FINISH:
        ESP_LOGD(TAG, "WEBSOCKET_EVENT_FINISH");
        s_ws_task_active = false;
        if (!s_is_ws_stopping && s_current_state != AI_STATE_IDLE) {
            recover_from_ws_failure("WEBSOCKET_EVENT_FINISH", false);
        } else {
            s_is_ws_connected = false;
            s_is_ws_connecting = false;
            s_setup_complete = false;
            s_ws_rx_len = 0;
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        // Driver esp_websocket_client sẽ tự gọi abort_connection -> WEBSOCKET_EVENT_DISCONNECTED & thoát task
        recover_from_ws_failure("WEBSOCKET_EVENT_ERROR", false);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (s_ws_rx_reset_req) {
            s_ws_rx_reset_req = false;
            s_ws_rx_len = 0;
        }
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

            // Chốt chặn RFC 6455: Dù là Close Frame chuẩn (1000-4999) hay Close Frame giả mạo do lệch khung
            // (Invalid Code ngoài dải 1000-4999), driver esp_websocket_client đã chuyển sang WEBSOCKET_STATE_CLOSING.
            // Bắt buộc phục hồi trạng thái & hạ cờ bận ngay để không treo ở AI_EDGE_STATE_AI_RESPONDING!
            if (close_code < 1000 || close_code > 4999) {
                ESP_LOGW(TAG, "Phát hiện WebSocket CLOSE Frame giả mạo do lệch khung (Invalid Code: %u, Raw: '%s') -> Phục hồi & hạ cờ bận!",
                         close_code, reason_buf);
                recover_from_ws_failure("Frame Desync Invalid Close Code", true);
            } else {
                ESP_LOGE(TAG, ">>> WS CLOSE FRAME from Google: Code %u | Reason: '%s' <<<", close_code, reason_buf);
                recover_from_ws_failure("WS CLOSE FRAME from server", true);
            }
            break;
        }

        if (data->op_code == 0x09) {
            // Ping Frame
            break;
        }

        if ((data->op_code >= 0x03 && data->op_code <= 0x07) || data->op_code >= 0x0B) {
            ESP_LOGW(TAG, "Phát hiện WebSocket opcode không hợp lệ (0x%02X) -> Lệch khung! Phục hồi & đóng phiên.",
                     data->op_code);
            recover_from_ws_failure("Invalid WS Opcode (Frame Desync)", true);
            break;
        }

        if (data->data_ptr && data->data_len > 0) {
            // Reset timer ngay khi nhận bất kỳ mảnh dữ liệu Wi-Fi nào từ Google (xác nhận mạng đang truyền tải)
            if (s_current_state == AI_STATE_SPEAKING && s_session_timer) {
                xTimerReset(s_session_timer, 0);
            }
            if (data->payload_offset == 0) {
                // Chỉ reset & kiểm tra '{' khi bắt đầu message mới (không phải Continuation frame 0x00)
                if (data->op_code != 0x00) {
                    s_ws_rx_len = 0;
                    if (data->op_code == 0x01 || data->op_code == 0x02) {
                        bool valid_json_start = false;
                        for (int i = 0; i < data->data_len && i < 32; i++) {
                            char c = data->data_ptr[i];
                            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
                            if (c == '{') valid_json_start = true;
                            break;
                        }
                        if (!valid_json_start) {
                            ESP_LOGE(TAG, ">>> [FRAME DESYNC GUARD] Đầu frame nhận được dữ liệu Base64/rác không phải JSON -> Hạ cờ bận & đóng phiên! <<<");
                            recover_from_ws_failure("Frame Desync Non-JSON Start", true);
                            break;
                        }
                    }
                }
            } else if (data->op_code != 0x00 && data->payload_offset != s_ws_rx_len) {
                ESP_LOGW(TAG, "WS payload offset mismatch! (Exp: %u, Got: %u) -> Phục hồi & đóng phiên!",
                         (unsigned int)s_ws_rx_len, (unsigned int)data->payload_offset);
                s_ws_rx_len = 0;
                recover_from_ws_failure("WS Payload Offset Mismatch", true);
                break;
            }
            
            if (s_ws_rx_buf && (s_ws_rx_len + data->data_len < WS_RX_BUF_SIZE)) {
                memcpy(s_ws_rx_buf + s_ws_rx_len, data->data_ptr, data->data_len);
                s_ws_rx_len += data->data_len;
            } else {
                ESP_LOGE(TAG, "s_ws_rx_buf overflow! (len: %u + inc: %u > max: %u) -> Phục hồi & đóng phiên!",
                         (unsigned int)s_ws_rx_len, (unsigned int)data->data_len, (unsigned int)WS_RX_BUF_SIZE);
                s_ws_rx_len = 0;
                recover_from_ws_failure("WS RX Buffer Overflow", true);
                break;
            }
            
            if (data->fin && (data->payload_offset + data->data_len >= data->payload_len)) {
                enqueue_rx_json_message();
            }
        } else if (data->fin && data->payload_len == 0 && s_ws_rx_len > 0 && data->op_code == 0x00) {
            enqueue_rx_json_message();
        }
        break;
    }
}

// Xả toàn bộ item tồn đọng trong s_rx_json_ringbuf (chỉ gọi từ cloud_rx_process_task để giữ Single-Consumer Thread Safety)
static void drain_rx_json_ringbuf(void) {
    if (!s_rx_json_ringbuf) {
        return;
    }
    size_t dummy_size = 0;
    void *dummy_item = NULL;
    while ((dummy_item = xRingbufferReceive(s_rx_json_ringbuf, &dummy_size, 0)) != NULL) {
        vRingbufferReturnItem(s_rx_json_ringbuf, dummy_item);
    }
}

// Luồng Consumer độc lập (Core 1): Lấy từng chuỗi JSON hoàn chỉnh từ s_rx_json_ringbuf để parse & giải mã PCM Base64
static void cloud_rx_process_task(void *arg) {
    ESP_LOGI(TAG, "Cloud RX Process Task Started on Core %d (Decoupled JSON Parse & PCM Base64 Decode).",
             xPortGetCoreID());

    while (1) {
        if (!s_rx_json_ringbuf) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (s_rx_flush_requested) {
            s_rx_flush_requested = false;
            drain_rx_json_ringbuf();
        }

        size_t item_size = 0;
        char *item = (char *)xRingbufferReceive(s_rx_json_ringbuf, &item_size, pdMS_TO_TICKS(20));
        if (item != NULL) {
            if (s_rx_flush_requested) {
                vRingbufferReturnItem(s_rx_json_ringbuf, (void *)item);
                s_rx_flush_requested = false;
                drain_rx_json_ringbuf();
                continue;
            }

            if (item_size > 0 && item[item_size - 1] == '\0') {
                parse_server_message((const char *)item);
            }
            vRingbufferReturnItem(s_rx_json_ringbuf, (void *)item);
        }
    }
}

// Hủy êm khi Server VAD chưa thấy giọng người: không gửi audioStreamEnd, không phát loa, dọn sạch WS
static void silent_cancel_no_speech_turn(void) {
    s_current_state = AI_STATE_IDLE;
    s_turn_ending = false;
    s_manual_end_turn = false;
    s_audio_sent_in_turn = false;
    s_server_activity_started = false;
    s_first_audio_sent_ms = 0;
    s_tx_pcm_len = 0;
    s_tx_pcm_first_ms = 0;
    s_tool_call_count = 0;
    s_mute_response_audio = true;
    if (s_session_timer) {
        xTimerStop(s_session_timer, 0);
    }
    s_rx_flush_requested = true;
    if (s_tx_rb_mutex && xSemaphoreTake(s_tx_rb_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_tx_audio_ringbuf) {
            size_t dummy_sz;
            uint8_t *dummy_it;
            while ((dummy_it = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_sz, 0)) != NULL) {
                vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_it);
            }
        }
        xSemaphoreGive(s_tx_rb_mutex);
    }
    ai_edge_stop_recording();
    stop_ws_client_safe();
    s_mute_response_audio = false;
}

// Luồng truyền âm thanh độc lập & FSM Worker (Consumer):
// Tiếp nhận các cờ sự kiện điều khiển (Task Notification Bitmask) và stream âm thanh từ RingBuffer TX.
static void cloud_tx_task(void *arg) {
    ESP_LOGI(TAG, "Cloud TX Task Started (Dedicated Asynchronous Streaming & FSM Worker on Core %d).", xPortGetCoreID());

    while (1) {
        // 1. Kiểm tra và nhận tín hiệu điều khiển bất đồng bộ (Task Notification)
        // Chỉ dùng timeout 0 khi đang thực sự RECORDING; mọi trạng thái khác ngủ 50ms để tránh busy-spin 100% CPU Core 1
        uint32_t notify_bits = 0;
        BaseType_t got_notify = xTaskNotifyWait(0, 0xFFFFFFFF, &notify_bits,
            (s_current_state == AI_STATE_RECORDING) ? 0 : pdMS_TO_TICKS(50));

        if (got_notify == pdTRUE) {
            // Xử lý WS_CLEANUP (Đóng sạch socket bị lỗi/lệch khung mà không làm gián đoạn audio đang phát ở loa)
            if (notify_bits & CLOUD_EVT_WS_CLEANUP) {
                ESP_LOGW(TAG, "Worker handling CLOUD_EVT_WS_CLEANUP (Stopping broken WS cleanly without touching playback)...");
                stop_ws_client_safe();
                if (!(notify_bits & (CLOUD_EVT_ABORT | CLOUD_EVT_TIMEOUT | CLOUD_EVT_CANCEL | CLOUD_EVT_START_TURN | CLOUD_EVT_COMMIT_TURN))) {
                    continue;
                }
            }

            // Xử lý WS_CONNECTED: Single-Writer gửi Setup Frame
            if (notify_bits & CLOUD_EVT_WS_CONNECTED) {
                ESP_LOGI(TAG, "Worker handling CLOUD_EVT_WS_CONNECTED (Sending Setup Frame from cloud_tx_task)...");
                send_setup_frame();
            }

            // Xử lý SEND_TOOL_RESP: Single-Writer gửi toolResponse
            if (notify_bits & CLOUD_EVT_SEND_TOOL_RESP) {
                char resp_copy[1024] = {0};
                if (s_tool_resp_mutex && xSemaphoreTake(s_tool_resp_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    strncpy(resp_copy, s_pending_tool_resp, sizeof(resp_copy) - 1);
                    xSemaphoreGive(s_tool_resp_mutex);
                }
                if (resp_copy[0] != '\0' && ws_client && s_is_ws_connected) {
                    if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                        esp_websocket_client_send_text(ws_client, resp_copy, strlen(resp_copy), pdMS_TO_TICKS(1500));
                        ESP_LOGI(TAG, ">>> [SINGLE-WRITER TX] Sent toolResponse to Gemini Live <<<");
                        xSemaphoreGive(ws_mutex);
                    }
                }
            }

            // Xử lý ABORT (Lỗi mạng hoặc ngắt khẩn cấp)
            if (notify_bits & CLOUD_EVT_ABORT) {
                ESP_LOGW(TAG, "Worker handling CLOUD_EVT_ABORT (Stopping WebSocket & resetting FSM)...");
                bool was_silent_tool_success = s_mute_response_audio;
                if (s_session_timer) xTimerStop(s_session_timer, 0);
                if (s_keep_alive_timer) xTimerStop(s_keep_alive_timer, 0);
                s_turn_ending = false;
                s_manual_end_turn = false;
                s_audio_sent_in_turn = false;
                s_server_activity_started = false;
                s_first_audio_sent_ms = 0;
                s_tx_pcm_len = 0;
                s_tx_pcm_first_ms = 0;
                s_tool_call_count = 0;
                s_mute_response_audio = false;
                s_rx_flush_requested = true;
                if (s_tx_rb_mutex && xSemaphoreTake(s_tx_rb_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (s_tx_audio_ringbuf) {
                        size_t dummy_sz;
                        uint8_t *dummy_it;
                        while ((dummy_it = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_sz, 0)) != NULL) {
                            vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_it);
                        }
                    }
                    xSemaphoreGive(s_tx_rb_mutex);
                }
                audio_hal_stop_playback();
                audio_hal_set_turn_active(false);
                stop_ws_client_safe();
                if (!was_silent_tool_success) {
                    audio_hal_play_error_sound();
                }
                s_current_state = AI_STATE_IDLE;
                ai_edge_stop_recording();
                continue;
            }

            // Xử lý TIMEOUT từ Software Timer
            if (notify_bits & CLOUD_EVT_TIMEOUT) {
                if (s_current_state == AI_STATE_SPEAKING) {
                    ESP_LOGI(TAG, "Chunk timeout during speaking -> Ending turn gracefully (no error sound)...");
                    s_current_state = AI_STATE_IDLE;
                    s_turn_ending = false;
                    s_manual_end_turn = false;
                    s_audio_sent_in_turn = false;
                    s_server_activity_started = false;
                    s_first_audio_sent_ms = 0;
                    s_tx_pcm_len = 0;
                    s_tx_pcm_first_ms = 0;
                    s_tool_call_count = 0;
                    s_mute_response_audio = false;
                    // KHÔNG gọi ai_edge_stop_recording() để loa phát hết Jitter Buffer rồi ai_edge tự về IDLE_WAKEWORD
                    if (s_session_timer) xTimerStop(s_session_timer, 0);
                    audio_hal_force_start_playing();
                    audio_hal_set_turn_active(false);
                    if (s_is_ws_connected && s_setup_complete && s_keep_alive_timer) {
                        xTimerStop(s_keep_alive_timer, 0);
                        xTimerStart(s_keep_alive_timer, 0);
                    }
                    continue;
                }
                if (s_current_state == AI_STATE_WAITING_AI || s_current_state == AI_STATE_WAITING_POST_TOOL_SPEECH) {
                    bool was_silent_tool_success = (s_current_state == AI_STATE_WAITING_POST_TOOL_SPEECH && s_mute_response_audio);
                    ESP_LOGW(TAG, "Response Timeout (waiting AI or post-tool ack). Resetting turn & closing socket...");
                    if (s_session_timer) xTimerStop(s_session_timer, 0);
                    if (s_keep_alive_timer) xTimerStop(s_keep_alive_timer, 0);
                    s_turn_ending = false;
                    s_manual_end_turn = false;
                    s_audio_sent_in_turn = false;
                    s_server_activity_started = false;
                    s_first_audio_sent_ms = 0;
                    s_tx_pcm_len = 0;
                    s_tx_pcm_first_ms = 0;
                    s_tool_call_count = 0;
                    s_mute_response_audio = false;
                    s_rx_flush_requested = true;
                    if (s_tx_rb_mutex && xSemaphoreTake(s_tx_rb_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                        if (s_tx_audio_ringbuf) {
                            size_t dummy_sz;
                            uint8_t *dummy_it;
                            while ((dummy_it = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_sz, 0)) != NULL) {
                                vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_it);
                            }
                        }
                        xSemaphoreGive(s_tx_rb_mutex);
                    }
                    audio_hal_stop_playback();
                    audio_hal_set_turn_active(false);
                    stop_ws_client_safe();
                    if (!was_silent_tool_success) {
                        audio_hal_play_error_sound();
                    }
                    s_current_state = AI_STATE_IDLE;
                    ai_edge_stop_recording();
                    continue;
                }
            }

            // Xử lý CANCEL (Khớp lệnh cục bộ Model 2 hoặc No-Speech) -> Thoát êm đẹp không kêu lỗi
            if (notify_bits & CLOUD_EVT_CANCEL) {
                ESP_LOGI(TAG, "Worker handling CLOUD_EVT_CANCEL (Local match or no-speech)...");
                bool audio_was_sent = s_audio_sent_in_turn;
                s_current_state = AI_STATE_IDLE;
                s_turn_ending = false;
                s_manual_end_turn = false;
                s_audio_sent_in_turn = false;
                s_server_activity_started = false;
                s_first_audio_sent_ms = 0;
                s_tx_pcm_len = 0;
                s_tx_pcm_first_ms = 0;
                s_tool_call_count = 0;
                s_mute_response_audio = false;
                if (s_session_timer) xTimerStop(s_session_timer, 0);
                s_rx_flush_requested = true;
                if (s_tx_rb_mutex && xSemaphoreTake(s_tx_rb_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                    if (s_tx_audio_ringbuf) {
                        size_t dummy_sz;
                        uint8_t *dummy_it;
                        while ((dummy_it = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_sz, 0)) != NULL) {
                            vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_it);
                        }
                    }
                    xSemaphoreGive(s_tx_rb_mutex);
                }
                // Chỉ giữ kết nối ngầm 15s nếu chưa gửi audio nào; nếu đã gửi audio phải đóng WS để làm sạch phiên server
                if (!audio_was_sent && s_is_ws_connected && s_setup_complete && s_keep_alive_timer) {
                    xTimerStop(s_keep_alive_timer, 0);
                    xTimerStart(s_keep_alive_timer, 0);
                } else {
                    if (audio_was_sent) {
                        ESP_LOGW(TAG, "Cancel after audio was sent -> Closing WebSocket to clean server session.");
                    }
                    stop_ws_client_safe();
                }
                continue;
            }

            // Xử lý KEEP_ALIVE_EXP
            if (notify_bits & CLOUD_EVT_KEEP_ALIVE_EXP) {
                if (s_current_state == AI_STATE_IDLE && !s_turn_ending) {
                    ESP_LOGI(TAG, ">>> [KEEP-ALIVE EXPIRED] Hết 15s không có câu lệnh mới -> Đóng WebSocket an toàn từ Worker Task. <<<");
                    stop_ws_client_safe();
                    continue;
                }
            }

            // Xử lý START_TURN: Đã được start_turn() chuyển AI_STATE_BUFFERING. TUYỆT ĐỐI KHÔNG xả s_tx_audio_ringbuf ở đây!
            if (notify_bits & CLOUD_EVT_START_TURN) {
                s_tx_pcm_len = 0;
                s_tx_pcm_first_ms = 0;
            }

            // Xử lý COMMIT_TURN: Chuyển sang RECORDING và bắt đầu truyền dữ liệu
            if (notify_bits & CLOUD_EVT_COMMIT_TURN) {
                s_current_state = AI_STATE_RECORDING;
            }

            // Xử lý END_TURN
            if (notify_bits & CLOUD_EVT_END_TURN) {
                if (s_current_state == AI_STATE_BUFFERING) {
                    s_current_state = AI_STATE_RECORDING;
                }
                s_turn_ending = true;
            }
        }

        // 2. Khi ở trạng thái rảnh (IDLE) và không có lượt đang dứt:
        if (s_current_state == AI_STATE_IDLE && !s_turn_ending) {
            if (s_tx_rb_mutex && xSemaphoreTake(s_tx_rb_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (s_tx_audio_ringbuf) {
                    size_t dummy_size = 0;
                    uint8_t *dummy_item = NULL;
                    while ((dummy_item = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_size, 0)) != NULL) {
                        vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_item);
                    }
                }
                xSemaphoreGive(s_tx_rb_mutex);
            }
            continue;
        }

        // 3. Nếu KHÔNG phải AI_STATE_RECORDING (BUFFERING, WAITING_AI, EXECUTING_TOOL, WAITING_POST_TOOL_SPEECH, SPEAKING) -> continue ngay!
        if (s_current_state != AI_STATE_RECORDING) {
            continue;
        }

        // 4. Khi đang ghi âm (RECORDING) hoặc đang chờ xả hết âm thanh (turn ending):
        if (!s_is_ws_connected || !s_setup_complete) {
            ai_cloud_prewarm_session();

            int wait_ticks = 0;
            bool aborted_or_cancelled = false;
            while ((!s_is_ws_connected || !s_setup_complete) && 
                   wait_ticks < 50 && 
                   (s_current_state == AI_STATE_RECORDING || s_turn_ending)) {
                if (!s_is_ws_connected && !s_is_ws_connecting && !s_is_ws_stopping && !s_ws_task_active) {
                    ai_cloud_prewarm_session();
                }

                uint32_t wait_bits = 0;
                if (xTaskNotifyWait(0, 0xFFFFFFFF, &wait_bits, pdMS_TO_TICKS(100)) == pdTRUE) {
                    if (wait_bits & CLOUD_EVT_ABORT) {
                        aborted_or_cancelled = true;
                        if (s_tx_task_handle) {
                            xTaskNotify(s_tx_task_handle, CLOUD_EVT_ABORT, eSetBits);
                        }
                        break;
                    }
                    if (wait_bits & CLOUD_EVT_CANCEL) {
                        aborted_or_cancelled = true;
                        if (s_tx_task_handle) {
                            xTaskNotify(s_tx_task_handle, CLOUD_EVT_CANCEL, eSetBits);
                        }
                        break;
                    }
                    if (wait_bits & CLOUD_EVT_END_TURN) {
                        s_turn_ending = true;
                    }
                    if (wait_bits & CLOUD_EVT_WS_CONNECTED) {
                        send_setup_frame();
                    }
                }
                wait_ticks++;
            }

            if (aborted_or_cancelled) {
                continue;
            }

            if (s_current_state == AI_STATE_IDLE && !s_turn_ending) {
                ESP_LOGI(TAG, "Turn cancelled or finished during connection wait. Exiting cleanly.");
                continue;
            }

            if (!s_is_ws_connected || !s_setup_complete) {
                ESP_LOGE(TAG, "WebSocket connection failed or setup timeout! Aborting turn...");
                ai_cloud_abort();
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
        }

        if (s_current_state == AI_STATE_IDLE && !s_turn_ending) {
            continue;
        }

        // Watchdog 3.5s: Nếu đã stream audio lên Gemini nhưng Server Neural VAD chưa từng phát hiện giọng người -> Hủy êm
        if (s_audio_sent_in_turn && !s_server_activity_started && s_first_audio_sent_ms > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000;
            if ((now_ms - s_first_audio_sent_ms) >= 3500) {
                ESP_LOGW(TAG, ">>> [SERVER NO-SPEECH WATCHDOG] 3.5s không thấy giọng người -> Hủy êm (không phát loa)! <<<");
                silent_cancel_no_speech_turn();
                continue;
            }
        }

        // 5. Gom gói PCM 180ms (5760 bytes) vào s_tx_pcm_buf (PSRAM) để giảm tần suất xung Wi-Fi TX
        if (s_tx_pcm_buf && s_tx_pcm_len < TX_PCM_BATCH_BYTES) {
            size_t space = (TX_PCM_BATCH_BYTES - s_tx_pcm_len) & ~1;
            if (space >= 2 && s_tx_rb_mutex && xSemaphoreTake(s_tx_rb_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                while (space >= 2) {
                    size_t rcv_sz = 0;
                    uint8_t *it = (uint8_t *)xRingbufferReceiveUpTo(s_tx_audio_ringbuf, &rcv_sz, 0, space);
                    if (!it || rcv_sz == 0) {
                        if (it) vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)it);
                        break;
                    }
                    size_t copy_len = rcv_sz & ~1;
                    if (copy_len == 0) {
                        vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)it);
                        break;
                    }
                    if (s_tx_pcm_len == 0) {
                        s_tx_pcm_first_ms = esp_timer_get_time() / 1000;
                    }
                    memcpy(s_tx_pcm_buf + s_tx_pcm_len, it, copy_len);
                    s_tx_pcm_len += copy_len;
                    space -= copy_len;
                    vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)it);
                }
                xSemaphoreGive(s_tx_rb_mutex);
            }
        }

        int64_t now_tx_ms = esp_timer_get_time() / 1000;
        bool should_flush = (s_tx_pcm_len >= TX_PCM_BATCH_BYTES) ||
                            (s_tx_pcm_len > 0 && s_turn_ending) ||
                            (s_tx_pcm_len > 0 && s_tx_pcm_first_ms > 0 && (now_tx_ms - s_tx_pcm_first_ms >= 200));

        if (should_flush) {
            // Khuếch đại tuyến tính cố định x3 (giữ nguyên tỷ lệ SNR tự nhiên, không thổi bùng nhiễu Wi-Fi khi im lặng)
            int16_t *pcm = (int16_t *)s_tx_pcm_buf;
            size_t num_samples = s_tx_pcm_len / sizeof(int16_t);
            for (size_t i = 0; i < num_samples; i++) {
                int32_t scaled = (int32_t)pcm[i] * 3;
                if (scaled > 32000) scaled = 32000;
                else if (scaled < -32000) scaled = -32000;
                pcm[i] = (int16_t)scaled;
            }

            if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (ws_client && s_is_ws_connected && s_setup_complete) {
                    if (s_tx_base64_buf && s_tx_json_buf) {
                        size_t base64_len = 0;
                        if (mbedtls_base64_encode(s_tx_base64_buf, TX_BASE64_BUF_SIZE - 1, &base64_len, s_tx_pcm_buf, s_tx_pcm_len) == 0) {
                            s_tx_base64_buf[base64_len] = '\0';

                            int json_len = snprintf(s_tx_json_buf, TX_JSON_BUF_SIZE,
                                     "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}",
                                     (char *)s_tx_base64_buf);
                            
                            if (json_len > 0 && json_len < TX_JSON_BUF_SIZE) {
                                int sent = esp_websocket_client_send_text(ws_client, s_tx_json_buf, json_len, pdMS_TO_TICKS(1500));
                                if (sent > 0) {
                                    if (!s_audio_sent_in_turn) {
                                        s_first_audio_sent_ms = esp_timer_get_time() / 1000;
                                    }
                                    s_audio_sent_in_turn = true;
                                } else {
                                    ESP_LOGW(TAG, "Audio send_text poll timeout or write fail (sent=%d)", sent);
                                }
                            }
                        }
                    }
                }
                xSemaphoreGive(ws_mutex);
            }

            s_tx_pcm_len = 0;
            s_tx_pcm_first_ms = 0;

            // Xả nhanh phần đệm khi dứt câu, vẫn chừa khe nhỏ nhường lwIP
            vTaskDelay(pdMS_TO_TICKS(s_turn_ending ? 5 : 10));
        } else {
            // RingBuffer và s_tx_pcm_buf đều đã rỗng hoàn toàn: bơm 1.44s im lặng chốt Server VAD rồi gửi audioStreamEnd: true
            if (s_turn_ending && s_current_state == AI_STATE_RECORDING && s_tx_pcm_len == 0) {
                if (!s_server_activity_started && (!s_manual_end_turn || !s_audio_sent_in_turn)) {
                    ESP_LOGW(TAG, ">>> [AUTO-END NO-SPEECH GUARD] Kết thúc lượt lúc Server VAD chưa thấy giọng người -> Hủy êm (không gửi audioStreamEnd)! <<<");
                    silent_cancel_no_speech_turn();
                    continue;
                }

                // Tái sử dụng buffer tĩnh PSRAM để tạo 1 khung im lặng 180ms và bơm 8 lần (1440ms > 1200ms silenceDurationMs)
                if (s_tx_pcm_buf && s_tx_base64_buf && s_tx_json_buf) {
                    memset(s_tx_pcm_buf, 0, TX_PCM_BATCH_BYTES);
                    size_t sil_b64_len = 0;
                    if (mbedtls_base64_encode(s_tx_base64_buf, TX_BASE64_BUF_SIZE - 1, &sil_b64_len, s_tx_pcm_buf, TX_PCM_BATCH_BYTES) == 0) {
                        s_tx_base64_buf[sil_b64_len] = '\0';
                        int sil_json_len = snprintf(s_tx_json_buf, TX_JSON_BUF_SIZE,
                            "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}",
                            (char *)s_tx_base64_buf);
                        if (sil_json_len > 0 && sil_json_len < TX_JSON_BUF_SIZE) {
                            ESP_LOGI(TAG, ">>> [TAIL SILENCE] Đang bơm tối đa 1.44s im lặng sạch để chốt Server VAD... <<<");
                            for (int s_idx = 0; s_idx < 8; s_idx++) {
                                if (s_current_state != AI_STATE_RECORDING || !s_is_ws_connected) {
                                    break;
                                }
                                if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                                    int sent = 0;
                                    if (ws_client && s_is_ws_connected && s_setup_complete) {
                                        sent = esp_websocket_client_send_text(ws_client, s_tx_json_buf, sil_json_len, pdMS_TO_TICKS(1000));
                                    }
                                    xSemaphoreGive(ws_mutex);
                                    if (sent <= 0) {
                                        break;
                                    }
                                }
                                vTaskDelay(pdMS_TO_TICKS(10));
                            }
                        }
                    }
                }

                s_turn_ending = false;
                s_manual_end_turn = false;
                if (s_current_state != AI_STATE_RECORDING) {
                    // Server VAD đã chuyển sang WAITING_AI / SPEAKING trong lúc bơm im lặng -> không gửi thừa audioStreamEnd
                    continue;
                }

                s_current_state = AI_STATE_WAITING_AI;
                expecting_first_chunk = true;

                bool end_sent = false;
                if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    if (ws_client != NULL && s_is_ws_connected) {
                        const char *audio_stream_end_msg = "{\"realtimeInput\":{\"audioStreamEnd\":true}}";
                        int sent = esp_websocket_client_send_text(ws_client, audio_stream_end_msg, strlen(audio_stream_end_msg), pdMS_TO_TICKS(1500));
                        if (sent > 0) {
                            end_sent = true;
                            ESP_LOGI(TAG, "Sent audioStreamEnd:true (turn complete, all audio flushed). Waiting for AI response...");
                            if (s_session_timer) {
                                xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS), 0);
                                xTimerStart(s_session_timer, 0);
                            }
                        } else {
                            ESP_LOGE(TAG, "Failed to send audioStreamEnd! Aborting turn...");
                        }
                    } else {
                        ESP_LOGE(TAG, "WebSocket disconnected before sending audioStreamEnd! Aborting turn...");
                    }
                    xSemaphoreGive(ws_mutex);
                } else {
                    ESP_LOGE(TAG, "Failed to take ws_mutex before sending audioStreamEnd! Aborting turn...");
                }

                if (!end_sent) {
                    ai_cloud_abort();
                }
            }
            // Nhường CPU khi TX RingBuffer đang tạm rỗng chờ mic để không làm đói cmd_infer_task và IDLE1
            if (s_current_state == AI_STATE_RECORDING) {
                vTaskDelay(pdMS_TO_TICKS(15));
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

    // Rate-limit backoff: Chờ tối thiểu 300ms sau khi đứt kết nối / hủy trước khi cho phép kết nối lại (đủ giải phóng socket cũ)
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if (s_last_disconnect_time_ms > 0 && (now - s_last_disconnect_time_ms < WS_RECONNECT_COOLDOWN_MS)) {
        ESP_LOGD(TAG, "WebSocket reconnect cooldown active (%lu ms remaining)...",
                 (unsigned long)(WS_RECONNECT_COOLDOWN_MS - (now - s_last_disconnect_time_ms)));
        return;
    }

    if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (ws_client == NULL) {
            ESP_LOGI(TAG, "Initializing WebSocket connection to Gemini Live...");
            esp_websocket_client_config_t websocket_cfg = {
                .uri = GEMINI_WS_URL,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .buffer_size = 24576, // 24KB (>16KB threshold -> cấp phát vào PSRAM, đọc trọn vẹn JSON Base64 chunk không phân mảnh)
                .task_stack = 8192,   // Stack 8KB cấp phát trong PSRAM (an toàn tuyệt đối cho TLS & mbedTLS handshake)
                .task_core_id_set = true,
                .task_core_id = 0,    // Ghim luồng WebSocket cố định vào Core 0 (Nhân Mạng)
                .task_prio = 6,
                .network_timeout_ms = 20000, // 20s: Chống EAGAIN timeout cắt ngang frame đang truyền dở
                .reconnect_timeout_ms = 0,
                .disable_auto_reconnect = true, // ai_cloud tự quản lý prewarm + 15s keep-alive, không chạy ngầm tự nối lại khi IDLE
                .keep_alive_enable = true,
                .keep_alive_idle = 10,
                .keep_alive_interval = 5,
                .keep_alive_count = 3,
            };
            ws_client = esp_websocket_client_init(&websocket_cfg);
            esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)ws_client);
            s_is_ws_connecting = true;
            esp_err_t err = esp_websocket_client_start(ws_client);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_websocket_client_start failed: 0x%x", err);
                s_is_ws_connecting = false;
            }
        } else if (s_is_ws_connected && s_setup_complete) {
            ESP_LOGI(TAG, ">>> [KEEP-ALIVE POOL HIT] WebSocket đã kết nối & Setup sẵn sàng (0ms độ trễ TLS)! <<<");
        } else if (s_ws_task_active) {
            ESP_LOGD(TAG, "WebSocket task đang hoàn tất đóng/kết nối, chờ hoàn thành...");
        } else if (!s_is_ws_stopping) {
            ESP_LOGI(TAG, "Starting WebSocket in background...");
            s_is_ws_connecting = true;
            esp_err_t err = esp_websocket_client_start(ws_client);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_websocket_client_start failed: 0x%x -> Resetting client", err);
                xSemaphoreGive(ws_mutex);
                stop_ws_client_safe();
                return;
            }
        }
        xSemaphoreGive(ws_mutex);
    }
}

void ai_cloud_init(void) {
    ws_mutex = xSemaphoreCreateMutex();
    s_tx_rb_mutex = xSemaphoreCreateMutex();
    s_tool_resp_mutex = xSemaphoreCreateMutex();

    s_tx_pcm_buf = (uint8_t *)heap_caps_malloc(TX_PCM_BATCH_BYTES, MALLOC_CAP_SPIRAM);
    s_tx_base64_buf = (unsigned char *)heap_caps_malloc(TX_BASE64_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_tx_json_buf = (char *)heap_caps_malloc(TX_JSON_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_ws_rx_buf = (char *)heap_caps_malloc(WS_RX_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_rx_pcm_buf = (unsigned char *)heap_caps_malloc(RX_PCM_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_tx_pcm_buf || !s_tx_base64_buf || !s_tx_json_buf || !s_ws_rx_buf || !s_rx_pcm_buf) {
        ESP_LOGE(TAG, "Failed to allocate static PSRAM buffers for TX/RX!");
    }

    // Khởi tạo TX Audio RingBuffer 256KB trong PSRAM (chứa được ~8 giây âm thanh PCM 16kHz không bao giờ tràn)
    s_tx_audio_ringbuf = xRingbufferCreateWithCaps(256 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!s_tx_audio_ringbuf) {
        ESP_LOGE(TAG, "Failed to create 256KB TX RingBuffer in PSRAM! Fallback to 128KB...");
        s_tx_audio_ringbuf = xRingbufferCreateWithCaps(128 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    }

    // Khởi tạo RX JSON RingBuffer (RINGBUF_TYPE_NOSPLIT) trong PSRAM: 768KB, fallback 512KB
    s_rx_json_ringbuf = xRingbufferCreateWithCaps(768 * 1024, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    if (!s_rx_json_ringbuf) {
        ESP_LOGW(TAG, "Failed to create 768KB RX JSON RingBuffer in PSRAM! Fallback to 512KB...");
        s_rx_json_ringbuf = xRingbufferCreateWithCaps(512 * 1024, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
    }

    // Tạo luồng truyền âm thanh mạng độc lập & FSM Worker (Core 1, Priority 5, Stack 12KB trong PSRAM)
    xTaskCreatePinnedToCoreWithCaps(cloud_tx_task, "cloud_tx_task", 12288, NULL, 5, &s_tx_task_handle, 1, MALLOC_CAP_SPIRAM);
    // Tạo luồng Consumer parse JSON & decode Base64 PCM (Core 1, Priority 7, Stack 12KB trong PSRAM)
    xTaskCreatePinnedToCoreWithCaps(cloud_rx_process_task, "cloud_rx_task", 12288, NULL, 7, &s_rx_task_handle, 1, MALLOC_CAP_SPIRAM);

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
    s_rx_flush_requested = true;

    // 2. Lấy s_tx_rb_mutex, xả sạch mọi byte tồn dư trong s_tx_audio_ringbuf trước khi nhận pre-roll từ ai_edge
    if (s_tx_rb_mutex && xSemaphoreTake(s_tx_rb_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_tx_audio_ringbuf) {
            size_t dummy_sz;
            uint8_t *dummy_it;
            while ((dummy_it = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_sz, 0)) != NULL) {
                vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_it);
            }
        }
        xSemaphoreGive(s_tx_rb_mutex);
    }

    // 3. Đảm bảo kết nối đã được kích hoạt
    ai_cloud_prewarm_session();

    // 4. Chuyển sang trạng thái đệm BUFFERING (chờ Model 2 chốt lệnh đơn hay câu hỏi mở)
    s_current_state = AI_STATE_BUFFERING;
    s_audio_sent_in_turn = false;
    s_turn_ending = false;
    s_manual_end_turn = false;
    s_server_activity_started = false;
    s_first_audio_sent_ms = 0;
    s_tx_pcm_len = 0;
    s_tx_pcm_first_ms = 0;
    s_tool_call_count = 0;
    s_mute_response_audio = false;

    if (s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_START_TURN, eSetBits);
    }

    return true;
}

void ai_cloud_commit_turn(void) {
    if (s_current_state == AI_STATE_BUFFERING) {
        ESP_LOGI(TAG, ">>> [BUFFERED-COMMIT] Commit turn: BUFFERING -> RECORDING. Starting live stream to Gemini Live! <<<");
        s_current_state = AI_STATE_RECORDING;
        if (s_tx_task_handle) {
            xTaskNotify(s_tx_task_handle, CLOUD_EVT_COMMIT_TURN, eSetBits);
        }
    }
}

void ai_cloud_send_audio_chunk(const uint8_t *data, size_t size) {
    if ((s_current_state != AI_STATE_RECORDING && s_current_state != AI_STATE_BUFFERING) || !data || size == 0 || s_turn_ending) {
        return;
    }

    if (!s_tx_audio_ringbuf) return;

    // Cắt dữ liệu lớn thành các chunk nhỏ 1920 bytes (~60ms) để không vượt quá khả năng đệm 1 frame
    const size_t max_chunk = 1920;
    size_t offset = 0;
    while (offset < size) {
        size_t to_send = size - offset;
        if (to_send > max_chunk) to_send = max_chunk;

        if (s_tx_rb_mutex && xSemaphoreTake(s_tx_rb_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            BaseType_t ok = xRingbufferSend(s_tx_audio_ringbuf, (void *)(data + offset), to_send, 0);
            xSemaphoreGive(s_tx_rb_mutex);
            if (ok != pdTRUE) {
                ESP_LOGW(TAG, "TX Audio Ringbuffer full! Dropped %u bytes (offset %u/%u)",
                         (unsigned int)to_send, (unsigned int)offset, (unsigned int)size);
                break;
            }
        }
        offset += to_send;
    }
}

void ai_cloud_end_turn(void) {
    if (s_current_state != AI_STATE_BUFFERING && s_current_state != AI_STATE_RECORDING) {
        return;
    }
    ESP_LOGI(TAG, ">>> [CLIENT FALLBACK] Dứt câu nói. Đang xả toàn bộ audio còn lại trong RingBuffer để gửi audioStreamEnd... <<<");
    if (s_current_state == AI_STATE_BUFFERING) {
        s_current_state = AI_STATE_RECORDING;
    }
    s_manual_end_turn = false;
    s_turn_ending = true;
    if (s_tx_task_handle) {
        xTaskNotify(s_tx_task_handle, CLOUD_EVT_END_TURN, eSetBits);
    }
}

void ai_cloud_end_turn_manual(void) {
    if (s_current_state != AI_STATE_BUFFERING && s_current_state != AI_STATE_RECORDING) {
        return;
    }
    ESP_LOGI(TAG, ">>> [MANUAL FINISH] Người dùng bấm gửi ngay -> Xả audio và chốt phản hồi... <<<");
    if (s_current_state == AI_STATE_BUFFERING) {
        s_current_state = AI_STATE_RECORDING;
    }
    s_manual_end_turn = true;
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
    ESP_LOGI(TAG, "Hủy lượt nói (khớp lệnh cục bộ hoặc no-speech)...");
    if (s_current_state == AI_STATE_BUFFERING) {
        if (s_tx_rb_mutex && xSemaphoreTake(s_tx_rb_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_tx_audio_ringbuf) {
                size_t dummy_sz;
                uint8_t *dummy_it;
                while ((dummy_it = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_sz, 0)) != NULL) {
                    vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_it);
                }
            }
            xSemaphoreGive(s_tx_rb_mutex);
        }
        s_current_state = AI_STATE_IDLE;
        s_turn_ending = false;
        s_manual_end_turn = false;
        s_audio_sent_in_turn = false;
        s_server_activity_started = false;
        s_first_audio_sent_ms = 0;
        s_tx_pcm_len = 0;
        s_tx_pcm_first_ms = 0;
        s_tool_call_count = 0;
        s_mute_response_audio = false;
        if (s_session_timer) xTimerStop(s_session_timer, 0);
        if (s_is_ws_connected && s_setup_complete && s_keep_alive_timer) {
            xTimerStop(s_keep_alive_timer, 0);
            xTimerStart(s_keep_alive_timer, 0);
            ESP_LOGI(TAG, ">>> [KEEP-ALIVE POOL] Cancel in BUFFERING: Giữ WebSocket kết nối sống trong 15s sẵn sàng... <<<");
        }
        return;
    }
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

bool ai_cloud_is_accepting_audio(void) {
    return (s_current_state == AI_STATE_BUFFERING || s_current_state == AI_STATE_RECORDING);
}

bool ai_cloud_is_ready(void) {
    return (s_is_ws_connected && s_setup_complete);
}


