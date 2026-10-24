cmake_minimum_required(VERSION 3.24)
project(a733_cpu_runtime_qualification LANGUAGES C ASM)
get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(FREERTOS_DIR "" CACHE PATH "Pinned FreeRTOS source")
function(apply_build_patch output source patch_file)
  execute_process(COMMAND patch --silent --fuzz=0 --output "${output}.tmp" "${source}" "${patch_file}"
    RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "Could not apply ${patch_file}")
  endif()
  configure_file("${output}.tmp" "${output}" COPYONLY)
  file(REMOVE "${output}.tmp")
endfunction()
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-smp-cached.cmake")
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
set(app "${CMAKE_CURRENT_SOURCE_DIR}")
set(generated "${CMAKE_CURRENT_BINARY_DIR}/qualification")
file(MAKE_DIRECTORY "${generated}")
get_target_property(sources a733-smp-cached-diagnostic SOURCES)
list(FILTER sources EXCLUDE REGEX "/build-id\\.c$")
set(inputs)
foreach(pair "boot.c|boot.patch" "continue.c|continue.patch" "continue.S|vectors.patch" "diagnostic.c|diagnostic.patch" "platform.c|platform.patch")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 unit)
  list(GET parts 1 patch)
  set(original "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/${unit}")
  apply_build_patch("${generated}/${unit}" "${original}" "${app}/${patch}")
  list(REMOVE_ITEM sources "${original}")
  list(APPEND sources "${generated}/${unit}")
  list(APPEND inputs "${original}" "${app}/${patch}")
endforeach()
list(REMOVE_ITEM sources "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/admission.c")
list(APPEND sources "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-supervised/admission.c"
  "${SUNSTONE_SOURCE_ROOT}/platform/a733/fault.c" "${SUNSTONE_SOURCE_ROOT}/platform/a733/fault.S"
  "${app}/runtime.c" "${app}/terminal.S" "${app}/timer.c")
add_executable(a733-cpu-runtime-qualification ${sources})
foreach(property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_OPTIONS)
  get_target_property(value a733-smp-cached-diagnostic ${property})
  set_property(TARGET a733-cpu-runtime-qualification PROPERTY ${property} "${value}")
endforeach()
target_include_directories(a733-cpu-runtime-qualification BEFORE PRIVATE "${app}")
target_compile_definitions(a733-cpu-runtime-qualification PRIVATE SUNSTONE_A733_FAULT_PAGE=1
  configINCLUDE_FREERTOS_TASK_C_ADDITIONS_H=1 A733_SMP_CONTRACT_MAGIC=0x4153373353554333ULL
  A733_SMP_RESULT_MAGIC=0x4153373353555231ULL)
get_target_property(links a733-cpu-runtime-qualification LINK_OPTIONS)
list(TRANSFORM links REPLACE "^-Wl,-Map,.*$" "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/qualification.map")
set_target_properties(a733-cpu-runtime-qualification PROPERTIES LINK_OPTIONS "${links}" SUFFIX ".elf"
  LINK_DEPENDS "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link.ld")
file(GLOB headers "${SUNSTONE_SOURCE_ROOT}/include/*.h" "${FREERTOS_DIR}/include/*.h"
  "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/*.h")
sunstone_build_identity(a733-cpu-runtime-qualification TAG "current-finite-cpu-local-timer-v2"
  BUILD_DIR "${generated}" PROFILE "current-finite-cpu-local-timer-v2" WORKLOAD "32-rounds-per-core-plus-local-timers"
  INPUTS ${sources} ${inputs} ${headers} "${app}/runtime.h" "${app}/freertos_tasks_c_additions.h"
    "${app}/CMakeLists.txt" "${SUNSTONE_SOURCE_ROOT}/cmake/a733-smp-cached.cmake"
    "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link.ld"
    "${CMAKE_CURRENT_BINARY_DIR}/freertos-a733-smp-cached/portmacro.h")
