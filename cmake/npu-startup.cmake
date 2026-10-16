# Include after the SMP and NPU setup. Keep fixture privilege changes private.
apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/npu-startup-port.c" "${smp_build}/port.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-privileged.patch")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/npu-startup-port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=npu_startup_task_privileged")
add_library(npu-startup-native-io OBJECT EXCLUDE_FROM_ALL
  drivers/gpu/a733_gpu_resources_native.c drivers/i2c/a733_i2c.c)
target_include_directories(npu-startup-native-io PRIVATE platform/a733 include)
target_compile_options(npu-startup-native-io PRIVATE -march=armv8-a -mstrict-align
  -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra -Werror)
add_executable(npu-startup-model EXCLUDE_FROM_ALL
  platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c runtime/memmove.c
  platform/a733/counter_clock.c platform/a733/shared_locks.c drivers/npu/a733_npu_startup.c
  drivers/clock/a733_clock_leases.c drivers/gpu/a733_gpu_resources.c
  drivers/npu/a733_npu_platform.c drivers/npu/a733_npu_platform_native.c
  drivers/npu/npu_service.c drivers/npu/a733_npu_controller.c
  checks/npu-startup.c $<TARGET_OBJECTS:npu-startup-native-io>
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/npu-startup-port.c" "${smp_build}/portASM.S")
target_include_directories(npu-startup-model PRIVATE platform/qemu-smp include runtime/include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(npu-startup-model PRIVATE INCLUDE_xTaskGetSchedulerState=1
  INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
target_compile_options(npu-startup-model PRIVATE -march=armv8-a -mstrict-align -ffreestanding
  -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra -Werror -fstack-usage)
target_link_options(npu-startup-model PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--wrap=a733_npu_platform_native_read -Wl,--wrap=a733_npu_platform_native_write
  -Wl,--wrap=a733_gpu_resources_native_init -Wl,--wrap=a733_i2c_open
  -Wl,--wrap=a733_i2c_read8 -Wl,--wrap=a733_i2c_close
  -Wl,--wrap=a733_platform_lock_leave
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/npu-startup-model.map")
set_target_properties(npu-startup-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
