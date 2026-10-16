cmake_minimum_required(VERSION 3.24)
project(sunstoneos LANGUAGES C ASM)

set(FREERTOS_DIR "" CACHE PATH "Pinned FreeRTOS kernel source directory")
if(NOT EXISTS "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE/port.c")
  message(FATAL_ERROR "Set FREERTOS_DIR to the kernel fetched by tools/fetch-sources.py")
endif()

set(PORT "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
execute_process(COMMAND patch --silent --output
  "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c" "${PORT}/port.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-aarch64-clang.patch"
  RESULT_VARIABLE patch_status)
if(NOT patch_status EQUAL 0)
  message(FATAL_ERROR "Could not apply the pinned FreeRTOS Clang compatibility patch")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PORT}/port.c" patches/freertos-aarch64-clang.patch)
add_executable(kernel-check
  arch/aarch64/start.S
  arch/aarch64/vectors.S
  platform/qemu-virt/platform.c
  runtime/memory.c
  checks/kernel.c
  checks/fp-context.S
  "${FREERTOS_DIR}/tasks.c"
  "${FREERTOS_DIR}/queue.c"
  "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c"
  "${PORT}/portASM.S")
target_include_directories(kernel-check PRIVATE
  include runtime/include "${FREERTOS_DIR}/include" "${PORT}")
target_compile_definitions(kernel-check PRIVATE GUEST=1)
target_compile_options(kernel-check PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra)
target_link_options(kernel-check PRIVATE
  -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-virt/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/kernel-check.map")
set_target_properties(kernel-check PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-virt/link.ld")

# The physical platform and its secure QEMU model share the monitor and kernel.
foreach(target a733-kernel-check a733-kernel-model)
  add_executable(${target}
    platform/a733/monitor.S platform/a733/monitor.c platform/a733/platform.c
    arch/aarch64/cache.S arch/aarch64/start.S arch/aarch64/vectors.S
    runtime/memory.c checks/kernel.c checks/fp-context.S
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c" "${PORT}/portASM.S")
  target_include_directories(${target} PRIVATE
    include runtime/include platform/a733 "${FREERTOS_DIR}/include" "${PORT}")
  target_compile_definitions(${target} PRIVATE GUEST=1 SUNSTONE_SECURE_MONITOR=1)
  target_compile_options(${target} PRIVATE
    -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
    -fno-unwind-tables -fno-asynchronous-unwind-tables
    -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra)
  target_link_options(${target} PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/a733/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
  set_target_properties(${target} PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/a733/link.ld")
endforeach()
set_source_files_properties(platform/a733/monitor.c PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
target_compile_definitions(a733-kernel-model PRIVATE MONITOR_QEMU=1)
