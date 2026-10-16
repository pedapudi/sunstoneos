# Firmware is an external, pinned build input, never a tracked generated array.
set(SUNSTONE_GPU_FIRMWARE "" CACHE FILEPATH "Pinned A733 GPU firmware for the service emulator check")
if(SUNSTONE_GPU_FIRMWARE)
  file(SHA256 "${SUNSTONE_GPU_FIRMWARE}" gpu_firmware_hash)
  if(NOT gpu_firmware_hash STREQUAL "1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2")
    message(FATAL_ERROR "GPU service fixture requires the pinned A733 firmware")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SUNSTONE_GPU_FIRMWARE}")
  set(gpu_image "${CMAKE_CURRENT_BINARY_DIR}/gpu-service-firmware.S")
  file(WRITE "${gpu_image}.tmp" ".section .rodata.gpu_firmware,\"a\"\n.balign 16\n.global gpu_firmware\ngpu_firmware:\n.incbin \"${SUNSTONE_GPU_FIRMWARE}\"\n.global gpu_firmware_end\ngpu_firmware_end:\n")
  configure_file("${gpu_image}.tmp" "${gpu_image}" COPYONLY)
  file(REMOVE "${gpu_image}.tmp")
  set_source_files_properties("${gpu_image}" PROPERTIES OBJECT_DEPENDS "${SUNSTONE_GPU_FIRMWARE}")
  apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/gpu-service-port.c" "${smp_build}/port.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-privileged.patch")
  # Reuse the checked privilege extension with this service's private hook.
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/gpu-service-port.c" PROPERTIES
    COMPILE_DEFINITIONS "npu_service_task_privileged=pvr_gpu_service_task_privileged")
  add_executable(gpu-service-model EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_BINARY_DIR}/npu-service-start.S" platform/qemu-smp/platform.c
    runtime/memory.c runtime/memmove.c checks/pvr-gpu-service.c "${gpu_image}"
    drivers/gpu/pvr_gpu_service.c drivers/gpu/pvr_firmware.c drivers/gpu/pvr_mips.c
    drivers/gpu/pvr_fw_state.c drivers/gpu/pvr_ccb.c drivers/gpu/pvr_gpu_start.c
    drivers/gpu/pvr_gpu_control.c drivers/gpu/pvr_gpu_stop.c
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${CMAKE_CURRENT_BINARY_DIR}/gpu-service-port.c" "${smp_build}/portASM.S")
  target_include_directories(gpu-service-model PRIVATE platform/qemu-smp include runtime/include
    "${FREERTOS_DIR}/include" "${smp_build}")
  target_compile_definitions(gpu-service-model PRIVATE
    INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1)
  target_compile_options(gpu-service-model PRIVATE
    -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
    -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
    -O2 -g -Wall -Wextra -Werror -fstack-usage)
  target_link_options(gpu-service-model PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/gpu-service-model.map")
  set_target_properties(gpu-service-model PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
endif()
