#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void ai_edge_init(void);
void ai_edge_stop_recording(void);
bool ai_edge_is_recording(void);

#ifdef __cplusplus
}
#endif
