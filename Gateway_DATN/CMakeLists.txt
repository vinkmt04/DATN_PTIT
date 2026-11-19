# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(i2s_recorder_test)

target_link_libraries(${CMAKE_PROJECT_NAME}.elf PRIVATE "-Wl,--whole-archive" __idf_espressif__mdns "-Wl,--no-whole-archive")
