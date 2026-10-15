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
#include "freertos/timers.h"
#include "freertos/semphr.h"

extern void ai_edge_stop_recording(void);

static const char *TAG = "ai_cloud";

// Live API (WebSockets) Configuration
#define GEMINI_API_KEY "AQ.Ab8RN6IdLF13NdQRBBf7Ge9dh9sFjg8VX3JxyUwLeFzdmUVELA"
#define GEMINI_WS_URL "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=" GEMINI_API_KEY

// Pre-allocated static buffers for Zero-Allocation Audio Streaming (PSRAM)
// Nâng lên 8KB và 10KB để chứa vừa vặn chunk 128ms (4096 bytes PCM -> 5464 bytes Base64)
#define TX_BASE64_BUF_SIZE 8192
#define TX_JSON_BUF_SIZE   10240
static unsigned char *s_tx_base64_buf = NULL;
static char *s_tx_json_buf = NULL;

// Timeouts
#define RESPONSE_TIMEOUT_MS  8000   // Chờ phản hồi đầu tiên từ Gemini: 8s
#define CHUNK_TIMEOUT_MS     4000   // Chờ chunk tiếp theo giữa chừng: 4s (phát hiện rớt mạng nhanh, tránh chờ lâu)
#define IDLE_TIMEOUT_MS      45000  // Ngắt WebSocket khi rảnh rỗi: 45s

typedef enum {
    AI_STATE_IDLE = 0,
    AI_STATE_RECORDING,
    AI_STATE_WAITING_AI,
    AI_STATE_SPEAKING,
} ai_gateway_state_t;

static volatile ai_gateway_state_t s_current_state = AI_STATE_IDLE;
static TimerHandle_t s_session_timer = NULL;
static SemaphoreHandle_t ws_mutex = NULL;

static esp_websocket_client_handle_t ws_client = NULL;

// Buffer gom các frame websocket bị phân mảnh
static char *ws_rx_buffer = NULL;
static size_t ws_rx_len = 0;

static bool expecting_first_chunk = false;

// Extern audio_hal functions
extern void audio_hal_play_error_sound(void);
extern void audio_hal_start_buffering(void);
extern void audio_hal_force_start_playing(void);
extern void audio_hal_stop_playback(void);

static void* cjson_spiram_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}
static void cjson_spiram_free(void* ptr) {
    heap_caps_free(ptr);
}

void ai_cloud_disconnect(void);

static void disconnect_task(void *arg) {
    s_current_state = AI_STATE_IDLE;
    ai_cloud_disconnect();
    vTaskDelete(NULL);
}

static void abort_task(void *arg) {
    ESP_LOGE(TAG, "Network Drop/Timeout Detected! Aborting turn immediately.");
    s_current_state = AI_STATE_IDLE;
    
    // 1. Dừng ngay tác vụ ghi âm ở tầng Edge
    ai_edge_stop_recording();

    // 2. Dừng bộ đếm thời gian
    if (s_session_timer) {
        xTimerStop(s_session_timer, 0);
    }

    // 3. Hủy kết nối hỏng ngay lập tức (dưới mutex) để lần bấm nút tiếp theo không bị xóa nhầm (triệt tiêu Race Condition)
    esp_websocket_client_handle_t client_to_destroy = NULL;
    if (ws_mutex) xSemaphoreTake(ws_mutex, portMAX_DELAY);
    if (ws_client) {
        client_to_destroy = ws_client;
        ws_client = NULL;
    }
    if (ws_mutex) xSemaphoreGive(ws_mutex);

    if (client_to_destroy) {
        esp_websocket_client_destroy(client_to_destroy);
    }

    // 4. Phát giọng nói báo lỗi tiếng Việt
    audio_hal_set_turn_active(false);
    audio_hal_play_error_sound();

    vTaskDelete(NULL);
}

