set(MIMUS_DIR "" CACHE PATH "Isolated mimus source with external platform hooks")
if(NOT MIMUS_DIR)
  return()
endif()
if(NOT NEWLIB_ROOT)
  message(FATAL_ERROR "Mimus requires NEWLIB_ROOT")
endif()
option(MIMUS_AARCH64_NEON "Use AArch64 NEON integer dot products" ON)
add_subdirectory("${MIMUS_DIR}/firmware/components/asr" mimus-asr)
# Recognition compute options, placed after the shared kernel options so they
# override them for the mimus libraries only. The library runs only after the
# MMU maps its code, heap, models and arena as Normal memory with SCTLR.A clear,
# so it needs no strict alignment, and C++ compute may use compiler builtins.
# Every A733 core is a Cortex-A55 or A76 and implements ARMv8.2-A. LSE atomics
# stay off like the rest of the system; dot product waits for ID-register
# evidence and an int8 kernel. The shared -ffp-contract=off keeps float parity.
set(MIMUS_COMPUTE_OPTIONS -march=armv8.2-a+nolse -mtune=cortex-a55 -mno-strict-align
  "$<$<COMPILE_LANGUAGE:CXX>:-fbuiltin>")
sunstone_cxx_compile(mimus_asr)
target_compile_options(mimus_asr PRIVATE ${MIMUS_COMPUTE_OPTIONS})
target_compile_definitions(mimus_asr PRIVATE MIMUS_EXTERNAL_PLATFORM=1)
set(mimus_targets a733-mimus a733-mimus-model a733-mimus-replay-model)
if(TINYUSB_DIR)
  list(APPEND mimus_targets a733-mimus-usb)
endif()
foreach(target ${mimus_targets})
  add_executable(${target}
    platform/a733/monitor.S platform/a733/monitor.c platform/a733/platform.c
    arch/aarch64/cache.S arch/aarch64/start.S arch/aarch64/vectors.S
    runtime/newlib.c runtime/new_delete.cc apps/mimus/platform.cc apps/mimus/main.cc apps/mimus/recorded.cc
    "${MIMUS_DIR}/firmware/components/pipeline/src/formatter.cc"
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c" "${PORT}/portASM.S")
  sunstone_cxx_compile(${target})
  target_include_directories(${target} PRIVATE "${MIMUS_DIR}/firmware/components/pipeline/include")
  target_compile_definitions(${target} PRIVATE SUNSTONE_USB_RUNTIME=1)
  target_link_options(${target} PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/a733/link.ld"
    -Wl,--defsym=__runtime_heap_size=4194304
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
  target_link_libraries(${target} PRIVATE mimus_asr
    "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
    "${newlib_sysroot}/lib/libm.a" "${newlib_sysroot}/lib/libc.a"
    "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
  set_target_properties(${target} PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/a733/link.ld")
endforeach()
target_compile_definitions(a733-mimus-model PRIVATE MONITOR_QEMU=1)
target_compile_definitions(a733-mimus-replay-model PRIVATE MONITOR_QEMU=1 SUNSTONE_MIMUS_PACED=1)
set(replay_targets a733-mimus-replay-model)
if(TINYUSB_DIR)
  list(APPEND replay_targets a733-mimus-usb)
endif()
foreach(target ${replay_targets})
  target_sources(${target} PRIVATE apps/mimus/replay.cc apps/mimus/replay_runtime.cc apps/mimus/paced.cc
    "${MIMUS_DIR}/firmware/components/pipeline/src/segmenter.cc")
endforeach()

if(TINYUSB_DIR)
  target_sources(a733-mimus-usb PRIVATE apps/mimus/service.cc
    drivers/usb/a733_usb.c drivers/usb/diagnostic.c
    "${TINYUSB_DIR}/src/tusb.c" "${TINYUSB_DIR}/src/common/tusb_fifo.c"
    "${TINYUSB_DIR}/src/device/usbd.c" "${TINYUSB_DIR}/src/class/vendor/vendor_device.c"
    "${CMAKE_CURRENT_BINARY_DIR}/tinyusb-a733/dcd_sunxi_musb.c")
  target_include_directories(a733-mimus-usb PRIVATE drivers/usb "${TINYUSB_DIR}/src")
  target_compile_definitions(a733-mimus-usb PRIVATE SUNSTONE_A733_USB=1 SUNSTONE_MIMUS_USB=1)
endif()
