cmake_minimum_required(VERSION 3.24)
project(a733_rtc_single_core LANGUAGES C ASM)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../rtc" "${CMAKE_CURRENT_BINARY_DIR}/rtc-base")
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/a733-rtc-single-core.cmake")
