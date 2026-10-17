# Include after dma-service.cmake and dma-startup.cmake. The service ABI adds
# stream dispatch even for finite-only callers, so close those existing links.
set(i2s_stream_runtime drivers/audio/a733_i2s_stream.c drivers/dma/dma_service_i2s_stream.c)
foreach(target dma-service-model dma-startup-model)
  if(TARGET ${target})
    target_sources(${target} PRIVATE ${i2s_stream_runtime})
  endif()
endforeach()
apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/i2s-stream-port.c" "${smp_build}/port.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-privileged.patch")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/i2s-stream-port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=i2s_stream_task_privileged")
add_executable(i2s-stream-model EXCLUDE_FROM_ALL
  platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c runtime/memmove.c
  arch/aarch64/device_memory.c platform/a733/dma_native.c platform/a733/shared_locks.c
  drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
  drivers/dma/dma_service.c drivers/dma/dma_service_i2s.c ${i2s_stream_runtime}
  drivers/audio/a733_i2s.c drivers/audio/a733_i2s_platform.c drivers/audio/a733_audio_leases.c
  drivers/clock/a733_clock_leases.c drivers/pinctrl/a733_pins.c drivers/pinctrl/a733_pin_tables.c
  checks/i2s-stream.c "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/i2s-stream-port.c" "${smp_build}/portASM.S")
target_include_directories(i2s-stream-model PRIVATE platform/qemu-smp include runtime/include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(i2s-stream-model PRIVATE INCLUDE_xTaskGetSchedulerState=1
  INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
target_compile_options(i2s-stream-model PRIVATE -march=armv8-a -mstrict-align -ffreestanding
  -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra -Werror -fstack-usage)
target_link_options(i2s-stream-model PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/i2s-stream-model.map")
set_target_properties(i2s-stream-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
