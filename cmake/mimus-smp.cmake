if(NOT MIMUS_DIR OR NOT NEWLIB_ROOT OR NOT NEWLIB_SMP_ROOT)
  return()
endif()
# Recompile the same recognition sources against the SMP C library headers.
get_target_property(asr_sources mimus_asr SOURCES)
get_target_property(asr_directory mimus_asr SOURCE_DIR)
set(smp_asr_sources)
foreach(source IN LISTS asr_sources)
  cmake_path(ABSOLUTE_PATH source BASE_DIRECTORY "${asr_directory}")
  list(APPEND smp_asr_sources "${source}")
endforeach()
add_library(mimus_asr_smp STATIC EXCLUDE_FROM_ALL ${smp_asr_sources})
sunstone_smp_compile(mimus_asr_smp)
target_include_directories(mimus_asr_smp PUBLIC "${MIMUS_DIR}/firmware/components/asr/include")
target_compile_definitions(mimus_asr_smp PRIVATE MIMUS_EXTERNAL_PLATFORM=1)
if(MIMUS_AARCH64_NEON)
  target_compile_definitions(mimus_asr_smp PRIVATE MIMUS_AARCH64_NEON=1)
endif()
foreach(target IN ITEMS smp-mimus-model smp-mimus-replay-model smp-mimus-linear-model)
  if(target STREQUAL "smp-mimus-linear-model")
    set(mimus_entry checks/mimus-linear.cc)
  else()
    set(mimus_entry apps/mimus/main.cc)
  endif()
  add_executable(${target} EXCLUDE_FROM_ALL
    platform/qemu-smp/start.S platform/qemu-smp/platform.c
    runtime/newlib.c runtime/newlib_smp.c runtime/newlib_syscalls_smp.c runtime/new_delete.cc
    runtime/cxx_terminate.cc
    apps/mimus/platform.cc ${mimus_entry} apps/mimus/recorded.cc apps/mimus/parallel_runtime.cc
    "${MIMUS_DIR}/firmware/components/pipeline/src/formatter.cc"
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${smp_build}/port.c" "${smp_build}/portASM.S")
  sunstone_smp_compile(${target})
  target_include_directories(${target} PRIVATE apps/mimus "${MIMUS_DIR}/firmware/components/pipeline/include")
  target_compile_definitions(${target} PRIVATE MONITOR_QEMU=1)
  target_link_options(${target} PRIVATE -nostdlib -static -Wl,--gc-sections -Wl,--wrap=_impure_ptr
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/runtime.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
  target_link_libraries(${target} PRIVATE mimus_asr_smp
    "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
    "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a"
    "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
  set_target_properties(${target} PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/runtime.ld")
endforeach()
target_compile_definitions(smp-mimus-replay-model PRIVATE SUNSTONE_MIMUS_PACED=1)
target_sources(smp-mimus-replay-model PRIVATE
  apps/mimus/replay.cc apps/mimus/replay_runtime.cc apps/mimus/paced.cc
  "${MIMUS_DIR}/firmware/components/pipeline/src/segmenter.cc")
