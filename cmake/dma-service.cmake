include("${CMAKE_CURRENT_LIST_DIR}/kernel-flags.cmake")
# Include after the SMP setup and npu.cmake; all generated files remain private.
apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/dma-service-port.c" "${smp_build}/port.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-privileged.patch")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/dma-service-port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=dma_service_task_privileged")
# Reuse the existing fixture-only EL observation instruction hook.
apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/dma-service-start.S"
  "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/start.S"
  "${CMAKE_CURRENT_SOURCE_DIR}/checks/npu-service-start.patch")
add_executable(dma-service-model EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_BINARY_DIR}/dma-service-start.S" platform/qemu-smp/platform.c
  arch/aarch64/device_memory.c runtime/memory.c runtime/memmove.c platform/a733/dma_native.c
  drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
  drivers/dma/dma_service.c drivers/dma/dma_service_i2s.c drivers/audio/a733_i2s.c
  checks/dma-service.c
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/dma-service-port.c" "${smp_build}/portASM.S")
target_include_directories(dma-service-model PRIVATE platform/qemu-smp include runtime/include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(dma-service-model PRIVATE
  INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1)
target_compile_options(dma-service-model PRIVATE
  ${SUNSTONE_KERNEL_FLAGS} -Werror -fstack-usage)
target_link_options(dma-service-model PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/dma-service-model.map")
set_target_properties(dma-service-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
