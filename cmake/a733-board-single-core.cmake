# Complete supervised board application on the boot CPU.
set(bdsc_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(bdsc_root "${bdsc_root}" ABSOLUTE)
set(bdsc_app "${bdsc_root}/apps/board-single-core")
set(bdsc_shared "${bdsc_root}/apps/storage-read-single-core")
set(bdsc_build "${CMAKE_CURRENT_BINARY_DIR}/board-single-core")
set(bdsc_port "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
file(MAKE_DIRECTORY "${bdsc_build}")
get_target_property(bdsc_sources sunstone-a733-board SOURCES)
list(FILTER bdsc_sources EXCLUDE REGEX "(/board-application/(boot|boot-supervised|platform|build-id)\\.c$|/storage-maintenance/(platform|port|continue)\\.(c|S)$|/platform/a733-smp-cached/|/platform/a733/cpu_|/freertos-a733-smp-cached/|/sunstone-a733-board/(boot|continue|port|build-id|usb_service)\\.|/a733-smp-observe/(usb\\.c|service\\.S))")
list(APPEND bdsc_sources "${bdsc_root}/platform/a733/cpu_coherence.c"
 "${bdsc_root}/platform/a733/cpu_coherence.S" "${bdsc_root}/platform/a733/cpu_start.c"
 "${bdsc_root}/platform/a733-smp-cached/memory.c")
foreach(pair IN ITEMS "session;apps/board/session.c" "identity;apps/board/identity.c" "admission;apps/board/admission.c" "main;apps/board/main.c")
 list(GET pair 0 name)
 list(GET pair 1 path)
 list(REMOVE_ITEM bdsc_sources "${bdsc_root}/${path}")
 apply_build_patch("${bdsc_build}/${name}.c" "${bdsc_root}/${path}" "${bdsc_app}/${name}.patch")
 list(APPEND bdsc_sources "${bdsc_build}/${name}.c")
endforeach()
apply_build_patch("${bdsc_build}/port.c" "${bdsc_port}/port.c" "${bdsc_root}/patches/freertos-aarch64-clang.patch")
apply_build_patch("${bdsc_build}/platform.c" "${bdsc_root}/platform/a733/platform.c" "${bdsc_shared}/platform.patch")
list(APPEND bdsc_sources "${bdsc_shared}/start.S" "${bdsc_shared}/monitor.c" "${bdsc_shared}/runtime.c"
 "${bdsc_build}/platform.c" "${bdsc_build}/port.c" "${bdsc_port}/portASM.S"
 "${bdsc_root}/drivers/usb/a733_usb_platform.c" "${bdsc_root}/drivers/usb/a733_usb_platform_native.c"
 "${bdsc_root}/arch/aarch64/start.S" "${bdsc_root}/arch/aarch64/vectors.S" "${bdsc_root}/arch/aarch64/cache.S")
list(REMOVE_DUPLICATES bdsc_sources)
set_source_files_properties("${bdsc_root}/drivers/gpu/a733_gpu_resources_native.c"
 "${bdsc_root}/drivers/i2c/a733_i2c.c" PROPERTIES COMPILE_OPTIONS "-iquote;${bdsc_root}/platform/a733")
add_executable(sunstone-a733-board-single-core ${bdsc_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
get_target_property(bdsc_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(bdsc_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(bdsc_definitions sunstone-a733-board COMPILE_DEFINITIONS)
list(FILTER bdsc_definitions EXCLUDE REGEX "^SUNSTONE_A733_SMP_DIAGNOSTIC=")
list(FILTER bdsc_includes EXCLUDE REGEX "(a733-smp-cached|freertos-a733-smp-cached)")
set_property(TARGET sunstone-a733-board-single-core PROPERTY COMPILE_OPTIONS "${bdsc_options}")
target_include_directories(sunstone-a733-board-single-core BEFORE PRIVATE "${bdsc_app}" "${bdsc_port}")
target_include_directories(sunstone-a733-board-single-core PRIVATE ${bdsc_includes} "${bdsc_root}/apps/board")
sunstone_enable_radio_security(sunstone-a733-board-single-core "${SUNSTONE_RSN_DIR}")
target_compile_definitions(sunstone-a733-board-single-core PRIVATE ${bdsc_definitions} GUEST=1 SUNSTONE_BOARD_SINGLE_CORE=1)
set_property(SOURCE "${bdsc_shared}/monitor.c" "${bdsc_shared}/runtime.c" "${bdsc_build}/platform.c" APPEND PROPERTY COMPILE_OPTIONS
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-board-single-core>:-include>"
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-board-single-core>:${bdsc_app}/boot.h>")
# Bind every declared producer input; the frozen closure also records compiler depfiles.
file(GLOB bdsc_headers "${bdsc_app}/*.h" "${bdsc_root}/apps/board/*.h" "${bdsc_root}/include/*.h")
list(FILTER bdsc_sources EXCLUDE REGEX "^\\$<TARGET_OBJECTS:")
set(bdsc_inputs ${bdsc_sources} ${bdsc_headers} "${bdsc_app}/link.ld" "${bdsc_app}/CMakeLists.txt"
 "${bdsc_shared}/platform.patch" "${bdsc_shared}/boot.h" "${bdsc_shared}/cached.h"
 "${bdsc_shared}/FreeRTOSConfig.h" "${bdsc_app}/session.patch" "${bdsc_app}/identity.patch" "${bdsc_app}/admission.patch" "${bdsc_app}/main.patch"
 "${bdsc_port}/portmacro.h" "${bdsc_port}/port.c" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES bdsc_inputs)
set(bdsc_identity "board-single-core-supervised-v1;ROM-USB-mode2;SRE;60s;${bdsc_options};${bdsc_definitions};${CMAKE_C_FLAGS};${CMAKE_ASM_FLAGS}")
foreach(input IN LISTS bdsc_inputs)
 file(SHA256 "${input}" bdsc_hash)
 string(APPEND bdsc_identity "${bdsc_hash}")
endforeach()
string(SHA256 bdsc_build_id "${bdsc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," bdsc_id_bytes "${bdsc_build_id}")
file(WRITE "${bdsc_build}/build-id.c.tmp" "const unsigned char a733_smp_build_id[32]={${bdsc_id_bytes}};\n")
configure_file("${bdsc_build}/build-id.c.tmp" "${bdsc_build}/build-id.c" COPYONLY)
file(WRITE "${bdsc_build}/build-id.txt" "${bdsc_build_id}\n")
string(REPLACE ";" "\n" bdsc_input_lines "${bdsc_inputs}")
file(WRITE "${bdsc_build}/identity-inputs.txt" "${bdsc_input_lines}\n")
target_sources(sunstone-a733-board-single-core PRIVATE "${bdsc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${bdsc_inputs})
target_link_options(sunstone-a733-board-single-core PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--undefined=board_single_core_geometry -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${bdsc_app}/link.ld" "-Wl,-Map,${bdsc_build}/image.map")
set_target_properties(sunstone-a733-board-single-core PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${bdsc_app}/link.ld")
set(bdsc_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@bdsc_build@/manifest.json" "{\"target\":\"sunstone-a733-board-single-core\",\"execution_profile\":\"board-single-core-supervised-v1\",\"workload\":\"board-usb-radio\",\"build_id\":\"@bdsc_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${bdsc_manifest_script}" bdsc_manifest_script @ONLY)
file(WRITE "${bdsc_build}/finish-manifest.cmake" "${bdsc_manifest_script}")
add_custom_command(TARGET sunstone-a733-board-single-core POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-board-single-core>" -P "${bdsc_build}/finish-manifest.cmake")
