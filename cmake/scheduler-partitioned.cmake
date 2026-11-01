# Generate the private fork after the existing SMP tasks patch. Callers put
# <output>/include before the upstream headers and use <output>/tasks.c.
# This function prepares sources; the caller admits the complete live feature.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/patch.cmake")

function(sunstone_scheduler_partitioned_prepare output smp_tasks)
  file(MAKE_DIRECTORY "${output}/include")
  apply_build_patch("${output}/include/FreeRTOS.h"
    "${FREERTOS_DIR}/include/FreeRTOS.h"
    "${PROJECT_SOURCE_DIR}/patches/freertos-partitioned-layout.patch")
  apply_build_patch("${output}/tasks.c" "${smp_tasks}"
    "${PROJECT_SOURCE_DIR}/patches/freertos-partitioned-tasks.patch")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${FREERTOS_DIR}/include/FreeRTOS.h" "${smp_tasks}"
    "${PROJECT_SOURCE_DIR}/patches/freertos-partitioned-layout.patch"
    "${PROJECT_SOURCE_DIR}/patches/freertos-partitioned-tasks.patch")
endfunction()
