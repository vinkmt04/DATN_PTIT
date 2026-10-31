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
#define GEMINI_API_KEY "AQ.Ab8RN6KNQ1b843NFqyLk4Vld15qwofwZ7LOFN5wEohnIMm7Wfw"
#define GEMINI_WS_URL "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=" GEMINI_API_KEY

// Kích thước tĩnh cho bộ đệm Base64 và JSON (cấp phát 1 lần trong PSRAM)
#define TX_BASE64_BUF_SIZE 8192
#define TX_JSON_BUF_SIZE   10240
static unsigned char *s_tx_base64_buf = NULL;
static char *s_tx_json_buf = NULL;

// Timeouts
#define RESPONSE_TIMEOUT_MS  8000   // Chờ phản hồi đầu tiên từ Gemini: 8s
#define CHUNK_TIMEOUT_MS     4000   // Chờ chunk tiếp theo giữa chừng: 4s
#define WS_KEEP_ALIVE_TIMEOUT_MS 15000 // Giữ kết nối ngầm 15 giây (Keep-Alive Idle Pool)

typedef enum {
    AI_STATE_IDLE = 0,
    AI_STATE_RECORDING,
    AI_STATE_WAITING_AI,
    AI_STATE_SPEAKING,
} ai_gateway_state_t;

static volatile ai_gateway_state_t s_current_state = AI_STATE_IDLE;
static TimerHandle_t s_session_timer = NULL;
static TimerHandle_t s_keep_alive_timer = NULL;
static volatile bool s_keep_alive_expired = false;
static SemaphoreHandle_t ws_mutex = NULL;

static esp_websocket_client_handle_t ws_client = NULL;
static volatile bool s_is_ws_connected = false;
static volatile bool s_is_ws_connecting = false;
static volatile bool s_setup_complete = false;

// Cờ đồng bộ tín hiệu thoại: đảm bảo gửi activityStart trước audio, và activityEnd sau cùng khi xả hết buffer
static volatile bool s_need_send_activity_start = false;
static volatile bool s_turn_ending = false;
static volatile bool s_is_aborting = false;

// RingBuffer TX: Chứa âm thanh mic từ ai_edge truyền sang (64KB trong PSRAM ~ 2 giây âm thanh)
// Giúp luồng thu âm mic KHÔNG BAO GIỜ BỊ BLOCK bởi mạng!
static RingbufHandle_t s_tx_audio_ringbuf = NULL;
static TaskHandle_t s_tx_task_handle = NULL;

// Buffer gom các frame websocket bị phân mảnh
static char *ws_rx_buffer = NULL;
static size_t ws_rx_len = 0;

static bool expecting_first_chunk = false;

static void* cjson_spiram_malloc(size_t size) {
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!p) {
        p = malloc(size);
    }
    return p;
}
static void cjson_spiram_free(void* ptr) {
    if (ptr) {
        heap_caps_free(ptr);
    }
}

static void abort_task(void *arg) {
    ESP_LOGE(TAG, "Network Drop/Timeout Detected! Aborting turn immediately.");
    s_current_state = AI_STATE_IDLE;
    s_turn_ending = false;
    s_need_send_activity_start = false;
    
    // 1. Dừng ngay tác vụ ghi âm ở tầng Edge
    ai_edge_stop_recording();

    // 2. Dừng bộ đếm thời gian
    if (s_session_timer) {
        xTimerStop(s_session_timer, 0);
    }

    // 3. Xóa sạch RingBuffer TX cũ
    if (s_tx_audio_ringbuf) {
        size_t dummy_size;
        uint8_t *dummy_item;
        while ((dummy_item = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_size, 0)) != NULL) {
            vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_item);
        }
    }

    // 4. Phát giọng nói báo lỗi tiếng Việt
    audio_hal_set_turn_active(false);
    audio_hal_play_error_sound();

    // 5. Ngắt phiên WebSocket để giải phóng triệt để session cũ trên Google server.
    // Sẽ chỉ kết nối lại On-Demand khi có lượt nói mới (tránh reconnect liên tục làm nghẽn CPU/mạng).
    if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (ws_client) {
            if (esp_websocket_client_is_connected(ws_client)) {
                ESP_LOGW(TAG, "Stopping WebSocket session (will reconnect on next turn)...");
                esp_websocket_client_stop(ws_client);
            }
            s_is_ws_connected = false;
            s_is_ws_connecting = false;
            s_setup_complete = false;
        }
        xSemaphoreGive(ws_mutex);
    }

    s_is_aborting = false;
    vTaskDelete(NULL);
}

