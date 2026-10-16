# Native cache/translation execution in QEMU; no physical DMA controller access.
add_executable(dma-native-model EXCLUDE_FROM_ALL
  arch/aarch64/start.S arch/aarch64/vectors.S platform/qemu-virt/platform.c
  runtime/memory.c checks/dma-native.c drivers/dma/dma_memory.c platform/a733/dma_native.c
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c" "${PORT}/portASM.S")
target_include_directories(dma-native-model PRIVATE
  include runtime/include "${FREERTOS_DIR}/include" "${PORT}")
target_compile_definitions(dma-native-model PRIVATE GUEST=1)
target_compile_options(dma-native-model PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra -Werror)
target_link_options(dma-native-model PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-virt/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/dma-native-model.map")
set_target_properties(dma-native-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-virt/link.ld")
