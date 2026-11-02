# Read-only DMA0 entry-state observation through the native probe contract.
include("${CMAKE_CURRENT_LIST_DIR}/sunstone-probe.cmake")
set(dma_authority_root "${SUNSTONE_SOURCE_ROOT}")
add_executable(dma-authority-control EXCLUDE_FROM_ALL
  "${dma_authority_root}/checks/dma-authority-control.S"
  "${dma_authority_root}/checks/dma-authority-control.c"
  "${dma_authority_root}/arch/aarch64/cache.S"
  "${dma_authority_root}/runtime/memory.c")
target_include_directories(dma-authority-control PRIVATE
  "${dma_authority_root}/checks" "${dma_authority_root}/platform/a733"
  "${dma_authority_root}/runtime/include")
target_compile_options(dma-authority-control PRIVATE -march=armv8-a
  -ffreestanding -fno-builtin -fno-stack-protector -mgeneral-regs-only
  -fno-unwind-tables -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra -Werror)
target_link_options(dma-authority-control PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${dma_authority_root}/checks/dma-authority-control.ld")
set_target_properties(dma-authority-control PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${dma_authority_root}/checks/dma-authority-control.ld")
sunstone_enable_probe(dma-authority-control)
