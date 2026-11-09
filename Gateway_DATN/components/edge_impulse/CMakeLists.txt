cmake_minimum_required(VERSION 3.16)

file(GLOB_RECURSE WW_MODEL_SRCS "model_wakeword/tflite-model/*.cpp")
file(GLOB_RECURSE CMD_MODEL_SRCS "model_command/tflite-model/*.cpp")
file(GLOB_RECURSE EI_PORTING_SRCS "edge-impulse-sdk/porting/espressif/*.cpp")
file(GLOB_RECURSE EI_DSP_SRCS "edge-impulse-sdk/dsp/*.cpp")
file(GLOB_RECURSE EI_CLASSIFIER_SRCS "edge-impulse-sdk/classifier/*.cpp")

file(GLOB_RECURSE TFLITE_MICRO_SRCS 
    "edge-impulse-sdk/tensorflow/lite/c/*.c"
    "edge-impulse-sdk/tensorflow/lite/core/api/*.cc"
    "edge-impulse-sdk/tensorflow/lite/kernels/*.cc"
    "edge-impulse-sdk/tensorflow/lite/kernels/internal/*.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/*.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/kernels/*.cc"
    "edge-impulse-sdk/tensorflow/lite/micro/memory_planner/*.cc"
    "edge-impulse-sdk/tensorflow/lite/schema/*.cc"
)

file(GLOB_RECURSE CMSIS_DSP_SRCS "edge-impulse-sdk/CMSIS/DSP/Source/*/*.c")

set(ALL_SRCS
    "ei_wakeword.cpp"
    "ei_command.cpp"
    ${WW_MODEL_SRCS}
    ${CMD_MODEL_SRCS}
    ${EI_PORTING_SRCS}
    ${EI_DSP_SRCS}
    ${EI_CLASSIFIER_SRCS}
    ${TFLITE_MICRO_SRCS}
    ${CMSIS_DSP_SRCS}
)

idf_component_register(
    SRCS ${ALL_SRCS}
    INCLUDE_DIRS 
        "include"
        "."
        "model_wakeword"
        "model_wakeword/model-parameters"
        "model_wakeword/tflite-model"
        "model_command"
        "model_command/model-parameters"
        "model_command/tflite-model"
        "edge-impulse-sdk"
        "edge-impulse-sdk/classifier"
        "edge-impulse-sdk/porting"
        "edge-impulse-sdk/porting/espressif"
        "edge-impulse-sdk/dsp"
        "edge-impulse-sdk/dsp/kissfft"
        "edge-impulse-sdk/third_party/flatbuffers/include"
        "edge-impulse-sdk/third_party/gemmlowp"
        "edge-impulse-sdk/third_party/ruy"
        "edge-impulse-sdk/CMSIS/Core/Include"
        "edge-impulse-sdk/CMSIS/DSP/Include"
        "edge-impulse-sdk/porting/espressif/esp-dsp/modules/common/include"
        "edge-impulse-sdk/porting/espressif/esp-dsp/modules/fft/include"
    REQUIRES 
        esp_timer
        log
)

target_compile_options(${COMPONENT_LIB} PRIVATE 
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17 -fno-rtti>
    -O3
    -w
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC 
    "-DEI_PORTING_ESPRESSIF=1" 
    "-DEIDSP_USE_ESP_DSP=0"
    "-DEI_IMPULSE_RESULT_CLASSIFICATION_IS_STATICALLY_ALLOCATED=0"
)
