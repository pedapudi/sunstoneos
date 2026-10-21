# One native I2S0 stream, retained DMA periods and USB0 sink observation.
if(NOT TARGET sunstone-a733-storage-maintenance)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-storage-maintenance.cmake")
endif()
option(SUNSTONE_I2S_TRANSCRIBE "Transcribe finite native I2S capture with unchanged mimus" OFF)
set(i2s_root "${SUNSTONE_SOURCE_ROOT}")
set(i2s_build "${CMAKE_CURRENT_BINARY_DIR}/i2s-application")
file(MAKE_DIRECTORY "${i2s_build}")
get_target_property(i2s_sources sunstone-a733-storage-maintenance SOURCES)
foreach(source IN ITEMS
 platform/a733-storage-maintenance/main.c platform/a733-storage-maintenance/session.c
 platform/a733-storage-maintenance/admission.c platform/a733-storage-update/bootstrap.c
 platform/a733-storage-update/worker.c platform/a733-storage-update/usb.c
 drivers/storage/a733_storage_update.c drivers/storage/storage_update_usb.c
 drivers/storage/a733_mmc_platform.c drivers/storage/a733_mmc_platform_native.c
 drivers/storage/sunxi_mmc.c drivers/watchdog/a733_maintenance_watchdog.c)
 list(REMOVE_ITEM i2s_sources "${i2s_root}/${source}")
endforeach()
list(REMOVE_ITEM i2s_sources "${storage_build}/build-id.c")
# Retain maintained boot/platform/EL3 return mechanics. Its boot patch publishes
# the immutable 4KiB session page and does not decode a storage grant.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SUNSTONE_I2S_RECORDED_WAV "" CACHE FILEPATH "Pinned existing recorded PCM fixture")
if(NOT SUNSTONE_I2S_RECORDED_WAV)
 message(FATAL_ERROR "Native I2S requires the pinned recorded development WAV")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${i2s_root}/apps/i2s/recorded.py"
 "${SUNSTONE_I2S_RECORDED_WAV}" "${i2s_build}/recorded.c" RESULT_VARIABLE pcm_status)
if(NOT pcm_status EQUAL 0)
 message(FATAL_ERROR "I2S recorded input verification failed")
endif()
list(REMOVE_ITEM i2s_sources "${storage_build}/boot.c" "${i2s_root}/platform/a733/maintenance_watchdog.c")
apply_build_patch("${i2s_build}/boot-supervised.c" "${storage_build}/boot.c"
 "${i2s_root}/platform/a733-smp-supervised/boot.patch")
apply_build_patch("${i2s_build}/boot.c" "${i2s_build}/boot-supervised.c" "${i2s_root}/apps/i2s/boot.patch")
list(APPEND i2s_sources "${i2s_build}/boot.c" "${i2s_build}/recorded.c")
foreach(source IN ITEMS apps/i2s/main.c apps/i2s/admission.c apps/i2s/snapshot.c apps/i2s/client.c apps/i2s/repeated.c apps/i2s/runtime.c apps/i2s/usb0.c
 platform/a733/memory_reservations.c runtime/device_memory_pool.c platform/a733/dma_pool_session.c
 platform/a733/dma_startup.c platform/a733/dma_native.c drivers/dma/dma_memory.c
 drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c drivers/dma/a733_dma_platform.c
 drivers/dma/a733_dma_platform_native.c drivers/dma/dma_service.c drivers/dma/dma_service_i2s.c
 drivers/dma/dma_service_i2s_stream.c drivers/audio/a733_i2s.c drivers/audio/a733_i2s_stream.c
 drivers/audio/a733_i2s_platform.c drivers/audio/a733_i2s_platform_native.c drivers/audio/a733_audio_leases.c)
 list(APPEND i2s_sources "${i2s_root}/${source}")
endforeach()
add_executable(sunstone-a733-i2s EXCLUDE_FROM_ALL ${i2s_sources})
get_target_property(i2s_options sunstone-a733-storage-maintenance COMPILE_OPTIONS)
set_property(TARGET sunstone-a733-i2s PROPERTY COMPILE_OPTIONS "${i2s_options}")
target_include_directories(sunstone-a733-i2s BEFORE PRIVATE "${i2s_root}/apps/i2s"
 "${i2s_root}/platform/a733-storage-update" "${i2s_root}/platform/a733-smp-cached"
 "${i2s_root}/drivers/usb" "${TINYUSB_DIR}/src" "${i2s_root}/include"
 "${i2s_root}/runtime/include" "${FREERTOS_DIR}/include" "${storage_base}")
target_compile_definitions(sunstone-a733-i2s PRIVATE SUNSTONE_SECURE_MONITOR=1 SUNSTONE_A733_SMP_DIAGNOSTIC=1
 SUNSTONE_A733_USB=1 A733_SMP_CONTRACT_MAGIC=0x4153373341554331ULL A733_SMP_RESULT_MAGIC=0x4153373341555231ULL)
