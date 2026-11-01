# A returning single-core audio diagnostic; include through the existing CMake
# top-level hook. Shared startup, monitor and probe ABI remain the source.
function(sunstone_i2s_probe)
  get_target_property(sources a733-kernel-check SOURCES)
  list(REMOVE_ITEM sources checks/kernel.c checks/fp-context.S platform/a733/monitor.c platform/a733/platform.c)
  set(out "${CMAKE_CURRENT_BINARY_DIR}/i2s-probe")
  file(MAKE_DIRECTORY "${out}")
  configure_file("${SUNSTONE_SOURCE_ROOT}/platform/a733/monitor.c" "${out}/monitor.c" COPYONLY)
  configure_file("${SUNSTONE_SOURCE_ROOT}/platform/a733/platform.c" "${out}/platform.c" COPYONLY)
  set_source_files_properties("${out}/monitor.c" PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only"
    COMPILE_DEFINITIONS "monitor_init=i2s_probe_monitor_init;sunstone_probe_restore_interrupts=i2s_probe_monitor_restore")
  set_source_files_properties("${out}/platform.c" PROPERTIES
    COMPILE_DEFINITIONS "vApplicationFPUSafeIRQHandler=i2s_probe_platform_irq")
  add_executable(sunstone-a733-i2s-probe EXCLUDE_FROM_ALL ${sources}
    "${out}/monitor.c" "${out}/platform.c"
    apps/i2s/probe.c apps/i2s/probe-platform.c apps/i2s/probe-cpu.c apps/i2s/retire.c runtime/memmove.c arch/aarch64/device_memory.c
    drivers/audio/a733_i2s.c drivers/audio/a733_i2s_stream.c drivers/audio/a733_i2s_platform.c
    drivers/audio/a733_i2s_platform_native.c drivers/audio/a733_audio_leases.c drivers/audio/dma_service_i2s_irq.c
    drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c drivers/dma/dma_memory.c
    drivers/dma/dma_service.c drivers/dma/dma_service_i2s.c drivers/dma/dma_service_i2s_stream.c
    platform/a733/dma_native.c platform/a733/shared_locks.c
    drivers/clock/a733_clock_leases.c drivers/pinctrl/a733_pins.c drivers/pinctrl/a733_pin_tables.c drivers/gpu/gicv3_spi.c)
  foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS LINK_OPTIONS LINK_DEPENDS)
    get_target_property(value a733-kernel-check ${property})
    if(NOT value STREQUAL "value-NOTFOUND")
      set_property(TARGET sunstone-a733-i2s-probe PROPERTY ${property} "${value}")
    endif()
  endforeach()
  target_compile_definitions(sunstone-a733-i2s-probe PRIVATE
    INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
  target_include_directories(sunstone-a733-i2s-probe AFTER PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/i2s")
  set_target_properties(sunstone-a733-i2s-probe PROPERTIES SUFFIX ".elf")
  set_source_files_properties(apps/i2s/probe-cpu.c PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
  include("${SUNSTONE_SOURCE_ROOT}/cmake/sunstone-probe.cmake")
  sunstone_enable_probe(sunstone-a733-i2s-probe)
  # Shared-library fixes remain reviewable patches until their owner integrates
  # them. Compile the patched copy without changing the shared source file.
  if(SUNSTONE_I2S_PROBE_ENTRY_PATCH)
    execute_process(COMMAND patch --fuzz=0 --reject-file=- --output "${out}/probe.c"
      "${SUNSTONE_SOURCE_ROOT}/platform/a733/probe.c" "${SUNSTONE_I2S_PROBE_ENTRY_PATCH}"
      RESULT_VARIABLE patch_status OUTPUT_VARIABLE patch_report ERROR_VARIABLE patch_error)
    if(NOT patch_status EQUAL 0)
      message(FATAL_ERROR "I2S entry patch failed: ${patch_report}${patch_error}")
    endif()
    # Exercise the default hooks in the existing memtest recovery closure too.
    foreach(probe_target sunstone-a733-i2s-probe a733-memtest)
      get_target_property(probe_sources ${probe_target} SOURCES)
      list(REMOVE_ITEM probe_sources "${SUNSTONE_SOURCE_ROOT}/platform/a733/probe.c")
      set_property(TARGET ${probe_target} PROPERTY SOURCES "${probe_sources}")
      target_sources(${probe_target} PRIVATE "${out}/probe.c")
    endforeach()
    set_source_files_properties("${out}/probe.c" PROPERTIES COMPILE_OPTIONS "-Os;-mgeneral-regs-only")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${SUNSTONE_SOURCE_ROOT}/platform/a733/probe.c" "${SUNSTONE_I2S_PROBE_ENTRY_PATCH}")
  endif()
  if(SUNSTONE_I2S_PROBE_ROM_PATCH)
    execute_process(COMMAND patch --fuzz=0 --reject-file=- --output "${out}/probe.S"
      "${SUNSTONE_SOURCE_ROOT}/platform/a733/probe.S" "${SUNSTONE_I2S_PROBE_ROM_PATCH}"
      RESULT_VARIABLE patch_status OUTPUT_VARIABLE patch_report ERROR_VARIABLE patch_error)
    if(NOT patch_status EQUAL 0)
      message(FATAL_ERROR "I2S ROM tail patch failed: ${patch_report}${patch_error}")
    endif()
    # Exercise the default hooks in the existing memtest recovery closure too.
    foreach(probe_target sunstone-a733-i2s-probe a733-memtest)
      get_target_property(probe_sources ${probe_target} SOURCES)
      list(REMOVE_ITEM probe_sources "${SUNSTONE_SOURCE_ROOT}/platform/a733/probe.S")
      set_property(TARGET ${probe_target} PROPERTY SOURCES "${probe_sources}")
      target_sources(${probe_target} PRIVATE "${out}/probe.S")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${SUNSTONE_SOURCE_ROOT}/platform/a733/probe.S" "${SUNSTONE_I2S_PROBE_ROM_PATCH}")
  endif()
  foreach(pair "i2s-cyclic-model|i2s-retire-model|I2S_RETIRE_CHECK" "i2s-file-model|i2s-file-retire-model|I2S_FILE_RETIRE_CHECK")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 original)
    list(GET parts 1 name)
    list(GET parts 2 flag)
    get_target_property(model_sources ${original} SOURCES)
    add_executable(${name} EXCLUDE_FROM_ALL ${model_sources} apps/i2s/retire.c)
    foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS LINK_OPTIONS LINK_DEPENDS)
      get_target_property(value ${original} ${property})
      if(NOT value STREQUAL "value-NOTFOUND")
        set_property(TARGET ${name} PROPERTY ${property} "${value}")
      endif()
    endforeach()
    target_compile_definitions(${name} PRIVATE ${flag}=1)
    set_target_properties(${name} PROPERTIES SUFFIX ".elf")
  endforeach()
endfunction()
cmake_language(DEFER CALL sunstone_i2s_probe)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_FILE}")
