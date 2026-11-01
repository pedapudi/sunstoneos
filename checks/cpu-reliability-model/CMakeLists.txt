cmake_minimum_required(VERSION 3.24)
project(cpu_reliability_model LANGUAGES C ASM)
get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${SUNSTONE_SOURCE_ROOT}/cmake/patch.cmake")
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-smp-cached.cmake")
get_target_property(sources a733-smp-cached-diagnostic SOURCES)
list(REMOVE_ITEM sources "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/diagnostic.c")
set_property(TARGET a733-smp-cached-diagnostic PROPERTY SOURCES "${sources}")
target_sources(a733-smp-cached-diagnostic PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/application.c"
  "${SUNSTONE_SOURCE_ROOT}/apps/cpu-runtime-qualification/reliability.c")
target_compile_definitions(a733-smp-cached-diagnostic PRIVATE REL_ROUNDS=8)
