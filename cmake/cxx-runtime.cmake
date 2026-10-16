# Clang compiles project code; the pinned bare-metal distribution supplies newlib,
# libstdc++ headers and archives, and compiler support routines.
set(NEWLIB_ROOT "" CACHE PATH "Pinned AArch64 bare-metal C/C++ distribution")
if(NOT NEWLIB_ROOT)
  return()
endif()
enable_language(CXX)
set(newlib_sysroot "${NEWLIB_ROOT}/aarch64-none-elf")
set(newlib_cxx "${newlib_sysroot}/include/c++/14.3.1")
foreach(target a733-cxx-check a733-cxx-model)
  add_executable(${target}
    platform/a733/monitor.S platform/a733/monitor.c platform/a733/platform.c
    arch/aarch64/cache.S arch/aarch64/start.S arch/aarch64/vectors.S
    runtime/newlib.c runtime/new_delete.cc checks/cxx-runtime.cc
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c" "${PORT}/portASM.S")
  target_include_directories(${target} PRIVATE include platform/a733
    "${FREERTOS_DIR}/include" "${PORT}")
  target_include_directories(${target} SYSTEM PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${newlib_cxx};${newlib_cxx}/aarch64-none-elf>"
    "${newlib_sysroot}/include")
  target_compile_features(${target} PRIVATE cxx_std_23)
  target_compile_definitions(${target} PRIVATE GUEST=1 SUNSTONE_SECURE_MONITOR=1 SUNSTONE_NEWLIB=1)
  target_compile_options(${target} PRIVATE
    -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
    -fno-unwind-tables -fno-asynchronous-unwind-tables -ffp-contract=off
    -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra
    "$<$<COMPILE_LANGUAGE:CXX>:-fhosted;-fno-exceptions;-fno-rtti;-fno-threadsafe-statics;-nostdinc++>")
  target_link_options(${target} PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/a733/link.ld"
    -Wl,--defsym=__runtime_heap_size=4194304
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
  target_link_libraries(${target} PRIVATE
    "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
    "${newlib_sysroot}/lib/libm.a" "${newlib_sysroot}/lib/libc.a"
    "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
  set_target_properties(${target} PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/a733/link.ld")
endforeach()
target_compile_definitions(a733-cxx-model PRIVATE MONITOR_QEMU=1)
