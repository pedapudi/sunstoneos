# Include after gpu.cmake. Reuse its pinned firmware and privileged SMP port.
if(SUNSTONE_GPU_FIRMWARE)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/platform.c" gpu_irq_platform)
  string(REPLACE "vApplicationIRQHandler" "gpu_irq_base_handler" gpu_irq_platform "${gpu_irq_platform}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gpu-irq-platform.c.tmp" "${gpu_irq_platform}")
  configure_file("${CMAKE_CURRENT_BINARY_DIR}/gpu-irq-platform.c.tmp"
    "${CMAKE_CURRENT_BINARY_DIR}/gpu-irq-platform.c" COPYONLY)
  file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/gpu-irq-platform.c.tmp")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS platform/qemu-smp/platform.c)
  add_executable(gpu-irq-model EXCLUDE_FROM_ALL ${gpu_compute_sources}
    "${CMAKE_CURRENT_BINARY_DIR}/npu-service-start.S" "${CMAKE_CURRENT_BINARY_DIR}/gpu-irq-platform.c"
    runtime/memory.c runtime/memmove.c checks/pvr-gpu-irq.c "${gpu_image}"
    drivers/gpu/gicv3_spi.c drivers/gpu/pvr_gpu_service.c drivers/gpu/pvr_firmware.c
    drivers/gpu/pvr_mips.c drivers/gpu/pvr_fw_state.c drivers/gpu/pvr_ccb.c
    drivers/gpu/pvr_gpu_start.c drivers/gpu/pvr_gpu_control.c drivers/gpu/pvr_gpu_stop.c
    drivers/gpu/pvr_free_list.c drivers/gpu/pvr_gpu_events.c
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${CMAKE_CURRENT_BINARY_DIR}/gpu-service-port.c" "${smp_build}/portASM.S")
  target_include_directories(gpu-irq-model PRIVATE platform/qemu-smp include runtime/include
    "${FREERTOS_DIR}/include" "${smp_build}")
  target_compile_definitions(gpu-irq-model PRIVATE
    INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1)
  target_compile_options(gpu-irq-model PRIVATE
    -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
    -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
    -O2 -g -Wall -Wextra -Werror -fstack-usage)
  target_link_options(gpu-irq-model PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/gpu-irq-model.map")
  set_target_properties(gpu-irq-model PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
endif()
