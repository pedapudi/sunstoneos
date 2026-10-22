# Returning memory qualification image: the returning platform (monitor,
# exception capture and result block) with apps/memtest linked by its own
# script, which places the monitor's recovery pages at fixed addresses.
# Include after the FreeRTOS port setup of CMakeLists.txt; the target uses
# PORT and the patched freertos-port.c from there.
find_program(SUNSTONE_OBJCOPY NAMES llvm-objcopy-21 llvm-objcopy
  HINTS /usr/lib/llvm-21/bin REQUIRED)
add_executable(a733-memtest
  platform/a733/monitor.S platform/a733/monitor.c platform/a733/platform.c
  arch/aarch64/cache.S arch/aarch64/start.S arch/aarch64/vectors.S arch/aarch64/device_memory.c
  runtime/memory.c drivers/clock/a733_cpu_clock.c
  apps/memtest/main.c apps/memtest/plan.c apps/memtest/kernels.S
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c" "${PORT}/portASM.S")
target_include_directories(a733-memtest PRIVATE
  include runtime/include platform/a733 apps/memtest "${FREERTOS_DIR}/include" "${PORT}")
# SUNSTONE_USB_RUNTIME builds the monitor's 0x734 renewal SMC; SUNSTONE_MEMTEST
# selects the memtest platform definitions once platform/a733 provides them.
target_compile_definitions(a733-memtest PRIVATE
  GUEST=1 SUNSTONE_SECURE_MONITOR=1 SUNSTONE_USB_RUNTIME=1 SUNSTONE_MEMTEST=1)
target_compile_options(a733-memtest PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra)
# --emit-relocs keeps the relocations that checks/memtest-image.py reads to
# prove the recovery pages reference nothing outside themselves.
target_link_options(a733-memtest PRIVATE -nostdlib -static -Wl,--gc-sections -Wl,--emit-relocs
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/apps/memtest/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/a733-memtest.map")
set_target_properties(a733-memtest PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/apps/memtest/link.ld")
# The DWARF readers of the host checks cannot apply the debug relocations
# that --emit-relocs also keeps; the debug sections are already final.
add_custom_command(TARGET a733-memtest POST_BUILD
  COMMAND "${SUNSTONE_OBJCOPY}" "--remove-section=.rela.debug*" "$<TARGET_FILE:a733-memtest>"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/apps/memtest/beacons.py" "$<TARGET_FILE:a733-memtest>"
  COMMAND "${SUNSTONE_OBJCOPY}" -O binary "$<TARGET_FILE:a733-memtest>" "${CMAKE_CURRENT_BINARY_DIR}/a733-memtest.bin"
  VERBATIM)
