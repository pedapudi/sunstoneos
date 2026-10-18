# Supervised RTC owner and native clients on the boot CPU.
set(rtsc_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(rtsc_root "${rtsc_root}" ABSOLUTE)
set(rtsc_app "${rtsc_root}/apps/rtc-single-core")
set(rtsc_shared "${rtsc_root}/apps/storage-read-single-core")
set(rtsc_build "${CMAKE_CURRENT_BINARY_DIR}/rtc-single-core")
set(rtsc_port "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
file(MAKE_DIRECTORY "${rtsc_build}")
get_target_property(rtsc_sources sunstone-a733-rtc SOURCES)
list(FILTER rtsc_sources EXCLUDE REGEX "(/rtc-application/(boot|platform|build-id)\\.c$|/storage-maintenance/(platform|port|continue)\\.(c|S)$|/platform/a733-smp-cached/|/platform/a733/cpu_|/freertos-a733-smp-cached/|/sunstone-a733-rtc/(boot|continue|port|build-id|usb_service)\\.|/a733-smp-observe/(usb\\.c|service\\.S))")
list(APPEND rtsc_sources "${rtsc_root}/platform/a733/cpu_coherence.c"
 "${rtsc_root}/platform/a733/cpu_coherence.S" "${rtsc_root}/platform/a733/cpu_start.c"
 "${rtsc_root}/platform/a733-smp-cached/memory.c")
foreach(pair IN ITEMS "session;apps/board/session.c" "snapshot;apps/rtc/snapshot.c" "initialize;apps/rtc/initialize.c" "main;apps/board/main.c")
 list(GET pair 0 name)
 list(GET pair 1 path)
 list(REMOVE_ITEM rtsc_sources "${rtsc_root}/${path}")
 apply_build_patch("${rtsc_build}/${name}.c" "${rtsc_root}/${path}" "${rtsc_app}/${name}.patch")
 list(APPEND rtsc_sources "${rtsc_build}/${name}.c")
endforeach()
apply_build_patch("${rtsc_build}/port.c" "${rtsc_port}/port.c" "${rtsc_root}/patches/freertos-aarch64-clang.patch")
apply_build_patch("${rtsc_build}/platform.c" "${rtsc_root}/platform/a733/platform.c" "${rtsc_shared}/platform.patch")
list(APPEND rtsc_sources "${rtsc_shared}/start.S" "${rtsc_shared}/monitor.c" "${rtsc_shared}/runtime.c"
 "${rtsc_build}/platform.c" "${rtsc_build}/port.c" "${rtsc_port}/portASM.S"
 "${rtsc_root}/drivers/usb/a733_usb_platform.c" "${rtsc_root}/drivers/usb/a733_usb_platform_native.c"
 "${rtsc_root}/arch/aarch64/start.S" "${rtsc_root}/arch/aarch64/vectors.S" "${rtsc_root}/arch/aarch64/cache.S")
list(REMOVE_DUPLICATES rtsc_sources)
set_source_files_properties("${rtsc_root}/drivers/gpu/a733_gpu_resources_native.c"
 "${rtsc_root}/drivers/i2c/a733_i2c.c" PROPERTIES COMPILE_OPTIONS "-iquote;${rtsc_root}/platform/a733")
add_executable(sunstone-a733-rtc-single-core ${rtsc_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
get_target_property(rtsc_options sunstone-a733-rtc COMPILE_OPTIONS)
get_target_property(rtsc_includes sunstone-a733-rtc INCLUDE_DIRECTORIES)
get_target_property(rtsc_definitions sunstone-a733-rtc COMPILE_DEFINITIONS)
list(FILTER rtsc_definitions EXCLUDE REGEX "^SUNSTONE_A733_SMP_DIAGNOSTIC=")
list(FILTER rtsc_includes EXCLUDE REGEX "(a733-smp-cached|freertos-a733-smp-cached)")
set_property(TARGET sunstone-a733-rtc-single-core PROPERTY COMPILE_OPTIONS "${rtsc_options}")
target_include_directories(sunstone-a733-rtc-single-core BEFORE PRIVATE "${rtsc_app}" "${rtsc_port}")
target_include_directories(sunstone-a733-rtc-single-core PRIVATE ${rtsc_includes} "${rtsc_root}/apps/rtc")
sunstone_enable_radio_security(sunstone-a733-rtc-single-core "${SUNSTONE_RSN_DIR}")
target_compile_definitions(sunstone-a733-rtc-single-core PRIVATE ${rtsc_definitions} GUEST=1 SUNSTONE_RTC_SINGLE_CORE=1 SUNSTONE_SD_SINGLE_CORE=1)
set_property(SOURCE "${rtsc_shared}/monitor.c" "${rtsc_shared}/runtime.c" "${rtsc_build}/platform.c" APPEND PROPERTY COMPILE_OPTIONS
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-rtc-single-core>:-include>"
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-rtc-single-core>:${rtsc_app}/boot.h>")
# Bind every declared producer input; the frozen closure also records compiler depfiles.
file(GLOB rtsc_headers "${rtsc_app}/*.h" "${rtsc_root}/apps/rtc/*.h" "${rtsc_root}/include/*.h")
list(FILTER rtsc_sources EXCLUDE REGEX "^\\$<TARGET_OBJECTS:")
set(rtsc_inputs ${rtsc_sources} ${rtsc_headers} "${rtsc_app}/link.ld" "${rtsc_app}/CMakeLists.txt"
 "${rtsc_shared}/platform.patch" "${rtsc_shared}/boot.h" "${rtsc_shared}/cached.h"
 "${rtsc_shared}/FreeRTOSConfig.h" "${rtsc_app}/session.patch" "${rtsc_app}/snapshot.patch" "${rtsc_app}/initialize.patch" "${rtsc_app}/main.patch"
 "${rtsc_port}/portmacro.h" "${rtsc_port}/port.c" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES rtsc_inputs)
set(rtsc_identity "rtc-single-core-supervised-v1;ROM-USB-mode2;SRE;60s;${rtsc_options};${rtsc_definitions};${CMAKE_C_FLAGS};${CMAKE_ASM_FLAGS}")
foreach(input IN LISTS rtsc_inputs)
 file(SHA256 "${input}" rtsc_hash)
 string(APPEND rtsc_identity "${rtsc_hash}")
endforeach()
string(SHA256 rtsc_build_id "${rtsc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," rtsc_id_bytes "${rtsc_build_id}")
file(WRITE "${rtsc_build}/build-id.c.tmp" "const unsigned char a733_smp_build_id[32]={${rtsc_id_bytes}};\n")
configure_file("${rtsc_build}/build-id.c.tmp" "${rtsc_build}/build-id.c" COPYONLY)
file(WRITE "${rtsc_build}/build-id.txt" "${rtsc_build_id}\n")
string(REPLACE ";" "\n" rtsc_input_lines "${rtsc_inputs}")
file(WRITE "${rtsc_build}/identity-inputs.txt" "${rtsc_input_lines}\n")
target_sources(sunstone-a733-rtc-single-core PRIVATE "${rtsc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${rtsc_inputs})
target_link_options(sunstone-a733-rtc-single-core PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--undefined=rtc_single_core_geometry
 "-Wl,-T,${rtsc_app}/link.ld" "-Wl,-Map,${rtsc_build}/image.map")
set_target_properties(sunstone-a733-rtc-single-core PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${rtsc_app}/link.ld")
set(rtsc_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@rtsc_build@/manifest.json" "{\"target\":\"sunstone-a733-rtc-single-core\",\"execution_profile\":\"rtc-single-core-supervised-v1\",\"workload\":\"rtc-owner-service\",\"build_id\":\"@rtsc_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${rtsc_manifest_script}" rtsc_manifest_script @ONLY)
file(WRITE "${rtsc_build}/finish-manifest.cmake" "${rtsc_manifest_script}")
add_custom_command(TARGET sunstone-a733-rtc-single-core POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-rtc-single-core>" -P "${rtsc_build}/finish-manifest.cmake")
