include("${CMAKE_CURRENT_LIST_DIR}/kernel-flags.cmake")
# Native object has no fixture definitions. Root includes this fragment after npu.cmake.
add_library(npu-convolution OBJECT EXCLUDE_FROM_ALL drivers/npu/npu_convolution.c)
target_include_directories(npu-convolution PRIVATE platform/qemu-smp include runtime/include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(npu-convolution PRIVATE INCLUDE_xTaskGetSchedulerState=1
  INCLUDE_xTaskGetCurrentTaskHandle=1)
target_compile_options(npu-convolution PRIVATE -march=armv8-a -mstrict-align
  -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra -Werror)

set(NPU_CONVOLUTION_FIXTURES "" CACHE PATH "External authenticated convolution fixture headers")
if(NPU_CONVOLUTION_FIXTURES)
  apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/npu-convolution-port.c" "${smp_build}/port.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-privileged.patch")
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/npu-convolution-port.c" PROPERTIES
    COMPILE_DEFINITIONS "npu_service_task_privileged=npu_convolution_task_privileged")
  add_executable(npu-convolution-model EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_BINARY_DIR}/npu-service-start.S" platform/qemu-smp/platform.c
    arch/aarch64/device_memory.c runtime/memory.c runtime/memmove.c runtime/counter_clock.c
    checks/npu-convolution.c platform/a733/npu_native.c
    drivers/npu/a733_npu_startup.c drivers/clock/a733_clock_leases.c drivers/npu/a733_npu_platform.c platform/a733/shared_locks.c
    drivers/npu/npu_service.c drivers/npu/a733_npu_controller.c
    drivers/power/a733_power_domains.c drivers/power/a733_pmic.c
    drivers/power/a733_rail_leases.c drivers/pinctrl/a733_pins.c drivers/pinctrl/a733_pin_tables.c
    drivers/gpu/a733_gpu_resources.c $<TARGET_OBJECTS:npu-startup-native-io>
    platform/a733/counter_clock.c
    drivers/npu/a733_npu_platform_native.c
    drivers/npu/a733_npu_memory.c drivers/npu/a733_npu_mmu.c
    drivers/npu/nbg_model.c drivers/npu/nbg_assembly.c drivers/npu/nbg_checksum.c drivers/npu/nbg_relocation.c
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${CMAKE_CURRENT_BINARY_DIR}/npu-convolution-port.c" "${smp_build}/portASM.S")
  target_include_directories(npu-convolution-model PRIVATE platform/qemu-smp include runtime/include
    "${NPU_CONVOLUTION_FIXTURES}" "${FREERTOS_DIR}/include" "${smp_build}")
  target_compile_definitions(npu-convolution-model PRIVATE INCLUDE_xTaskGetSchedulerState=1
    INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
  target_compile_options(npu-convolution-model PRIVATE ${SUNSTONE_KERNEL_FLAGS} -Werror -fstack-usage)
  target_link_options(npu-convolution-model PRIVATE -nostdlib -static -Wl,--gc-sections
    -Wl,--wrap=a733_npu_native_operations -Wl,--wrap=a733_npu_startup_prepared
    -Wl,--wrap=aarch64_device_translate
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/npu-convolution-model.map")
  set_target_properties(npu-convolution-model PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
endif()
