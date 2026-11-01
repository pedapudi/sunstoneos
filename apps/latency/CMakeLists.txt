cmake_minimum_required(VERSION 3.24)
project(sunstone_latency LANGUAGES C ASM)
get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(FREERTOS_DIR "" CACHE PATH "Pinned FreeRTOS kernel source")
include("${SUNSTONE_SOURCE_ROOT}/cmake/patch.cmake")
# Reuse the same generated port inputs as the root SMP targets.
set(smp_port "${FREERTOS_DIR}/portable/GCC/ARM_CR82")
set(smp_build "${CMAKE_CURRENT_BINARY_DIR}/freertos-smp")
file(MAKE_DIRECTORY "${smp_build}")
foreach(pair "port.c|c" "portASM.S|asm" "portmacro.h|macro")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 unit)
  list(GET parts 1 patch_name)
  apply_build_patch("${smp_build}/${unit}" "${smp_port}/${unit}"
    "${SUNSTONE_SOURCE_ROOT}/patches/freertos-smp-${patch_name}.patch")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${smp_port}/${unit}" "${SUNSTONE_SOURCE_ROOT}/patches/freertos-smp-${patch_name}.patch")
endforeach()
include("${SUNSTONE_SOURCE_ROOT}/cmake/latency.cmake")