void ai_cloud_abort(void) {
    if (!s_is_aborting) {
        s_is_aborting = true;
        xTaskCreate(abort_task, "abort_task", 4096, NULL, 5, NULL);
    }
}

static void session_timer_cb(TimerHandle_t xTimer) {
    switch (s_current_state) {
    case AI_STATE_WAITING_AI:
        ESP_LOGE(TAG, "Response Timeout (8s). No audio received from API!");
        ai_cloud_abort();
        break;
    case AI_STATE_SPEAKING:
        ESP_LOGE(TAG, "Chunk Timeout (%ds). Network dropped mid-sentence!", CHUNK_TIMEOUT_MS / 1000);
        ai_cloud_abort();
        break;
    default:
        break;
    }
}

static void parse_server_message(const char *json_string) {
    cJSON *root = cJSON_Parse(json_string);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", json_string);
        return;
    }

    // 1. Kiểm tra xác nhận hoàn tất thiết lập từ Gemini Live API
    cJSON *setupComplete = cJSON_GetObjectItem(root, "setupComplete");
    if (setupComplete) {
        ESP_LOGI(TAG, ">>> [GEMINI LIVE] Setup Complete! Connection Ready for Realtime Audio. <<<");
        s_setup_complete = true;
        cJSON_Delete(root);
        return;
    }

    // 2. Bắt lỗi từ máy chủ Google API
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        ESP_LOGE(TAG, "Gemini API Server Error: %s", (msg && cJSON_IsString(msg)) ? msg->valuestring : "Unknown error");
        ai_cloud_abort();
        cJSON_Delete(root);
        return;
    }

    // 3. Xử lý nội dung máy chủ trả về
    cJSON *serverContent = cJSON_GetObjectItem(root, "serverContent");
    if (serverContent) {
        // Nhận diện giọng nói của Người Dùng (STT)
        cJSON *inputTranscription = cJSON_GetObjectItem(serverContent, "inputTranscription");
        if (inputTranscription) {
            cJSON *user_text = cJSON_GetObjectItem(inputTranscription, "text");
            if (user_text && cJSON_IsString(user_text) && strlen(user_text->valuestring) > 0) {
                ESP_LOGW(TAG, "User Said (STT): \"%s\"", user_text->valuestring);
            }
        }

        // Nội dung trả lời của Gemini (Model Turn)
        cJSON *modelTurn = cJSON_GetObjectItem(serverContent, "modelTurn");
        if (modelTurn) {
            cJSON *parts = cJSON_GetObjectItem(modelTurn, "parts");
            if (cJSON_IsArray(parts)) {
                cJSON *part;
                cJSON_ArrayForEach(part, parts) {
                    cJSON *text = cJSON_GetObjectItem(part, "text");
                    if (text && cJSON_IsString(text)) {
                        ESP_LOGI(TAG, "Gemini Reply: %s", text->valuestring);
                        if (strncmp(text->valuestring, "<CMD:", 5) == 0) {
                            mesh_controller_send_command(text->valuestring);
                        }
                    }
                    cJSON *inlineData = cJSON_GetObjectItem(part, "inlineData");
                    if (inlineData) {
                        cJSON *data = cJSON_GetObjectItem(inlineData, "data");
                        if (data && cJSON_IsString(data)) {
                            // Đây là audio PCM
                            if (s_current_state == AI_STATE_WAITING_AI) {
                                s_current_state = AI_STATE_SPEAKING;
                                if (s_session_timer) {
                                    xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(CHUNK_TIMEOUT_MS), 0);
                                    xTimerStart(s_session_timer, 0);
                                }
                            } else if (s_current_state == AI_STATE_SPEAKING) {
                                if (s_session_timer) {
                                    xTimerReset(s_session_timer, 0);
                                }
                            } else if (s_current_state == AI_STATE_RECORDING) {
                                // SERVER VAD KÍCH HOẠT: Gemini đã nhận đủ ý & bắt đầu trả lời
                                ESP_LOGI(TAG, ">>> [SERVER VAD] Gemini phát hiện dứt câu sớm & bắt đầu trả lời! Dừng thu âm.");
                                s_current_state = AI_STATE_SPEAKING;
                                ai_edge_stop_recording();
                                if (s_session_timer) {
                                    xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(CHUNK_TIMEOUT_MS), 0);
                                    xTimerStart(s_session_timer, 0);
                                }
                            } else {
                                ESP_LOGD(TAG, "Dropping lingering audio chunk in state %d", s_current_state);
                                continue;
                            }

                            if (expecting_first_chunk) {
                                expecting_first_chunk = false;
                                audio_hal_start_buffering(); // Kích hoạt đệm Jitter chống giật
                                audio_hal_set_turn_active(true);
                            }
                            
                            size_t pcm_len = 0;
                            size_t b64_len = strlen(data->valuestring);
                            unsigned char *pcm_data = (unsigned char *)heap_caps_malloc(b64_len, MALLOC_CAP_SPIRAM);
                            if (!pcm_data) {
                                pcm_data = (unsigned char *)malloc(b64_len);
                            }
                            if (pcm_data) {
                                mbedtls_base64_decode(pcm_data, b64_len, &pcm_len, (const unsigned char*)data->valuestring, b64_len);
                                ESP_LOGD(TAG, "Received Audio PCM: %d bytes", pcm_len);
                                audio_hal_play_chunk(pcm_data, pcm_len);
                                heap_caps_free(pcm_data);
                            }
                        }
                    }
                }
            }
        }

        cJSON *turnComplete = cJSON_GetObjectItem(serverContent, "turnComplete");
        cJSON *interrupted = cJSON_GetObjectItem(serverContent, "interrupted");
        cJSON *generationComplete = cJSON_GetObjectItem(serverContent, "generationComplete");
        bool is_turn_finished = (turnComplete && cJSON_IsTrue(turnComplete)) ||
                                (interrupted && cJSON_IsTrue(interrupted)) ||
                                (generationComplete && cJSON_IsTrue(generationComplete));
        if (is_turn_finished && s_current_state != AI_STATE_IDLE) {
            ESP_LOGI(TAG, "Model turn complete. Waiting for next user input...");
            s_current_state = AI_STATE_IDLE;
            if (s_session_timer) {
                xTimerStop(s_session_timer, 0);
            }
            audio_hal_force_start_playing();
            audio_hal_set_turn_active(false);
        }
    }
    cJSON_Delete(root);
}

