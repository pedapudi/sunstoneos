# Offline NPU page-table construction. This target performs no hardware access.
add_library(a733-npu-mmu OBJECT EXCLUDE_FROM_ALL drivers/npu/a733_npu_mmu.c)
target_include_directories(a733-npu-mmu PRIVATE include)
target_compile_options(a733-npu-mmu PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra -Werror)

add_library(a733-npu-controller OBJECT EXCLUDE_FROM_ALL drivers/npu/a733_npu_controller.c)
target_include_directories(a733-npu-controller PRIVATE include)
target_compile_options(a733-npu-controller PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra -Werror)

add_library(a733-npu-memory OBJECT EXCLUDE_FROM_ALL drivers/npu/a733_npu_memory.c)
target_include_directories(a733-npu-memory PRIVATE include)
target_compile_options(a733-npu-memory PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra -Werror)

# Native CPU operations under FreeRTOS. QEMU supplies no A733 NPU device.
add_executable(npu-native-model EXCLUDE_FROM_ALL
  arch/aarch64/start.S arch/aarch64/vectors.S arch/aarch64/device_memory.c
  platform/qemu-virt/platform.c runtime/memory.c runtime/counter_clock.c
  checks/npu-native.c platform/a733/npu_native.c
  drivers/npu/a733_npu_memory.c drivers/npu/a733_npu_mmu.c drivers/npu/a733_npu_controller.c
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c" "${PORT}/portASM.S")
target_include_directories(npu-native-model PRIVATE
  include runtime/include "${FREERTOS_DIR}/include" "${PORT}")
target_compile_definitions(npu-native-model PRIVATE GUEST=1)
target_compile_options(npu-native-model PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra -Werror)
target_link_options(npu-native-model PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-virt/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/npu-native-model.map")
set_target_properties(npu-native-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-virt/link.ld")

# Separate SMP port copy: only the driver worker starts at EL1t. Existing SMP
# targets retain their unmodified generated port and startup files.
apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/npu-service-port.c" "${smp_build}/port.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-privileged.patch")
apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/npu-service-start.S"
  "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/start.S"
  "${CMAKE_CURRENT_SOURCE_DIR}/checks/npu-service-start.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  patches/freertos-smp-privileged.patch checks/npu-service-start.patch platform/qemu-smp/start.S)
add_executable(npu-service-model EXCLUDE_FROM_ALL
  "${CMAKE_CURRENT_BINARY_DIR}/npu-service-start.S" platform/qemu-smp/platform.c
  arch/aarch64/device_memory.c runtime/memory.c runtime/counter_clock.c
  checks/npu-service.c checks/smp-context.S platform/a733/npu_native.c
  drivers/npu/npu_service.c drivers/npu/a733_npu_controller.c
  drivers/npu/a733_npu_memory.c drivers/npu/a733_npu_mmu.c
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/npu-service-port.c" "${smp_build}/portASM.S")
target_include_directories(npu-service-model PRIVATE platform/qemu-smp include runtime/include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(npu-service-model PRIVATE
  INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1)
target_compile_options(npu-service-model PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra -Werror)
target_link_options(npu-service-model PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/npu-service-model.map")
set_target_properties(npu-service-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
