# Native cached eight-core composition. Storage supplies shared initialization,
# fatal handling and launcher mechanics; recognition uses the SMP C++ runtime.
if(NOT MIMUS_DIR OR NOT NEWLIB_ROOT OR NOT NEWLIB_SMP_ROOT OR NOT TINYUSB_DIR)
  return()
endif()
if(NOT TARGET sunstone-a733-storage-maintenance)
  include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-storage-maintenance.cmake")
endif()
option(SUNSTONE_MIMUS_SUPERVISED "Retained mimus session with manual whole-board power removal" OFF)
set(mimus_native_build "${CMAKE_CURRENT_BINARY_DIR}/mimus-native")
file(MAKE_DIRECTORY "${mimus_native_build}")
get_target_property(mimus_native_sources sunstone-a733-storage-maintenance SOURCES)
# runtime/memory.c and memmove.c stay linked: they replace newlib's SIMD string
# routines, which fault when EL3 boot code clears or copies Device memory.
foreach(source IN ITEMS
  platform/a733-storage-maintenance/main.c platform/a733-storage-maintenance/session.c platform/a733-storage-maintenance/admission.c
  platform/a733-storage-update/bootstrap.c platform/a733-storage-update/worker.c platform/a733-storage-update/usb.c
  drivers/watchdog/a733_maintenance_watchdog.c drivers/storage/a733_storage_update.c
  drivers/storage/storage_update_usb.c drivers/storage/a733_mmc_platform.c
  drivers/storage/a733_mmc_platform_native.c drivers/storage/sunxi_mmc.c)
  list(REMOVE_ITEM mimus_native_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
list(REMOVE_ITEM mimus_native_sources "${storage_build}/build-id.c")
if(SUNSTONE_MIMUS_SUPERVISED)
 list(REMOVE_ITEM mimus_native_sources "${storage_build}/boot.c" "${SUNSTONE_SOURCE_ROOT}/platform/a733/maintenance_watchdog.c")
 apply_build_patch("${mimus_native_build}/boot.c" "${storage_build}/boot.c" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-supervised/boot.patch")
 list(APPEND mimus_native_sources "${mimus_native_build}/boot.c")
endif()
list(APPEND mimus_native_sources "${SUNSTONE_SOURCE_ROOT}/apps/mimus/admission.c")
apply_build_patch("${mimus_native_build}/link-base.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link.ld"
  "${SUNSTONE_SOURCE_ROOT}/apps/mimus/link.patch")
apply_build_patch("${mimus_native_build}/link.ld" "${mimus_native_build}/link-base.ld"
  "${SUNSTONE_SOURCE_ROOT}/platform/a733-storage-maintenance/link.patch")
list(APPEND mimus_native_sources
  "${SUNSTONE_SOURCE_ROOT}/apps/mimus/native.c" "${SUNSTONE_SOURCE_ROOT}/apps/mimus/main.cc" "${SUNSTONE_SOURCE_ROOT}/apps/mimus/recorded.cc"
  "${SUNSTONE_SOURCE_ROOT}/apps/mimus/platform.cc" "${SUNSTONE_SOURCE_ROOT}/apps/mimus/parallel_runtime.cc"
  "${SUNSTONE_SOURCE_ROOT}/apps/mimus/paced.cc" "${SUNSTONE_SOURCE_ROOT}/apps/mimus/replay.cc"
  "${SUNSTONE_SOURCE_ROOT}/apps/mimus/replay_runtime.cc"
  "${SUNSTONE_SOURCE_ROOT}/runtime/newlib.c" "${SUNSTONE_SOURCE_ROOT}/runtime/newlib_smp.c"
  "${SUNSTONE_SOURCE_ROOT}/runtime/newlib_syscalls_smp.c" "${SUNSTONE_SOURCE_ROOT}/runtime/new_delete.cc"
  "${SUNSTONE_SOURCE_ROOT}/runtime/cxx_terminate.cc"
  "${MIMUS_DIR}/firmware/components/pipeline/src/formatter.cc"
  "${MIMUS_DIR}/firmware/components/pipeline/src/segmenter.cc")
set(mimus_native_inputs ${mimus_native_sources} ${storage_inputs}
  "${CMAKE_CURRENT_LIST_FILE}" "${mimus_native_build}/link.ld")
file(GLOB mimus_native_headers "${SUNSTONE_SOURCE_ROOT}/apps/mimus/*.h")
list(APPEND mimus_native_inputs ${mimus_native_headers})
get_target_property(mimus_native_asr_sources mimus_asr_smp SOURCES)
file(GLOB_RECURSE mimus_native_model_headers "${MIMUS_DIR}/firmware/components/asr/include/*.h"
  "${MIMUS_DIR}/firmware/components/pipeline/include/*.h")
list(APPEND mimus_native_inputs ${mimus_native_asr_sources} ${mimus_native_model_headers}
  "${SUNSTONE_SOURCE_ROOT}/cmake/mimus.cmake" "${SUNSTONE_SOURCE_ROOT}/cmake/mimus-smp.cmake"
  "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
  "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a"
  "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a"
  "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/freertos_tasks_c_additions.h"
  "${SUNSTONE_SOURCE_ROOT}/platform/qemu-smp/freertos_tasks_c_additions.h")
list(REMOVE_DUPLICATES mimus_native_inputs)
set(mimus_native_identity "native-mimus-1;${SUNSTONE_MIMUS_SUPERVISED};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};8;two-inference-workers;WBWA")
foreach(input IN LISTS mimus_native_inputs)
  file(SHA256 "${input}" input_digest)
  string(APPEND mimus_native_identity "${input_digest}")
endforeach()
string(SHA256 mimus_native_build_id "${mimus_native_identity}")
string(REGEX REPLACE "(..)" "0x\\1," mimus_native_id_bytes "${mimus_native_build_id}")
file(WRITE "${mimus_native_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${mimus_native_id_bytes}};\n")
file(WRITE "${mimus_native_build}/build-id.txt" "${mimus_native_build_id}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${mimus_native_inputs})
add_executable(sunstone-a733-mimus-native EXCLUDE_FROM_ALL ${mimus_native_sources} "${mimus_native_build}/build-id.c")
set(mimus_native_target sunstone-a733-mimus-native)
sunstone_smp_compile(${mimus_native_target})
# The pinned newlib stdatomic header requires stdint typedefs before inclusion.
target_compile_options(${mimus_native_target} PRIVATE "$<$<COMPILE_LANGUAGE:C>:-include;stdint.h>")
target_include_directories(${mimus_native_target} BEFORE PRIVATE
  "${SUNSTONE_SOURCE_ROOT}/apps/mimus" "${SUNSTONE_SOURCE_ROOT}/platform/a733-storage-maintenance"
  "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached")
target_include_directories(${mimus_native_target} PRIVATE "${SUNSTONE_SOURCE_ROOT}/platform/a733-storage-update"
  "${SUNSTONE_SOURCE_ROOT}/drivers/usb" "${TINYUSB_DIR}/src" "${storage_base}"
  "${MIMUS_DIR}/firmware/components/pipeline/include")
target_compile_definitions(${mimus_native_target} PRIVATE SUNSTONE_SECURE_MONITOR=1 SUNSTONE_A733_SMP_DIAGNOSTIC=1
  SUNSTONE_A733_USB=1 SUNSTONE_MIMUS_NATIVE=1 SUNSTONE_MIMUS_PACED=1
 )
if(SUNSTONE_MIMUS_SUPERVISED)
 target_compile_definitions(${mimus_native_target} PRIVATE SUNSTONE_MIMUS_SUPERVISED=1 A733_SMP_CONTRACT_MAGIC=0x415337334d494333ULL A733_SMP_RESULT_MAGIC=0x415337334d495233ULL)
else()
 target_compile_definitions(${mimus_native_target} PRIVATE A733_SMP_CONTRACT_MAGIC=0x415337334d494331ULL A733_SMP_RESULT_MAGIC=0x415337334d495231ULL)
endif()
target_link_options(${mimus_native_target} PRIVATE -nostdlib -static -Wl,--gc-sections -Wl,--wrap=_impure_ptr
  "-Wl,-T,${mimus_native_build}/link.ld" -Wl,--defsym=__runtime_heap_size=4194304
  "-Wl,-Map,${mimus_native_build}/image.map")
target_link_libraries(${mimus_native_target} PRIVATE mimus_asr_smp
  "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
  "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a"
  "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
set_target_properties(${mimus_native_target} PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${mimus_native_build}/link.ld")
