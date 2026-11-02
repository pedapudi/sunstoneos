# Build the complete private fork as one configuration and source unit.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/patch.cmake")
option(SUNSTONE_PARTITIONED_SCHEDULER "Select the private owner scheduler in SMP images" OFF)

function(sunstone_scheduler_partitioned_prepare output smp_tasks)
  get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  file(MAKE_DIRECTORY "${output}/include")
  # Copy the full header set: quoted includes in task.h must resolve the same
  # private FreeRTOS.h used by external StaticTask_t consumers.
  file(GLOB headers "${FREERTOS_DIR}/include/*.h")
  foreach(header IN LISTS headers)
    get_filename_component(name "${header}" NAME)
    configure_file("${header}" "${output}/include/${name}" COPYONLY)
  endforeach()
  apply_build_patch("${output}/include/FreeRTOS.h"
    "${FREERTOS_DIR}/include/FreeRTOS.h"
    "${root}/patches/freertos-partitioned-layout.patch")
  apply_build_patch("${output}/tasks-layout.c" "${smp_tasks}"
    "${root}/patches/freertos-partitioned-tasks.patch")
  apply_build_patch("${output}/tasks.c" "${output}/tasks-layout.c"
    "${root}/patches/freertos-partitioned-api.patch")
  apply_build_patch("${output}/portASM-owner.S" "${output}/portASM.S"
    "${root}/patches/freertos-partitioned-port-asm.patch")
  configure_file("${output}/portASM-owner.S" "${output}/portASM.S" COPYONLY)
  foreach(name queue event_groups stream_buffer timers)
    configure_file("${FREERTOS_DIR}/${name}.c" "${output}/${name}.c" COPYONLY)
  endforeach()
  execute_process(COMMAND patch --fuzz=0 --no-backup-if-mismatch --reject-file=-
    -d "${output}" -p0 -i "${root}/patches/freertos-partitioned-objects.patch"
    RESULT_VARIABLE status OUTPUT_VARIABLE report ERROR_VARIABLE report)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "Cannot prepare private kernel objects: ${report}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${headers}
    "${root}/patches/freertos-partitioned-layout.patch"
    "${root}/patches/freertos-partitioned-tasks.patch"
    "${root}/patches/freertos-partitioned-api.patch"
    "${root}/patches/freertos-partitioned-objects.patch"
    "${root}/patches/freertos-partitioned-port-asm.patch")
  foreach(name queue event_groups stream_buffer timers)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FREERTOS_DIR}/${name}.c")
  endforeach()
endfunction()

function(sunstone_scheduler_partitioned_sources variable output)
  get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  set(sources "${${variable}}")
  foreach(name queue event_groups stream_buffer timers)
    list(TRANSFORM sources REPLACE "^${FREERTOS_DIR}/${name}\\.c$" "${output}/${name}.c")
  endforeach()
  foreach(name owner notification deadline freertos_registry freertos_control)
    list(APPEND sources "${root}/kernel/scheduler/${name}.c")
  endforeach()
  list(REMOVE_DUPLICATES sources)
  set(${variable} "${sources}" PARENT_SCOPE)
endfunction()

# Apply this to a complete SMP image. Its application settings must publish
# these exact include directories and definitions to separately built objects.
function(sunstone_scheduler_partitioned_bind target output)
  get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  get_target_property(sources ${target} SOURCES)
  sunstone_scheduler_partitioned_sources(sources "${output}")
  set_property(TARGET ${target} PROPERTY SOURCES "${sources}")
  set_source_files_properties("${output}/tasks.c" "${output}/queue.c"
    "${output}/event_groups.c" "${output}/stream_buffer.c" "${output}/timers.c"
    PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
  foreach(name owner notification deadline freertos_registry freertos_control)
    set_source_files_properties("${root}/kernel/scheduler/${name}.c"
      PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
  endforeach()
  target_include_directories(${target} BEFORE PRIVATE "${output}/include" "${root}/kernel/scheduler")
  target_compile_definitions(${target} PRIVATE configSS_PARTITIONED_SCHEDULER=1)
  target_compile_options(${target} PRIVATE -march=armv8.1-a+lse)
endfunction()
