cmake_minimum_required(VERSION 3.24)
project(sunstone-accelerators C ASM)
get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(A733_NPU_LARGE_GRAPH ON CACHE BOOL "" FORCE)
set(A733_NPU_NN_POOL_GRAPH ON CACHE BOOL "" FORCE)
set(A733_NPU_RUNTIME_INPUT ON CACHE BOOL "" FORCE)
set(A733_NPU_INTERRUPT ON CACHE BOOL "" FORCE)
set(A733_NPU_SUPERVISED ON CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../npu-convolution" npu)
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-npu-single-core.cmake")

# Root owns the executable, architecture sources, admission and selected layout.
# Add only the retained application composition to that existing target.
function(sunstone_accelerator_application target)
  target_sources(${target} PRIVATE
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/main.c"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/gpu.c"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/usb.c"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../npu-runtime/main.c"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../npu-runtime-large/runtime.c")
  target_include_directories(${target} BEFORE PRIVATE
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../npu-runtime-large")
  target_compile_definitions(${target} PRIVATE SUNSTONE_ACCELERATORS=1)
endfunction()

include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-accelerators.cmake")
