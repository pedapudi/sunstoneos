# Supervised five-sensor thermal observation on the boot CPU.
set(thsc_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(thsc_root "${thsc_root}" ABSOLUTE)
set(thsc_app "${thsc_root}/apps/thermal-single-core")
set(thsc_shared "${thsc_root}/apps/storage-read-single-core")
set(thsc_build "${CMAKE_CURRENT_BINARY_DIR}/thermal-single-core")
set(thsc_port "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
file(MAKE_DIRECTORY "${thsc_build}")
get_target_property(thsc_sources sunstone-a733-thermal SOURCES)
list(FILTER thsc_sources EXCLUDE REGEX "(/thermal-application/(boot|platform|build-id)\\.c$|/storage-maintenance/(platform|port|continue)\\.(c|S)$|/platform/a733-smp-cached/|/platform/a733/cpu_|/freertos-a733-smp-cached/|/sunstone-a733-thermal/(boot|continue|port|build-id|usb_service)\\.|/a733-smp-observe/(usb\\.c|service\\.S))")
list(APPEND thsc_sources "${thsc_root}/platform/a733/cpu_coherence.c"
 "${thsc_root}/platform/a733/cpu_coherence.S" "${thsc_root}/platform/a733/cpu_start.c"
 "${thsc_root}/platform/a733-smp-cached/memory.c")
foreach(pair IN ITEMS "session;apps/board/session.c" "snapshot;apps/thermal/snapshot.c" "main;apps/board/main.c")
 list(GET pair 0 name)
 list(GET pair 1 path)
 list(REMOVE_ITEM thsc_sources "${thsc_root}/${path}")
 apply_build_patch("${thsc_build}/${name}.c" "${thsc_root}/${path}" "${thsc_app}/${name}.patch")
 list(APPEND thsc_sources "${thsc_build}/${name}.c")
endforeach()
apply_build_patch("${thsc_build}/port.c" "${thsc_port}/port.c" "${thsc_root}/patches/freertos-aarch64-clang.patch")
apply_build_patch("${thsc_build}/platform.c" "${thsc_root}/platform/a733/platform.c" "${thsc_shared}/platform.patch")
list(APPEND thsc_sources "${thsc_shared}/start.S" "${thsc_shared}/monitor.c" "${thsc_shared}/runtime.c"
 "${thsc_build}/platform.c" "${thsc_build}/port.c" "${thsc_port}/portASM.S"
 "${thsc_root}/drivers/usb/a733_usb_platform.c" "${thsc_root}/drivers/usb/a733_usb_platform_native.c"
 "${thsc_root}/arch/aarch64/start.S" "${thsc_root}/arch/aarch64/vectors.S" "${thsc_root}/arch/aarch64/cache.S")
list(REMOVE_DUPLICATES thsc_sources)
set_source_files_properties("${thsc_root}/drivers/gpu/a733_gpu_resources_native.c"
 "${thsc_root}/drivers/i2c/a733_i2c.c" PROPERTIES COMPILE_OPTIONS "-iquote;${thsc_root}/platform/a733")
add_executable(sunstone-a733-thermal-single-core ${thsc_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
get_target_property(thsc_options sunstone-a733-thermal COMPILE_OPTIONS)
get_target_property(thsc_includes sunstone-a733-thermal INCLUDE_DIRECTORIES)
get_target_property(thsc_definitions sunstone-a733-thermal COMPILE_DEFINITIONS)
list(FILTER thsc_definitions EXCLUDE REGEX "^SUNSTONE_A733_SMP_DIAGNOSTIC=")
list(FILTER thsc_includes EXCLUDE REGEX "(a733-smp-cached|freertos-a733-smp-cached)")
set_property(TARGET sunstone-a733-thermal-single-core PROPERTY COMPILE_OPTIONS "${thsc_options}")
target_include_directories(sunstone-a733-thermal-single-core BEFORE PRIVATE "${thsc_app}" "${thsc_port}")
target_include_directories(sunstone-a733-thermal-single-core PRIVATE ${thsc_includes} "${thsc_root}/apps/thermal")
sunstone_enable_radio_security(sunstone-a733-thermal-single-core "${SUNSTONE_RSN_DIR}")
target_compile_definitions(sunstone-a733-thermal-single-core PRIVATE ${thsc_definitions} GUEST=1 SUNSTONE_THERMAL_SINGLE_CORE=1 SUNSTONE_SD_SINGLE_CORE=1)
set_property(SOURCE "${thsc_shared}/monitor.c" "${thsc_shared}/runtime.c" "${thsc_build}/platform.c" APPEND PROPERTY COMPILE_OPTIONS
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-thermal-single-core>:-include>"
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-thermal-single-core>:${thsc_app}/boot.h>")
# Bind every declared producer input; the frozen closure also records compiler depfiles.
file(GLOB thsc_headers "${thsc_app}/*.h" "${thsc_root}/apps/thermal/*.h" "${thsc_root}/include/*.h")
list(FILTER thsc_sources EXCLUDE REGEX "^\\$<TARGET_OBJECTS:")
set(thsc_inputs ${thsc_sources} ${thsc_headers} "${thsc_app}/link.ld" "${thsc_app}/CMakeLists.txt"
 "${thsc_shared}/platform.patch" "${thsc_shared}/boot.h" "${thsc_shared}/cached.h"
 "${thsc_shared}/FreeRTOSConfig.h" "${thsc_app}/session.patch" "${thsc_app}/snapshot.patch" "${thsc_app}/main.patch"
 "${thsc_port}/portmacro.h" "${thsc_port}/port.c" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES thsc_inputs)
set(thsc_identity "thermal-single-core-supervised-v1;ROM-USB-mode2;SRE;60s;${thsc_options};${thsc_definitions};${CMAKE_C_FLAGS};${CMAKE_ASM_FLAGS}")
foreach(input IN LISTS thsc_inputs)
 file(SHA256 "${input}" thsc_hash)
 string(APPEND thsc_identity "${thsc_hash}")
endforeach()
string(SHA256 thsc_build_id "${thsc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," thsc_id_bytes "${thsc_build_id}")
file(WRITE "${thsc_build}/build-id.c.tmp" "const unsigned char a733_smp_build_id[32]={${thsc_id_bytes}};\n")
configure_file("${thsc_build}/build-id.c.tmp" "${thsc_build}/build-id.c" COPYONLY)
file(WRITE "${thsc_build}/build-id.txt" "${thsc_build_id}\n")
string(REPLACE ";" "\n" thsc_input_lines "${thsc_inputs}")
file(WRITE "${thsc_build}/identity-inputs.txt" "${thsc_input_lines}\n")
target_sources(sunstone-a733-thermal-single-core PRIVATE "${thsc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${thsc_inputs})
target_link_options(sunstone-a733-thermal-single-core PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--undefined=thermal_single_core_geometry
 "-Wl,-T,${thsc_app}/link.ld" "-Wl,-Map,${thsc_build}/image.map")
set_target_properties(sunstone-a733-thermal-single-core PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${thsc_app}/link.ld")
set(thsc_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@thsc_build@/manifest.json" "{\"target\":\"sunstone-a733-thermal-single-core\",\"execution_profile\":\"thermal-single-core-supervised-v1\",\"workload\":\"thermal-five-sensor\",\"build_id\":\"@thsc_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${thsc_manifest_script}" thsc_manifest_script @ONLY)
file(WRITE "${thsc_build}/finish-manifest.cmake" "${thsc_manifest_script}")
add_custom_command(TARGET sunstone-a733-thermal-single-core POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-thermal-single-core>" -P "${thsc_build}/finish-manifest.cmake")