static void session_timer_cb(TimerHandle_t xTimer) {
    switch (s_current_state) {
    case AI_STATE_WAITING_AI:
        ESP_LOGE(TAG, "Response Timeout (8s). No audio received from API!");
        xTaskCreate(abort_task, "abort_task", 4096, NULL, 5, NULL);
        break;
    case AI_STATE_SPEAKING:
        ESP_LOGE(TAG, "Chunk Timeout (%ds). Network dropped mid-sentence!", CHUNK_TIMEOUT_MS / 1000);
        xTaskCreate(abort_task, "abort_task", 4096, NULL, 5, NULL);
        break;
    case AI_STATE_IDLE:
        ESP_LOGI(TAG, "Idle timer expired (45s). Disconnecting WebSocket to save resources.");
        xTaskCreate(disconnect_task, "disconnect_task", 4096, NULL, 5, NULL);
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

    // Bắt lỗi từ máy chủ Google API (ví dụ: quota exceeded, invalid key, content policy)
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        ESP_LOGE(TAG, "Gemini API Server Error: %s", (msg && cJSON_IsString(msg)) ? msg->valuestring : "Unknown error");
        xTaskCreate(abort_task, "abort_task", 4096, NULL, 5, NULL);
        cJSON_Delete(root);
        return;
    }

    cJSON *serverContent = cJSON_GetObjectItem(root, "serverContent");
    if (serverContent) {
        // 1. Nhận diện giọng nói của Người Dùng (User Speech-to-Text)
        cJSON *inputTranscription = cJSON_GetObjectItem(serverContent, "inputTranscription");
        if (inputTranscription) {
            cJSON *user_text = cJSON_GetObjectItem(inputTranscription, "text");
            if (user_text && cJSON_IsString(user_text) && strlen(user_text->valuestring) > 0) {
                ESP_LOGW(TAG, "User Said (STT): \"%s\"", user_text->valuestring);
            }
        }

        // 2. Nội dung trả lời của Gemini (Model Turn)
        cJSON *modelTurn = cJSON_GetObjectItem(serverContent, "modelTurn");
        if (modelTurn) {
            cJSON *parts = cJSON_GetObjectItem(modelTurn, "parts");
            if (cJSON_IsArray(parts)) {
                cJSON *part;
                cJSON_ArrayForEach(part, parts) {
                    cJSON *text = cJSON_GetObjectItem(part, "text");
                    if (text && cJSON_IsString(text)) {
                        ESP_LOGI(TAG, "Gemini Reply: %s", text->valuestring);
                        // Xử lý lệnh nếu có <CMD...>
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
                            }

                            if (expecting_first_chunk) {
                                expecting_first_chunk = false;
                                audio_hal_start_buffering(); // Kích hoạt đệm Jitter
                                audio_hal_set_turn_active(true);
                            }
                            
                            size_t pcm_len = 0;
                            unsigned char *pcm_data = heap_caps_malloc(strlen(data->valuestring), MALLOC_CAP_SPIRAM);
                            if (pcm_data) {
                                mbedtls_base64_decode(pcm_data, strlen(data->valuestring), &pcm_len, (const unsigned char*)data->valuestring, strlen(data->valuestring));
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
        if (is_turn_finished) {
            ESP_LOGI(TAG, "Model turn complete (turnComplete=%d, generationComplete=%d, interrupted=%d). Waiting for user input...",
                     turnComplete ? cJSON_IsTrue(turnComplete) : 0,
                     generationComplete ? cJSON_IsTrue(generationComplete) : 0,
                     interrupted ? cJSON_IsTrue(interrupted) : 0);
            
            s_current_state = AI_STATE_IDLE;
            if (s_session_timer) {
                xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(IDLE_TIMEOUT_MS), 0);
                xTimerStart(s_session_timer, 0);
            }
            audio_hal_force_start_playing(); // Ép xả nốt Jitter Buffer nếu câu trả lời quá ngắn
            audio_hal_set_turn_active(false); // Báo audio_hal model đã nói xong, xả hết buffer thì mới idle
        } else if (!modelTurn) {
            ESP_LOGD(TAG, "Non-audio serverContent: %s", json_string);
        }
    } else {
        if (strcmp(json_string, "{}") != 0) {
            ESP_LOGD(TAG, "Other Server Msg: %s", json_string);
        }
    }
    cJSON_Delete(root);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
        if (ws_rx_buffer) {
            heap_caps_free(ws_rx_buffer);
            ws_rx_buffer = NULL;
            ws_rx_len = 0;
        }
        // Nếu rớt mạng khi đang ghi âm hoặc đang chờ AI trả lời -> báo lỗi ngay tức khắc!
        if (s_current_state == AI_STATE_RECORDING || s_current_state == AI_STATE_WAITING_AI) {
            ESP_LOGE(TAG, "WebSocket disconnected mid-session! Aborting turn...");
            xTaskCreate(abort_task, "abort_task", 4096, NULL, 5, NULL);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
        if (s_current_state == AI_STATE_RECORDING || s_current_state == AI_STATE_WAITING_AI) {
            ESP_LOGE(TAG, "WebSocket error mid-session! Aborting turn...");
            xTaskCreate(abort_task, "abort_task", 4096, NULL, 5, NULL);
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->payload_offset == 0) {
            ESP_LOGD(TAG, "WS Frame start: opcode=%d, payload_len=%d", data->op_code, data->payload_len);
        } else {
            ESP_LOGD(TAG, "WS fragment: data_len=%d, offset=%d/%d", data->data_len, data->payload_offset, data->payload_len);
        }
        
        // Reset timer nếu đang trong lượt trả lời của model hoặc đang ở trạng thái idle
        if (s_current_state == AI_STATE_SPEAKING && s_session_timer && xTimerIsTimerActive(s_session_timer)) {
            xTimerReset(s_session_timer, 0);
        } else if (s_current_state == AI_STATE_IDLE && s_session_timer && xTimerIsTimerActive(s_session_timer)) {
            xTimerReset(s_session_timer, 0);
        }

        if (data->op_code == 0x08) {
            ESP_LOGE(TAG, "WebSocket Closed by Server! payload_len=%d, data_len=%d", data->payload_len, data->data_len);
            if (data->data_len > 0) {
                ESP_LOGE(TAG, "Close Reason: %.*s", data->data_len, (char *)data->data_ptr);
            }
            if (s_current_state == AI_STATE_RECORDING || s_current_state == AI_STATE_WAITING_AI) {
                xTaskCreate(abort_task, "abort_task", 4096, NULL, 5, NULL);
            }
        } else if (data->op_code == 0x01 || data->op_code == 0x02 || data->op_code == 0x00) { // Text, Binary, or continuation
            // Gom dữ liệu
            if (data->payload_offset == 0) {
                if (ws_rx_buffer) heap_caps_free(ws_rx_buffer);
                ws_rx_buffer = heap_caps_malloc(data->payload_len + 1, MALLOC_CAP_SPIRAM);
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

void ai_cloud_disconnect(void) {
    s_current_state = AI_STATE_IDLE;
    if (s_session_timer) {
        xTimerStop(s_session_timer, 0);
    }
    audio_hal_set_turn_active(false);
    if (ws_mutex) xSemaphoreTake(ws_mutex, portMAX_DELAY);
    if (ws_client != NULL && esp_websocket_client_is_connected(ws_client)) {
        ESP_LOGI(TAG, "Disconnecting WebSocket gracefully...");
        esp_websocket_client_stop(ws_client);
    }
    if (ws_mutex) xSemaphoreGive(ws_mutex);
}

void ai_cloud_abort(void) {
    s_current_state = AI_STATE_IDLE;
    if (s_session_timer) {
        xTimerStop(s_session_timer, 0);
    }
    audio_hal_set_turn_active(false);
    if (ws_mutex) xSemaphoreTake(ws_mutex, portMAX_DELAY);
    if (ws_client != NULL) {
        ESP_LOGI(TAG, "Aborting WebSocket due to network error...");
        esp_websocket_client_destroy(ws_client);
        ws_client = NULL;
    }
    if (ws_mutex) xSemaphoreGive(ws_mutex);
}

void ai_cloud_on_playback_idle(void) {
    ESP_LOGD(TAG, "Audio playback idle notification received.");
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

    s_session_timer = xTimerCreate("ai_session_tmr", pdMS_TO_TICKS(45000), pdFALSE, (void *)0, session_timer_cb);
    ESP_LOGI(TAG, "ai_cloud initialized for Live API (WebSockets) - Unified State Machine");
}

bool ai_cloud_start_turn(void) {
    // 1. Dừng ngay âm thanh cũ đang phát ra loa (Hỗ trợ Barge-In / ngắt lời) và xả sạch RingBuffer
    audio_hal_stop_playback();

    if (s_session_timer) {
        xTimerStop(s_session_timer, 0);
    }

    if (ws_mutex) xSemaphoreTake(ws_mutex, portMAX_DELAY);
    bool is_new_connection = false;
    if (ws_client == NULL) {
        esp_websocket_client_config_t websocket_cfg = {
            .uri = GEMINI_WS_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 16384, // 16KB: Khớp với chuẩn TLS record
            .task_stack = 16384, // 16KB: Thoải mái giải mã TLS và cJSON parse payload lớn
            .network_timeout_ms = 10000,
            .reconnect_timeout_ms = 0,
            .disable_auto_reconnect = true,
        };
        ws_client = esp_websocket_client_init(&websocket_cfg);
        esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)ws_client);
        esp_websocket_client_start(ws_client);
        is_new_connection = true;
    } else if (!esp_websocket_client_is_connected(ws_client)) {
        ESP_LOGW(TAG, "WebSocket disconnected. Forcing reconnect (destroy & re-init)...");
        esp_websocket_client_destroy(ws_client);
        
        esp_websocket_client_config_t websocket_cfg = {
            .uri = GEMINI_WS_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .buffer_size = 16384,
            .task_stack = 16384,
            .network_timeout_ms = 10000,
            .reconnect_timeout_ms = 0,
            .disable_auto_reconnect = true,
        };
        ws_client = esp_websocket_client_init(&websocket_cfg);
        esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)ws_client);
        esp_websocket_client_start(ws_client);
        is_new_connection = true;
    }
    esp_websocket_client_handle_t local_client = ws_client;
    if (ws_mutex) xSemaphoreGive(ws_mutex);
    
    // Đợi kết nối
    int retry = 0;
    while (local_client != NULL && !esp_websocket_client_is_connected(local_client) && retry < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        retry++;
    }
    
    if (ws_mutex) xSemaphoreTake(ws_mutex, portMAX_DELAY);
    if (ws_client == NULL || !esp_websocket_client_is_connected(ws_client)) {
        ESP_LOGE(TAG, "Failed to connect to Live API");
        if (ws_mutex) xSemaphoreGive(ws_mutex);
        return false;
    }

    if (is_new_connection) {
        // Gửi Setup Frame với systemInstruction và yêu cầu transcribe giọng nói của user
        const char *setup_msg = "{\"setup\":{\"model\":\"models/gemini-3.8-live\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"]},\"systemInstruction\":{\"parts\":[{\"text\":\"You are a helpful AI assistant. Always respond in the same language that the user speaks. If you hear silence, background noise, or unintelligible sounds, you MUST reply exactly 'Tôi chưa nghe rõ, vui lòng thử lại' in Vietnamese and nothing else. Default language is Vietnamese.\"}]},\"realtimeInputConfig\":{\"automaticActivityDetection\":{\"disabled\":true}},\"inputAudioTranscription\":{}}}";
        esp_websocket_client_send_text(ws_client, setup_msg, strlen(setup_msg), portMAX_DELAY);
        ESP_LOGI(TAG, "Sent Setup message (Multi-language + STT). Waiting for setupComplete...");
    }
    if (ws_mutex) xSemaphoreGive(ws_mutex);

    if (is_new_connection) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    s_current_state = AI_STATE_RECORDING;

    // Play Siri-like wake sound indicating readiness
    audio_hal_play_wake_sound();
    
    return true;
}

void ai_cloud_send_activity_start(void) {
    if (ws_mutex) xSemaphoreTake(ws_mutex, portMAX_DELAY);
    if (ws_client == NULL || !esp_websocket_client_is_connected(ws_client)) {
        if (ws_mutex) xSemaphoreGive(ws_mutex);
        return;
    }
    const char *start_msg = "{\"realtimeInput\":{\"activityStart\":{}}}";
    esp_websocket_client_send_text(ws_client, start_msg, strlen(start_msg), portMAX_DELAY);
    if (ws_mutex) xSemaphoreGive(ws_mutex);
    ESP_LOGI(TAG, "Sent activityStart");
}

void ai_cloud_send_audio_chunk(const uint8_t *data, size_t size) {
    if (s_current_state != AI_STATE_RECORDING) {
        return;
    }

    if (!s_tx_base64_buf || !s_tx_json_buf) {
        ESP_LOGE(TAG, "Static TX buffers not allocated!");
        return;
    }

    // Kiểm tra kích thước chunk
    size_t required_b64_len = ((size + 2) / 3) * 4 + 1;
    if (required_b64_len >= TX_BASE64_BUF_SIZE) {
        ESP_LOGE(TAG, "Audio chunk too large for static Base64 buffer: %u bytes", (unsigned)size);
        return;
    }

    size_t base64_len = 0;
    if (mbedtls_base64_encode(s_tx_base64_buf, TX_BASE64_BUF_SIZE - 1, &base64_len, data, size) != 0) {
        ESP_LOGE(TAG, "mbedtls_base64_encode failed");
        return;
    }
    s_tx_base64_buf[base64_len] = '\0';

    int json_len = snprintf(s_tx_json_buf, TX_JSON_BUF_SIZE,
             "{\"realtimeInput\":{\"audio\":{\"mimeType\":\"audio/pcm;rate=16000\",\"data\":\"%s\"}}}",
             (char *)s_tx_base64_buf);
    
    if (json_len < 0 || json_len >= TX_JSON_BUF_SIZE) {
        ESP_LOGE(TAG, "JSON message truncated!");
        return;
    }

    // Gửi qua WebSocket (Dùng timeout 2000ms để chịu đựng Wi-Fi jitter mà không rớt socket)
    if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (ws_client != NULL && esp_websocket_client_is_connected(ws_client)) {
            int sent = esp_websocket_client_send_text(ws_client, s_tx_json_buf, json_len, pdMS_TO_TICKS(2000));
            if (sent <= 0) {
                ESP_LOGW(TAG, "esp_websocket_client_send_text failed (ret=%d)", sent);
            }
        }
        xSemaphoreGive(ws_mutex);
    }
}

