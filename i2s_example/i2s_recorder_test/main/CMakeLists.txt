# ==============================================================================
# CẤU HÌNH BIÊN DỊCH CHO THƯ MỤC MAIN:
# - Chế độ Voice Gateway chính (Wake Word "Hey Cherry" + Gemini Live API): "main.c"
# - Chế độ thu âm Dataset: "dataset_recorder_main.c"
# ==============================================================================

set(APP_SOURCE "main.c")
# set(APP_SOURCE "dataset_recorder_main.c")

idf_component_register(SRCS "${APP_SOURCE}"
                       INCLUDE_DIRS "."
                       REQUIRES sys_core audio_hal mesh_controller ai_cloud ai_edge esp_http_server esp_netif iot_cloud ui_display i2s_examples_common)