if(SUNSTONE_I2S_TRANSCRIBE)
 if(NOT TARGET mimus_asr_smp OR NOT NEWLIB_ROOT OR NOT NEWLIB_SMP_ROOT)
  message(FATAL_ERROR "I2S transcription requires root composition with pinned mimus and SMP newlib")
 endif()
 set(i2s_recognition_sources
  "${i2s_root}/apps/i2s/recognition.cc" "${i2s_root}/apps/mimus/recorded.cc"
  "${i2s_root}/apps/mimus/platform.cc" "${i2s_root}/runtime/newlib.c"
  "${i2s_root}/runtime/newlib_smp.c" "${i2s_root}/runtime/newlib_syscalls_smp.c"
  "${i2s_root}/runtime/new_delete.cc" "${i2s_root}/runtime/cxx_terminate.cc"
  "${MIMUS_DIR}/firmware/components/pipeline/src/formatter.cc")
 target_sources(sunstone-a733-i2s PRIVATE ${i2s_recognition_sources})
 list(APPEND i2s_sources ${i2s_recognition_sources})
 get_target_property(i2s_includes sunstone-a733-i2s INCLUDE_DIRECTORIES)
 list(REMOVE_ITEM i2s_includes "${i2s_root}/runtime/include")
 set_property(TARGET sunstone-a733-i2s PROPERTY INCLUDE_DIRECTORIES "${i2s_includes}")
 sunstone_smp_compile(sunstone-a733-i2s)
 target_include_directories(sunstone-a733-i2s PRIVATE "${i2s_root}/apps/mimus"
  "${MIMUS_DIR}/firmware/components/pipeline/include")
 target_compile_definitions(sunstone-a733-i2s PRIVATE SUNSTONE_I2S_TRANSCRIBE=1 "$<$<COMPILE_LANGUAGE:CXX>:SUNSTONE_MIMUS_NATIVE=1>")
 target_compile_options(sunstone-a733-i2s PRIVATE "$<$<COMPILE_LANGUAGE:C>:-include;stdint.h>")
 target_link_libraries(sunstone-a733-i2s PRIVATE mimus_asr_smp
  "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
  "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a"
  "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
 apply_build_patch("${i2s_build}/link-base.ld" "${i2s_root}/platform/a733-smp-cached/link.ld"
  "${i2s_root}/apps/mimus/link.patch")
 apply_build_patch("${i2s_build}/link.ld" "${i2s_build}/link-base.ld"
  "${i2s_root}/platform/a733-storage-maintenance/link.patch")
 set(i2s_link "${i2s_build}/link.ld")
 target_link_options(sunstone-a733-i2s PRIVATE -Wl,--wrap=_impure_ptr -Wl,--defsym=__runtime_heap_size=4194304)
else()
 set(i2s_link "${storage_build}/link.ld")
endif()
file(GLOB i2s_headers "${i2s_root}/apps/i2s/*.h")
set(i2s_inputs ${i2s_sources} ${i2s_headers} ${storage_inputs}
 "${i2s_root}/apps/i2s/boot.patch"
 "${CMAKE_CURRENT_LIST_FILE}" "${i2s_root}/apps/mimus/link.patch" "${i2s_root}/apps/i2s/CMakeLists.txt" "${i2s_root}/apps/i2s/recorded.py"
 "${i2s_root}/platform/a733-smp-supervised/boot.patch" "${SUNSTONE_I2S_RECORDED_WAV}")
if(SUNSTONE_I2S_TRANSCRIBE)
 get_target_property(i2s_asr_sources mimus_asr_smp SOURCES)
 file(GLOB_RECURSE i2s_model_headers "${MIMUS_DIR}/firmware/components/asr/include/*.h"
  "${MIMUS_DIR}/firmware/components/pipeline/include/*.h")
 list(APPEND i2s_inputs ${i2s_asr_sources} ${i2s_model_headers}
  "${i2s_root}/apps/mimus/recorded.h" "${i2s_root}/apps/mimus/timing.h"
  "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
  "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a"
  "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
endif()
list(REMOVE_DUPLICATES i2s_inputs)
set(i2s_identity "native-i2s-1;${SUNSTONE_I2S_TRANSCRIBE};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};8;I2S0;16kHz;mono;S16;USB0-sink;supervised-retained")
foreach(input IN LISTS i2s_inputs)
 file(SHA256 "${input}" i2s_digest)
 string(APPEND i2s_identity "${i2s_digest}")
endforeach()
string(SHA256 i2s_build_id "${i2s_identity}")
string(REGEX REPLACE "(..)" "0x\\1," i2s_id_bytes "${i2s_build_id}")
file(WRITE "${i2s_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${i2s_id_bytes}};\n")
file(WRITE "${i2s_build}/build-id.txt" "${i2s_build_id}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${i2s_inputs})
target_sources(sunstone-a733-i2s PRIVATE "${i2s_build}/build-id.c")
target_link_options(sunstone-a733-i2s PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__device_pool_size=2097152 "-Wl,-T,${i2s_link}" "-Wl,-Map,${i2s_build}/image.map")
set_target_properties(sunstone-a733-i2s PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${i2s_link}")
