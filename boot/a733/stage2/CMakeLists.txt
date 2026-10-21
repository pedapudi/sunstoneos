cmake_minimum_required(VERSION 3.24)
project(a733_stage2 LANGUAGES C ASM)
get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-standalone-boot.cmake")
