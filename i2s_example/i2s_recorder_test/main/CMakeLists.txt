# ==============================================================================
# CẤU HÌNH BIÊN DỊCH CHO THƯ MỤC MAIN:
# - Chế độ thu âm Dataset: "dataset_recorder_main.c"
# - Chế độ Voice Gateway chính: "main.c"
# ==============================================================================

# Khi nào muốn chuyển về Voice Gateway chính, bạn chỉ cần đổi:
# set(APP_SOURCE "main.c")
set(APP_SOURCE "dataset_recorder_main.c")

idf_component_register(SRCS "${APP_SOURCE}"
                       INCLUDE_DIRS "."
                       REQUIRES esp_http_server esp_netif sys_core audio_hal)
