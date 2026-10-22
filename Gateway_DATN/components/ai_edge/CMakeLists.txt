idf_component_register(SRCS "ai_edge.c" "wake_word_engine.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES audio_hal ai_cloud mesh_controller sys_core edge_impulse)

target_compile_options(${COMPONENT_LIB} PRIVATE 
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fno-rtti>
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE "-DEI_PORTING_ESPRESSIF=1" "-DEIDSP_USE_ESP_DSP=0")
