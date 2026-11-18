idf_component_register(SRCS "ai_edge.c"
                       INCLUDE_DIRS "."
                       REQUIRES audio_hal ai_cloud mesh_controller sys_core edge_impulse ui_display)

target_compile_options(${COMPONENT_LIB} PRIVATE 
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fno-rtti>
)