static void send_setup_frame(void) {
    if (ws_client && s_is_ws_connected) {
        s_setup_complete = false;
        // Bật disabled: true để Gemini Live trao quyền VAD hoàn toàn cho client (Espressif WebRTC Hardware VAD).
        // Khi đó, client quyết định chính xác thời điểm gửi activityStart và activityEnd!
        const char *setup_msg = "{\"setup\":{\"model\":\"models/gemini-3.8-live\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]},\"systemInstruction\":{\"parts\":[{\"text\":\"You are a helpful AI assistant. Always respond in the same language that the user speaks. If you hear silence, background noise, or unintelligible sounds, you MUST reply exactly 'Tôi chưa nghe rõ, vui lòng thử lại' in Vietnamese and nothing else. Default language is Vietnamese.\"}]},\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":true}},\"inputAudioTranscription\":{}}}";
        esp_websocket_client_send_text(ws_client, setup_msg, strlen(setup_msg), pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "Sent Setup message (Client WebRTC VAD + Multi-language + STT). Waiting for setupComplete...");
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
        if (ws_rx_buffer) {
            heap_caps_free(ws_rx_buffer);
            ws_rx_buffer = NULL;
            ws_rx_len = 0;
        }
        if (s_current_state == AI_STATE_RECORDING || s_current_state == AI_STATE_WAITING_AI) {
            ESP_LOGE(TAG, "WebSocket disconnected mid-session! Aborting turn...");
            ai_cloud_abort();
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        s_is_ws_connected = false;
        s_is_ws_connecting = false;
        s_setup_complete = false;
        if (s_current_state == AI_STATE_RECORDING || s_current_state == AI_STATE_WAITING_AI) {
            ESP_LOGE(TAG, "WebSocket error mid-session! Aborting turn...");
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
            break;
        }

        if (data->op_code == 0x09) {
            // Ping Frame
            break;
        }

        if (data->data_ptr && data->data_len > 0) {
            if (data->payload_offset == 0) {
                if (ws_rx_buffer) heap_caps_free(ws_rx_buffer);
                ws_rx_buffer = heap_caps_malloc(data->payload_len + 1, MALLOC_CAP_SPIRAM);
                if (!ws_rx_buffer) {
                    ws_rx_buffer = malloc(data->payload_len + 1);
                }
                ws_rx_len = 0;
            } else if (ws_rx_buffer && data->payload_offset != ws_rx_len) {
                ESP_LOGE(TAG, "WebSocket payload offset mismatch! Dropping broken packet.");
                heap_caps_free(ws_rx_buffer);
                ws_rx_buffer = NULL;
                ws_rx_len = 0;
            }
            
            if (ws_rx_buffer && ws_rx_len + data->data_len <= data->payload_len) {
                memcpy(ws_rx_buffer + ws_rx_len, data->data_ptr, data->data_len);
                ws_rx_len += data->data_len;
            }
            
            if (ws_rx_len == data->payload_len && ws_rx_buffer) {
                ws_rx_buffer[ws_rx_len] = '\0';
                parse_server_message(ws_rx_buffer);
                heap_caps_free(ws_rx_buffer);
                ws_rx_buffer = NULL;
                ws_rx_len = 0;
            }
        }
        break;
    }
}

