# Supervised PWM fan and optional LED service on the boot CPU.
set(pwsc_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(pwsc_root "${pwsc_root}" ABSOLUTE)
set(pwsc_app "${pwsc_root}/apps/pwm-single-core")
set(pwsc_shared "${pwsc_root}/apps/storage-read-single-core")
set(pwsc_build "${CMAKE_CURRENT_BINARY_DIR}/pwm-single-core")
set(pwsc_port "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
file(MAKE_DIRECTORY "${pwsc_build}")
get_target_property(pwsc_sources sunstone-a733-pwm SOURCES)
list(FILTER pwsc_sources EXCLUDE REGEX "(/pwm-application/(boot|platform|build-id)\\.c$|/storage-maintenance/(platform|port|continue)\\.(c|S)$|/platform/a733-smp-cached/|/platform/a733/cpu_|/freertos-a733-smp-cached/|/sunstone-a733-board/(boot|continue|port|build-id|usb_service)\\.|/a733-smp-observe/(usb\\.c|service\\.S))")
list(APPEND pwsc_sources "${pwsc_root}/platform/a733/cpu_coherence.c"
 "${pwsc_root}/platform/a733/cpu_coherence.S" "${pwsc_root}/platform/a733/cpu_start.c"
 "${pwsc_root}/platform/a733-smp-cached/memory.c")
foreach(pair IN ITEMS "session;apps/board/session.c" "snapshot;apps/pwm/snapshot.c" "main;apps/board/main.c" "admission;apps/pwm/admission.c")
 list(GET pair 0 name)
 list(GET pair 1 path)
 list(REMOVE_ITEM pwsc_sources "${pwsc_root}/${path}")
 apply_build_patch("${pwsc_build}/${name}.c" "${pwsc_root}/${path}" "${pwsc_app}/${name}.patch")
 list(APPEND pwsc_sources "${pwsc_build}/${name}.c")
endforeach()
apply_build_patch("${pwsc_build}/port.c" "${pwsc_port}/port.c" "${pwsc_root}/patches/freertos-aarch64-clang.patch")
apply_build_patch("${pwsc_build}/board-admission.c" "${pwsc_root}/apps/board/admission.c" "${pwsc_app}/board-admission.patch")
apply_build_patch("${pwsc_build}/platform-base.c" "${pwsc_root}/platform/a733/platform.c" "${pwsc_shared}/platform.patch")
apply_build_patch("${pwsc_build}/platform.c" "${pwsc_build}/platform-base.c" "${pwsc_app}/platform.patch")
foreach(name IN ITEMS monitor runtime)
 apply_build_patch("${pwsc_build}/${name}.c" "${pwsc_shared}/${name}.c" "${pwsc_app}/${name}.patch")
endforeach()
list(APPEND pwsc_sources "${pwsc_shared}/start.S" "${pwsc_build}/monitor.c" "${pwsc_build}/runtime.c"
 "${pwsc_build}/platform.c" "${pwsc_build}/port.c" "${pwsc_port}/portASM.S"
 "${pwsc_root}/drivers/usb/a733_usb_platform.c" "${pwsc_root}/drivers/usb/a733_usb_platform_native.c"
 "${pwsc_root}/arch/aarch64/start.S" "${pwsc_root}/arch/aarch64/vectors.S" "${pwsc_root}/arch/aarch64/cache.S")
list(REMOVE_DUPLICATES pwsc_sources)
set_source_files_properties("${pwsc_root}/drivers/gpu/a733_gpu_resources_native.c"
 "${pwsc_root}/drivers/i2c/a733_i2c.c" PROPERTIES COMPILE_OPTIONS "-iquote;${pwsc_root}/platform/a733")
add_executable(sunstone-a733-pwm-single-core ${pwsc_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
get_target_property(pwsc_options sunstone-a733-pwm COMPILE_OPTIONS)
get_target_property(pwsc_includes sunstone-a733-pwm INCLUDE_DIRECTORIES)
get_target_property(pwsc_definitions sunstone-a733-pwm COMPILE_DEFINITIONS)
list(FILTER pwsc_definitions EXCLUDE REGEX "^SUNSTONE_A733_SMP_DIAGNOSTIC=")
list(FILTER pwsc_includes EXCLUDE REGEX "(a733-smp-cached|freertos-a733-smp-cached)")
set_property(TARGET sunstone-a733-pwm-single-core PROPERTY COMPILE_OPTIONS "${pwsc_options}")
target_include_directories(sunstone-a733-pwm-single-core BEFORE PRIVATE "${pwsc_app}" "${pwsc_build}" "${pwsc_port}")
target_include_directories(sunstone-a733-pwm-single-core PRIVATE ${pwsc_includes} "${pwsc_root}/apps/pwm")
sunstone_enable_radio_security(sunstone-a733-pwm-single-core "${SUNSTONE_RSN_DIR}")
target_compile_definitions(sunstone-a733-pwm-single-core PRIVATE ${pwsc_definitions} GUEST=1 SUNSTONE_PWM_SINGLE_CORE=1 SUNSTONE_SD_SINGLE_CORE=1)
set_property(SOURCE "${pwsc_build}/monitor.c" "${pwsc_build}/runtime.c" "${pwsc_build}/platform.c" APPEND PROPERTY COMPILE_OPTIONS
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-pwm-single-core>:-include>"
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-pwm-single-core>:${pwsc_app}/boot.h>")
# Bind every declared producer input; the frozen closure also records compiler depfiles.
file(GLOB pwsc_headers "${pwsc_app}/*.h" "${pwsc_root}/apps/pwm/*.h" "${pwsc_root}/include/*.h")
list(FILTER pwsc_sources EXCLUDE REGEX "^\\$<TARGET_OBJECTS:")
set(pwsc_inputs ${pwsc_sources} ${pwsc_headers} "${pwsc_build}/board-admission.c" "${pwsc_app}/link.ld" "${pwsc_app}/CMakeLists.txt"
 "${pwsc_shared}/platform.patch" "${pwsc_shared}/boot.h" "${pwsc_shared}/cached.h"
 "${pwsc_shared}/FreeRTOSConfig.h" "${pwsc_app}/session.patch" "${pwsc_app}/snapshot.patch" "${pwsc_app}/main.patch"
 "${pwsc_app}/monitor.patch" "${pwsc_app}/runtime.patch" "${pwsc_app}/platform.patch"
 "${pwsc_app}/admission.patch" "${pwsc_app}/board-admission.patch"
 "${pwsc_port}/portmacro.h" "${pwsc_port}/port.c" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES pwsc_inputs)
set(pwsc_identity "pwm-single-core-supervised-v1;ROM-USB-mode2;SRE;60s;${pwsc_options};${pwsc_definitions};${CMAKE_C_FLAGS};${CMAKE_ASM_FLAGS}")
foreach(input IN LISTS pwsc_inputs)
 file(SHA256 "${input}" pwsc_hash)
 string(APPEND pwsc_identity "${pwsc_hash}")
endforeach()
string(SHA256 pwsc_build_id "${pwsc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," pwsc_id_bytes "${pwsc_build_id}")
file(WRITE "${pwsc_build}/build-id.c.tmp" "const unsigned char a733_smp_build_id[32]={${pwsc_id_bytes}};\n")
configure_file("${pwsc_build}/build-id.c.tmp" "${pwsc_build}/build-id.c" COPYONLY)
file(WRITE "${pwsc_build}/build-id.txt" "${pwsc_build_id}\n")
string(REPLACE ";" "\n" pwsc_input_lines "${pwsc_inputs}")
file(WRITE "${pwsc_build}/identity-inputs.txt" "${pwsc_input_lines}\n")
target_sources(sunstone-a733-pwm-single-core PRIVATE "${pwsc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${pwsc_inputs})
target_link_options(sunstone-a733-pwm-single-core PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--undefined=pwm_single_core_geometry
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${pwsc_app}/link.ld" "-Wl,-Map,${pwsc_build}/image.map")
set_target_properties(sunstone-a733-pwm-single-core PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${pwsc_app}/link.ld")
set(pwsc_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@pwsc_build@/manifest.json" "{\"target\":\"sunstone-a733-pwm-single-core\",\"execution_profile\":\"pwm-single-core-supervised-v1\",\"workload\":\"pwm-fan-led\",\"build_id\":\"@pwsc_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${pwsc_manifest_script}" pwsc_manifest_script @ONLY)
file(WRITE "${pwsc_build}/finish-manifest.cmake" "${pwsc_manifest_script}")
add_custom_command(TARGET sunstone-a733-pwm-single-core POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-pwm-single-core>" -P "${pwsc_build}/finish-manifest.cmake")
