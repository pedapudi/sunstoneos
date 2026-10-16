# Include after SMP and npu setup. Each fixture uses private generated port code.
apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/dma-startup-port.c" "${smp_build}/port.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-privileged.patch")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/dma-startup-port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=dma_startup_task_privileged")
add_executable(dma-startup-model EXCLUDE_FROM_ALL
  platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c runtime/memmove.c
  platform/a733/counter_clock.c platform/a733/shared_locks.c platform/a733/dma_startup.c
  drivers/clock/a733_clock_leases.c drivers/dma/a733_dma_platform.c
  drivers/dma/a733_dma_platform_native.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
  drivers/dma/dma_service.c drivers/dma/dma_service_i2s.c drivers/audio/a733_i2s.c
  checks/dma-startup.c "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/dma-startup-port.c" "${smp_build}/portASM.S")
target_include_directories(dma-startup-model PRIVATE platform/qemu-smp include runtime/include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(dma-startup-model PRIVATE INCLUDE_xTaskGetSchedulerState=1
  INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
target_compile_options(dma-startup-model PRIVATE -march=armv8-a -mstrict-align -ffreestanding
  -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra -Werror -fstack-usage)
target_link_options(dma-startup-model PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--wrap=a733_dma_platform_native_read -Wl,--wrap=a733_dma_platform_native_write
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/dma-startup-model.map")
set_target_properties(dma-startup-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