void ai_cloud_end_turn(void) {
    s_current_state = AI_STATE_WAITING_AI;
    expecting_first_chunk = true;

    // Chờ mutex tối đa 3000ms để chunk ghi âm dở dang (timeout 2000ms) kết thúc an toàn
    if (ws_mutex && xSemaphoreTake(ws_mutex, pdMS_TO_TICKS(3000)) == pdTRUE) {
        if (ws_client != NULL && esp_websocket_client_is_connected(ws_client)) {
            const char *activity_end_msg = "{\"realtimeInput\":{\"activityEnd\":{}}}";
            int sent = esp_websocket_client_send_text(ws_client, activity_end_msg, strlen(activity_end_msg), pdMS_TO_TICKS(1500));
            if (sent > 0) {
                ESP_LOGI(TAG, "Sent activityEnd (turn complete). Waiting for AI response...");
                // CHỈ BẬT TIMER 8S KHI ĐÃ GỬI THÀNH CÔNG CHO GOOGLE!
                if (s_session_timer) {
                    xTimerChangePeriod(s_session_timer, pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS), 0);
                    xTimerStart(s_session_timer, 0);
                }
            } else {
                ESP_LOGE(TAG, "Failed to send activityEnd! Aborting turn immediately.");
                xTaskCreate(abort_task, "abort_task", 4096, NULL, 5, NULL);
            }
        } else {
            ESP_LOGE(TAG, "WebSocket not connected while ending turn! Aborting turn immediately.");
            xTaskCreate(abort_task, "abort_task", 4096, NULL, 5, NULL);
        }
        xSemaphoreGive(ws_mutex);
    } else {
        ESP_LOGE(TAG, "Failed to take mutex within 3s while ending turn! Aborting turn immediately.");
        xTaskCreate(abort_task, "abort_task", 4096, NULL, 5, NULL);
    }
}

