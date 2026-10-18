# Supervised fixed SHA-256/AES application on the boot CPU.
set(crsc_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(crsc_root "${crsc_root}" ABSOLUTE)
set(crsc_app "${crsc_root}/apps/crypto-single-core")
set(crsc_shared "${crsc_root}/apps/storage-read-single-core")
set(crsc_build "${CMAKE_CURRENT_BINARY_DIR}/crypto-single-core")
set(crsc_port "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
file(MAKE_DIRECTORY "${crsc_build}")
get_target_property(crsc_sources sunstone-a733-crypto SOURCES)
list(FILTER crsc_sources EXCLUDE REGEX "(/crypto-application/(boot|platform|build-id)\\.c$|/storage-maintenance/(platform|port|continue)\\.(c|S)$|/platform/a733-smp-cached/|/platform/a733/cpu_|/freertos-a733-smp-cached/|/sunstone-a733-crypto/(boot|continue|port|build-id|usb_service)\\.|/a733-smp-observe/(usb\\.c|service\\.S))")
list(APPEND crsc_sources "${crsc_root}/platform/a733/cpu_coherence.c"
 "${crsc_root}/platform/a733/cpu_coherence.S" "${crsc_root}/platform/a733/cpu_start.c"
 "${crsc_root}/platform/a733-smp-cached/memory.c")
foreach(pair IN ITEMS "session;apps/board/session.c" "snapshot;apps/crypto/snapshot.c" "main;apps/board/main.c")
 list(GET pair 0 name)
 list(GET pair 1 path)
 list(REMOVE_ITEM crsc_sources "${crsc_root}/${path}")
 apply_build_patch("${crsc_build}/${name}.c" "${crsc_root}/${path}" "${crsc_app}/${name}.patch")
 list(APPEND crsc_sources "${crsc_build}/${name}.c")
endforeach()
foreach(unit IN ITEMS monitor runtime)
 apply_build_patch("${crsc_build}/${unit}.c" "${crsc_shared}/${unit}.c" "${crsc_app}/${unit}.patch")
endforeach()
apply_build_patch("${crsc_build}/port.c" "${crsc_port}/port.c" "${crsc_root}/patches/freertos-aarch64-clang.patch")
apply_build_patch("${crsc_build}/platform.c" "${crsc_root}/platform/a733/platform.c" "${crsc_app}/platform.patch")
list(APPEND crsc_sources "${crsc_shared}/start.S" "${crsc_build}/monitor.c" "${crsc_build}/runtime.c"
 "${crsc_build}/platform.c" "${crsc_build}/port.c" "${crsc_port}/portASM.S"
 "${crsc_root}/drivers/usb/a733_usb_platform.c" "${crsc_root}/drivers/usb/a733_usb_platform_native.c"
 "${crsc_root}/arch/aarch64/start.S" "${crsc_root}/arch/aarch64/vectors.S" "${crsc_root}/arch/aarch64/cache.S")
list(REMOVE_DUPLICATES crsc_sources)
set_source_files_properties("${crsc_root}/drivers/gpu/a733_gpu_resources_native.c"
 "${crsc_root}/drivers/i2c/a733_i2c.c" PROPERTIES COMPILE_OPTIONS "-iquote;${crsc_root}/platform/a733")
add_executable(sunstone-a733-crypto-single-core ${crsc_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
get_target_property(crsc_options sunstone-a733-crypto COMPILE_OPTIONS)
get_target_property(crsc_includes sunstone-a733-crypto INCLUDE_DIRECTORIES)
get_target_property(crsc_definitions sunstone-a733-crypto COMPILE_DEFINITIONS)
list(FILTER crsc_definitions EXCLUDE REGEX "^SUNSTONE_A733_SMP_DIAGNOSTIC=")
list(FILTER crsc_includes EXCLUDE REGEX "(a733-smp-cached|freertos-a733-smp-cached)")
set_property(TARGET sunstone-a733-crypto-single-core PROPERTY COMPILE_OPTIONS "${crsc_options}")
target_include_directories(sunstone-a733-crypto-single-core BEFORE PRIVATE "${crsc_app}" "${crsc_port}")
target_include_directories(sunstone-a733-crypto-single-core PRIVATE ${crsc_includes} "${crsc_root}/apps/crypto")
sunstone_enable_radio_security(sunstone-a733-crypto-single-core "${SUNSTONE_RSN_DIR}")
target_compile_definitions(sunstone-a733-crypto-single-core PRIVATE ${crsc_definitions} GUEST=1 SUNSTONE_CRYPTO_SINGLE_CORE=1 SUNSTONE_SD_SINGLE_CORE=1)
set_property(SOURCE "${crsc_build}/monitor.c" "${crsc_build}/runtime.c" "${crsc_build}/platform.c" APPEND PROPERTY COMPILE_OPTIONS
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-crypto-single-core>:-include>"
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-crypto-single-core>:${crsc_app}/boot.h>")
# Bind every declared producer input; the frozen closure also records compiler depfiles.
file(GLOB crsc_headers "${crsc_app}/*.h" "${crsc_root}/apps/crypto/*.h" "${crsc_root}/include/*.h")
list(FILTER crsc_sources EXCLUDE REGEX "^\\$<TARGET_OBJECTS:")
set(crsc_inputs ${crsc_sources} ${crsc_headers} "${crsc_app}/link.ld" "${crsc_app}/CMakeLists.txt"
 "${crsc_app}/platform.patch" "${crsc_shared}/boot.h" "${crsc_shared}/cached.h"
 "${crsc_shared}/FreeRTOSConfig.h" "${crsc_app}/session.patch" "${crsc_app}/snapshot.patch" "${crsc_app}/main.patch" "${crsc_app}/monitor.patch" "${crsc_app}/runtime.patch"
 "${crsc_port}/portmacro.h" "${crsc_port}/port.c" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES crsc_inputs)
set(crsc_identity "crypto-single-core-supervised-v1;ROM-USB-mode2;SRE;60s;${crsc_options};${crsc_definitions};${CMAKE_C_FLAGS};${CMAKE_ASM_FLAGS}")
foreach(input IN LISTS crsc_inputs)
 file(SHA256 "${input}" crsc_hash)
 string(APPEND crsc_identity "${crsc_hash}")
endforeach()
string(SHA256 crsc_build_id "${crsc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," crsc_id_bytes "${crsc_build_id}")
file(WRITE "${crsc_build}/build-id.c.tmp" "const unsigned char a733_smp_build_id[32]={${crsc_id_bytes}};\n")
configure_file("${crsc_build}/build-id.c.tmp" "${crsc_build}/build-id.c" COPYONLY)
file(WRITE "${crsc_build}/build-id.txt" "${crsc_build_id}\n")
string(REPLACE ";" "\n" crsc_input_lines "${crsc_inputs}")
file(WRITE "${crsc_build}/identity-inputs.txt" "${crsc_input_lines}\n")
target_sources(sunstone-a733-crypto-single-core PRIVATE "${crsc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${crsc_inputs})
target_link_options(sunstone-a733-crypto-single-core PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__device_pool_size=2097152 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--undefined=crypto_single_core_geometry
 "-Wl,-T,${crsc_app}/link.ld" "-Wl,-Map,${crsc_build}/image.map")
set_target_properties(sunstone-a733-crypto-single-core PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${crsc_app}/link.ld")
set(crsc_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@crsc_build@/manifest.json" "{\"target\":\"sunstone-a733-crypto-single-core\",\"execution_profile\":\"crypto-single-core-supervised-v1\",\"workload\":\"fixed-sha256-aes128\",\"build_id\":\"@crsc_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${crsc_manifest_script}" crsc_manifest_script @ONLY)
file(WRITE "${crsc_build}/finish-manifest.cmake" "${crsc_manifest_script}")
add_custom_command(TARGET sunstone-a733-crypto-single-core POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-crypto-single-core>" -P "${crsc_build}/finish-manifest.cmake")
