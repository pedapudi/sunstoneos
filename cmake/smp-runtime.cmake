set(NEWLIB_SMP_ROOT "" CACHE PATH "Newlib built by tools/build-newlib-smp.py")
if(NOT NEWLIB_ROOT OR NOT NEWLIB_SMP_ROOT)
  return()
endif()
set(smp_libc "${NEWLIB_SMP_ROOT}/aarch64-none-elf")
add_executable(smp-runtime-model EXCLUDE_FROM_ALL
  platform/qemu-smp/start.S platform/qemu-smp/platform.c
  runtime/newlib.c runtime/newlib_smp.c runtime/newlib_syscalls_smp.c
  runtime/new_delete.cc checks/smp-runtime.cc
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${smp_build}/port.c" "${smp_build}/portASM.S")
target_include_directories(smp-runtime-model PRIVATE platform/qemu-smp include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_include_directories(smp-runtime-model SYSTEM PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:${newlib_cxx};${newlib_cxx}/aarch64-none-elf>" "${smp_libc}/include")
target_compile_features(smp-runtime-model PRIVATE cxx_std_23)
target_compile_definitions(smp-runtime-model PRIVATE SUNSTONE_NEWLIB=1 SUNSTONE_SMP_NEWLIB=1 __DYNAMIC_REENT__=1)
target_compile_options(smp-runtime-model PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffp-contract=off
  -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra
  "$<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>"
  "$<$<COMPILE_LANGUAGE:CXX>:-fhosted;-fno-exceptions;-fno-rtti;-fno-threadsafe-statics;-nostdinc++>")
target_link_options(smp-runtime-model PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--wrap=_impure_ptr
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/runtime.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/smp-runtime-model.map")
target_link_libraries(smp-runtime-model PRIVATE
  "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
  "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a"
  "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
set_target_properties(smp-runtime-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/runtime.ld")
