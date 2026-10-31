#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AI_EDGE_STATE_IDLE_WAKEWORD = 0, // 1. Lắng nghe 'Hey Cherry' ngầm 24/7 (lát cắt 250ms)
    AI_EDGE_STATE_COMMAND_LISTENING, // 2. Lắng nghe câu lệnh Model 2 (Sliding Window 400ms / Cửa sổ 1.2s, tối đa 4.0s)
    AI_EDGE_STATE_STREAMING_SPEECH,  // 3. Streaming âm thanh câu hỏi lên Google Gemini Live API
    AI_EDGE_STATE_AI_RESPONDING      // 4. Gemini đang xử lý & loa đang phát phản hồi TTS
} ai_edge_state_t;

void ai_edge_init(void);
void ai_edge_stop_recording(void);
bool ai_edge_is_recording(void);
ai_edge_state_t ai_edge_get_state(void);

#ifdef __cplusplus
}
#endif
