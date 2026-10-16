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
