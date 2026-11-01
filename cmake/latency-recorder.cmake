include("${CMAKE_CURRENT_LIST_DIR}/kernel-flags.cmake")
add_executable(latency-recorder-model EXCLUDE_FROM_ALL
  platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c
  runtime/latency.c checks/latency-recorder.c
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${smp_build}/port.c" "${smp_build}/portASM.S")
target_include_directories(latency-recorder-model PRIVATE platform/qemu-smp include runtime/include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(latency-recorder-model PRIVATE SUNSTONE_LATENCY=1)
target_compile_options(latency-recorder-model PRIVATE ${SUNSTONE_KERNEL_FLAGS}
  $<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only> -Werror)
target_link_options(latency-recorder-model PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${SUNSTONE_SOURCE_ROOT}/platform/qemu-smp/link.ld")
set_target_properties(latency-recorder-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${SUNSTONE_SOURCE_ROOT}/platform/qemu-smp/link.ld")
