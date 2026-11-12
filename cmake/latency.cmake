include("${CMAKE_CURRENT_LIST_DIR}/kernel-flags.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/latency-instrumentation.cmake")
# Include after smp.cmake. Only this diagnostic enables latency recording.
set(latency_root "${SUNSTONE_SOURCE_ROOT}")
if(NOT latency_root)
  set(latency_root "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
set(latency_build "${CMAKE_CURRENT_BINARY_DIR}/latency")
file(MAKE_DIRECTORY "${latency_build}")
apply_build_patch("${latency_build}/port.c" "${smp_build}/port.c"
  "${latency_root}/patches/freertos-smp-privileged.patch")
apply_build_patch("${latency_build}/tasks.c" "${FREERTOS_DIR}/tasks.c"
  "${latency_root}/patches/freertos-smp-tasks.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${latency_root}/patches/freertos-smp-privileged.patch"
  "${latency_root}/patches/freertos-smp-tasks.patch")
set_source_files_properties("${latency_build}/port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=dma_service_task_privileged")
add_executable(latency-model EXCLUDE_FROM_ALL
  "${latency_root}/platform/qemu-smp/start.S"
  "${latency_root}/platform/qemu-smp/platform.c"
  "${latency_root}/apps/latency/model.c"
  "${latency_root}/apps/latency/workload.c"
  "${latency_root}/apps/latency/dma-model.c"
  "${latency_root}/runtime/latency.c" "${latency_root}/runtime/latency_irq.c"
  "${latency_root}/runtime/memory.c" "${latency_root}/runtime/memmove.c"
  "${latency_root}/drivers/dma/a733_dma.c"
  "${latency_root}/drivers/dma/a733_dma_controller.c"
  "${latency_root}/drivers/dma/dma_service.c"
  "${latency_root}/drivers/dma/dma_service_i2s.c"
  "${latency_root}/drivers/dma/dma_service_i2s_stream.c"
  "${latency_root}/drivers/audio/a733_i2s.c"
  "${latency_root}/drivers/audio/a733_i2s_stream.c"
  "${latency_build}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${latency_build}/port.c" "${smp_build}/portASM.S")
target_include_directories(latency-model PRIVATE
  "${latency_root}/platform/qemu-smp" "${latency_root}/include"
  "${latency_root}/runtime/include" "${latency_root}/apps/latency"
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(latency-model PRIVATE SUNSTONE_LATENCY=1 SUNSTONE_LATENCY_MODEL=1
  SUNSTONE_SMP_CORES=8 configIDLE_AFFINITY=1
  INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1)
target_compile_options(latency-model PRIVATE ${SUNSTONE_KERNEL_FLAGS} -Werror
  "$<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>")
target_link_options(latency-model PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--defsym=SUNSTONE_SMP_STACKS=8 -Wl,--wrap=vApplicationIRQHandler
  "-Wl,-T,${latency_root}/platform/qemu-smp/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/latency-model.map")
set_target_properties(latency-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${latency_root}/platform/qemu-smp/link.ld")
sunstone_enable_latency_port(latency-model)

# The same recorder workload on the private owner scheduler. The host model's
# images report masked spans as atomic-operation counts (checks/latency-ops.h).
function(sunstone_latency_partitioned_model target metric)
  add_executable(${target} EXCLUDE_FROM_ALL
    "${latency_root}/platform/qemu-smp/start.S"
    "${latency_root}/platform/qemu-smp/platform.c"
    "${latency_root}/apps/latency/model.c"
    "${latency_root}/apps/latency/workload.c"
    "${latency_root}/runtime/latency.c" "${latency_root}/runtime/latency_irq.c"
    "${latency_root}/runtime/memory.c" "${latency_root}/runtime/memmove.c"
    "${latency_root}/drivers/dma/a733_dma.c"
    "${latency_root}/drivers/dma/a733_dma_controller.c"
    "${latency_root}/drivers/dma/dma_service.c"
    "${latency_root}/drivers/dma/dma_service_i2s.c"
    "${latency_root}/drivers/dma/dma_service_i2s_stream.c"
    "${latency_root}/drivers/audio/a733_i2s.c"
    "${latency_root}/drivers/audio/a733_i2s_stream.c"
    "${smp_build}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${latency_build}/port.c" "${smp_build}/portASM.S")
  target_include_directories(${target} PRIVATE
    "${latency_root}/checks/scheduler-partitioned-qemu-config"
    "${latency_root}/platform/qemu-smp" "${latency_root}/include"
    "${latency_root}/runtime/include" "${latency_root}/apps/latency"
    "${FREERTOS_DIR}/include" "${smp_build}")
  target_compile_definitions(${target} PRIVATE SUNSTONE_LATENCY=1 SUNSTONE_LATENCY_MODEL=1 SUNSTONE_LATENCY_CPU_ONLY=1
    SUNSTONE_SMP_CORES=8 configIDLE_AFFINITY=1
    INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1)
  if(metric)
    target_compile_definitions(${target} PRIVATE SS_LATENCY_OPS=${metric})
    target_compile_options(${target} PRIVATE
      "$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${latency_root}/checks/latency-ops.h>")
  else()
    # Real counter time with the PMU capture of the board image (QEMU counts instructions only).
    target_compile_definitions(${target} PRIVATE SUNSTONE_LATENCY_PMU=1)
  endif()
  target_compile_options(${target} PRIVATE ${SUNSTONE_KERNEL_FLAGS}
    "$<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>")
  target_link_options(${target} PRIVATE -nostdlib -static -Wl,--gc-sections
    -Wl,--defsym=SUNSTONE_SMP_STACKS=8 -Wl,--wrap=vApplicationIRQHandler
    "-Wl,-T,${latency_root}/platform/qemu-smp/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
  set_target_properties(${target} PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${latency_root}/platform/qemu-smp/link.ld")
  sunstone_scheduler_partitioned_bind(${target} "${smp_build}")
  if(NOT metric)
    sunstone_enable_scheduler_latency_trace(${target})
  endif()
  sunstone_enable_latency_port(${target})
endfunction()
# Masked spans reported as scheduler atomic stores and read-modify-writes, and as loads.
if(SUNSTONE_PARTITIONED_SCHEDULER)
  sunstone_latency_partitioned_model(latency-partitioned-writes-model 1)
  sunstone_latency_partitioned_model(latency-partitioned-loads-model 2)
  sunstone_latency_partitioned_model(latency-partitioned-pmu-model 0)
endif()
