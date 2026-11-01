include("${CMAKE_CURRENT_LIST_DIR}/kernel-flags.cmake")
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