// Luồng truyền âm thanh độc lập (Consumer):
// Lấy từng chunk từ RingBuffer TX, mã hóa Base64 và gửi lên WebSocket.
// Khi lượt nói kết thúc (s_turn_ending == true), luồng này sẽ xả hết sạch RingBuffer rồi mới gửi activityEnd!
static void cloud_tx_task(void *arg) {
    ESP_LOGI(TAG, "Cloud TX Task Started (Dedicated Asynchronous Streaming).");

    while (1) {
        // 1. Khi ở trạng thái rảnh (IDLE) và không có lượt đang dứt:
        if (s_current_state == AI_STATE_IDLE && !s_turn_ending) {
            // Xử lý đóng WebSocket ngầm khi hết hạn 15s Keep-Alive an toàn từ cloud_tx_task (Stack 8KB)
            if (s_keep_alive_expired) {
                s_keep_alive_expired = false;
                ESP_LOGI(TAG, ">>> [KEEP-ALIVE EXPIRED] Hết 15s không có câu lệnh mới -> Đóng WebSocket an toàn từ cloud_tx_task (Stack 8KB). <<<");
                if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    if (ws_client) {
                        if (s_is_ws_connected || s_is_ws_connecting) {
                            esp_websocket_client_stop(ws_client);
                        }
                    }
                    s_is_ws_connected = false;
                    s_is_ws_connecting = false;
                    s_setup_complete = false;
                    xSemaphoreGive(ws_mutex);
                }
            }

            // Xả sạch bất kỳ byte rác nào còn sót lại trong RingBuffer
            size_t dummy_size = 0;
            uint8_t *dummy_item = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_size, pdMS_TO_TICKS(50));
            if (dummy_item) {
                vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_item);
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
                   (s_is_ws_connecting || s_is_ws_connected) && 
                   wait_ticks < 50 && 
                   (s_current_state == AI_STATE_RECORDING || s_turn_ending)) {
                vTaskDelay(pdMS_TO_TICKS(100)); // Chờ tối đa 5.0 giây
                wait_ticks++;
            }

            if (!s_is_ws_connected || !s_setup_complete) {
                ESP_LOGE(TAG, "WebSocket connection failed or setup timeout! Aborting turn...");
                ai_cloud_abort();
                continue;
            }
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

        // 4. Lấy từng chunk âm thanh từ RingBuffer để stream lên WebSocket (Tối đa 3840 bytes = 120ms để tự động gộp chunk khi mạng trễ)
        size_t item_size = 0;
        uint8_t *item = (uint8_t *)xRingbufferReceiveUpTo(s_tx_audio_ringbuf, &item_size, pdMS_TO_TICKS(20), 3840);

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
                                int sent = esp_websocket_client_send_text(ws_client, s_tx_json_buf, json_len, pdMS_TO_TICKS(3000));
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
        } else {
            // RingBuffer đã rỗng hoàn toàn (toàn bộ âm thanh câu nói đã được gửi lên Google)
            if (s_turn_ending && s_current_state == AI_STATE_RECORDING) {
                s_turn_ending = false;
                s_current_state = AI_STATE_WAITING_AI;
                expecting_first_chunk = true;

                if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    if (ws_client != NULL && s_is_ws_connected) {
                        const char *activity_end_msg = "{\"realtimeInput\":{\"activityEnd\":{}}}";
                        int sent = esp_websocket_client_send_text(ws_client, activity_end_msg, strlen(activity_end_msg), pdMS_TO_TICKS(2500));
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
    // Chỉ bật cờ hiệu để cloud_tx_task (Stack 8KB) thực hiện đóng an toàn,
    // tuyệt đối KHÔNG gọi esp_websocket_client_stop() trong task Tmr Svc (Stack chỉ 2KB)!
    s_keep_alive_expired = true;
}

void ai_cloud_prewarm_session(void) {
    s_keep_alive_expired = false;
    // 1. Dừng bộ đếm Keep-Alive nếu đang chạy (người dùng gọi tiếp câu lệnh)
    if (s_keep_alive_timer) {
        xTimerStop(s_keep_alive_timer, 0);
    }

    if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (ws_client == NULL) {
            ESP_LOGI(TAG, "Initializing WebSocket connection to Gemini Live...");
            esp_websocket_client_config_t websocket_cfg = {
                .uri = GEMINI_WS_URL,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .buffer_size = 8192,
                .task_stack = 8192,
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
        } else if (!s_is_ws_connected && !s_is_ws_connecting) {
            ESP_LOGI(TAG, "Starting WebSocket in background...");
            s_is_ws_connecting = true;
            esp_websocket_client_start(ws_client);
        }
        xSemaphoreGive(ws_mutex);
    }
}

void ai_cloud_init(void) {
    cJSON_Hooks hooks;
    hooks.malloc_fn = cjson_spiram_malloc;
    hooks.free_fn = cjson_spiram_free;
    cJSON_InitHooks(&hooks);

    ws_mutex = xSemaphoreCreateMutex();

    s_tx_base64_buf = (unsigned char *)heap_caps_malloc(TX_BASE64_BUF_SIZE, MALLOC_CAP_SPIRAM);
    s_tx_json_buf = (char *)heap_caps_malloc(TX_JSON_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_tx_base64_buf || !s_tx_json_buf) {
        ESP_LOGE(TAG, "Failed to allocate static PSRAM buffers for TX!");
    }

    // Khởi tạo TX Audio RingBuffer 256KB trong PSRAM (chứa được ~8 giây âm thanh PCM 16kHz không bao giờ tràn)
    s_tx_audio_ringbuf = xRingbufferCreateWithCaps(256 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    if (!s_tx_audio_ringbuf) {
        ESP_LOGE(TAG, "Failed to create 256KB TX RingBuffer in PSRAM! Fallback to 128KB...");
        s_tx_audio_ringbuf = xRingbufferCreateWithCaps(128 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
    }

    // Tạo luồng phát âm thanh mạng độc lập (Core 0, Priority 5)
    xTaskCreatePinnedToCore(cloud_tx_task, "cloud_tx_task", 8192, NULL, 5, &s_tx_task_handle, 0);

    s_session_timer = xTimerCreate("ai_session_tmr", pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS), pdFALSE, (void *)0, session_timer_cb);
    s_keep_alive_timer = xTimerCreate("ai_ws_keep_alive", pdMS_TO_TICKS(WS_KEEP_ALIVE_TIMEOUT_MS), pdFALSE, (void *)0, ws_keep_alive_timer_cb);
    ESP_LOGI(TAG, "ai_cloud initialized (Zero-Drop Audio Architecture + 15s Keep-Alive WebSocket Pool Ready).");
}

bool ai_cloud_start_turn(void) {
    // 1. Dừng ngay âm thanh cũ đang phát ra loa (Hỗ trợ Barge-In / ngắt lời)
    audio_hal_stop_playback();

    if (s_session_timer) {
        xTimerStop(s_session_timer, 0);
    }

    s_turn_ending = false;

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
    // cloud_tx_task sẽ đảm nhiệm việc gửi activityStart ngay trước chunk audio đầu tiên
    s_current_state = AI_STATE_RECORDING;
    s_need_send_activity_start = true;

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
}

void ai_cloud_stop_session(void) {
    s_current_state = AI_STATE_IDLE;
    s_turn_ending = false;
    s_need_send_activity_start = false;

    if (s_session_timer) {
        xTimerStop(s_session_timer, 0);
    }

    // Xóa sạch RingBuffer TX
    if (s_tx_audio_ringbuf) {
        size_t dummy_size;
        uint8_t *dummy_item;
        while ((dummy_item = (uint8_t *)xRingbufferReceive(s_tx_audio_ringbuf, &dummy_size, 0)) != NULL) {
            vRingbufferReturnItem(s_tx_audio_ringbuf, (void *)dummy_item);
        }
    }

    // Cơ chế Keep-Alive Idle Pool 15 giây:
    // Nếu WebSocket đang kết nối sẵn: KHÔNG ngắt vội, mà bật timer giữ kết nối sống 15s.
    // Nếu bạn nói tiếp hoặc gọi 'Hey Cherry' trong 15s đó -> socket dùng được ngay (0ms trễ TLS).
    if (s_is_ws_connected && s_setup_complete) {
        if (s_keep_alive_timer) {
            xTimerStop(s_keep_alive_timer, 0);
            xTimerStart(s_keep_alive_timer, 0);
            ESP_LOGI(TAG, ">>> [KEEP-ALIVE POOL] Giữ WebSocket kết nối sống trong 15s sẵn sàng cho lượt tiếp theo... <<<");
        }
    } else {
        // Chưa kết nối xong hoặc lỗi: dọn dẹp sạch
        if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            if (ws_client) {
                if (s_is_ws_connected || s_is_ws_connecting) {
                    ESP_LOGI(TAG, "Stopping WebSocket session on-demand...");
                    esp_websocket_client_stop(ws_client);
                }
            }
            s_is_ws_connected = false;
            s_is_ws_connecting = false;
            s_setup_complete = false;
            xSemaphoreGive(ws_mutex);
        }
    }
}

void ai_cloud_cancel_turn(void) {
    ESP_LOGI(TAG, "Hủy lượt nói (khớp lệnh cục bộ hoặc no-speech). Đóng phiên WebSocket...");
    ai_cloud_stop_session();
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
