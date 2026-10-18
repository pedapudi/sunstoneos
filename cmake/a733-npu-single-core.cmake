# Fixed trusted convolution on the boot CPU; no secondary startup is linked.
set(npsc_root "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(npsc_root "${npsc_root}" ABSOLUTE)
set(npsc_app "${npsc_root}/apps/npu-convolution-single-core")
set(npsc_shared "${npsc_root}/apps/storage-read-single-core")
set(npsc_build "${CMAKE_CURRENT_BINARY_DIR}/npu-single-core")
set(npsc_port "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
file(MAKE_DIRECTORY "${npsc_build}")
get_target_property(npsc_sources a733-npu-convolution-supervised SOURCES)
list(FILTER npsc_sources EXCLUDE REGEX "(/platform/a733-smp-cached/|/platform/a733/cpu_|/freertos-a733-smp-cached/|/a733-npu-convolution-supervised/(boot|continue|port|build-id|usb_service)\\.|/a733-smp-observe/(usb\\.c|service\\.S))")
list(APPEND npsc_sources "${npsc_root}/platform/a733/cpu_coherence.c"
 "${npsc_root}/platform/a733/cpu_coherence.S" "${npsc_root}/platform/a733/cpu_start.c"
 "${npsc_root}/platform/a733-smp-cached/memory.c")
# Producer scope is local to its subdirectory, so use the target source path.
get_target_property(npsc_original a733-npu-convolution-supervised SOURCES)
foreach(src IN LISTS npsc_original)
 if(src MATCHES "/a733-npu-convolution-supervised/usb_service\\.c$")
  apply_build_patch("${npsc_build}/usb_service.c" "${src}" "${npsc_app}/usb-service.patch")
 endif()
endforeach()
apply_build_patch("${npsc_build}/port.c" "${npsc_port}/port.c" "${npsc_root}/patches/freertos-aarch64-clang.patch")
apply_build_patch("${npsc_build}/platform.c" "${npsc_root}/platform/a733/platform.c" "${npsc_shared}/platform.patch")
list(APPEND npsc_sources "${npsc_shared}/start.S" "${npsc_shared}/monitor.c" "${npsc_shared}/runtime.c"
 "${npsc_build}/platform.c" "${npsc_build}/port.c" "${npsc_port}/portASM.S"
 "${npsc_build}/usb_service.c" "${npsc_app}/usb.c"
 "${npsc_root}/drivers/usb/a733_usb_platform.c" "${npsc_root}/drivers/usb/a733_usb_platform_native.c"
 "${npsc_root}/arch/aarch64/start.S" "${npsc_root}/arch/aarch64/vectors.S" "${npsc_root}/arch/aarch64/cache.S")
list(REMOVE_DUPLICATES npsc_sources)
set_source_files_properties("${npsc_root}/drivers/npu/npu_convolution.c" PROPERTIES
 COMPILE_DEFINITIONS "a733_npu_native_operations=a733_npu_application_operations")
set_source_files_properties("${npsc_root}/drivers/gpu/a733_gpu_resources_native.c"
 "${npsc_root}/drivers/i2c/a733_i2c.c" PROPERTIES COMPILE_OPTIONS "-iquote;${npsc_root}/platform/a733")
add_executable(a733-npu-convolution-single-core ${npsc_sources})
get_target_property(npsc_options a733-npu-convolution-supervised COMPILE_OPTIONS)
get_target_property(npsc_includes a733-npu-convolution-supervised INCLUDE_DIRECTORIES)
get_target_property(npsc_definitions a733-npu-convolution-supervised COMPILE_DEFINITIONS)
list(FILTER npsc_definitions EXCLUDE REGEX "^SUNSTONE_A733_SMP_DIAGNOSTIC=")
list(FILTER npsc_includes EXCLUDE REGEX "(a733-smp-cached|freertos-a733-smp-cached)")
set_property(TARGET a733-npu-convolution-single-core PROPERTY COMPILE_OPTIONS "${npsc_options}")
target_include_directories(a733-npu-convolution-single-core BEFORE PRIVATE "${npsc_app}" "${npsc_port}")
target_include_directories(a733-npu-convolution-single-core PRIVATE ${npsc_includes} "${npsc_root}/apps/npu-convolution")
target_compile_definitions(a733-npu-convolution-single-core PRIVATE ${npsc_definitions} GUEST=1 SUNSTONE_NPU_SINGLE_CORE=1)
set_property(SOURCE "${npsc_shared}/monitor.c" "${npsc_shared}/runtime.c" "${npsc_build}/platform.c" APPEND PROPERTY COMPILE_OPTIONS
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,a733-npu-convolution-single-core>:-include>"
 "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,a733-npu-convolution-single-core>:${npsc_app}/boot.h>")
# Bind every declared producer input; the frozen closure also records compiler depfiles.
file(GLOB npsc_headers "${npsc_app}/*.h" "${npsc_root}/apps/npu-convolution/*.h" "${npsc_root}/include/*.h")
set(npsc_inputs ${npsc_sources} ${npsc_headers} "${npsc_app}/link.ld" "${npsc_app}/CMakeLists.txt"
 "${npsc_shared}/platform.patch" "${npsc_shared}/boot.h" "${npsc_shared}/cached.h"
 "${npsc_shared}/FreeRTOSConfig.h" "${npsc_app}/usb-service.patch"
 "${npsc_port}/portmacro.h" "${npsc_port}/port.c" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES npsc_inputs)
set(npsc_identity "npu-single-core-supervised-v1;ROM-USB-mode2;SRE;20s;${npsc_options};${npsc_definitions};${CMAKE_C_FLAGS};${CMAKE_ASM_FLAGS}")
foreach(input IN LISTS npsc_inputs)
 file(SHA256 "${input}" npsc_hash)
 string(APPEND npsc_identity "${npsc_hash}")
endforeach()
string(SHA256 npsc_build_id "${npsc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," npsc_id_bytes "${npsc_build_id}")
file(WRITE "${npsc_build}/build-id.c.tmp" "const unsigned char a733_smp_build_id[32]={${npsc_id_bytes}};\n")
configure_file("${npsc_build}/build-id.c.tmp" "${npsc_build}/build-id.c" COPYONLY)
file(WRITE "${npsc_build}/build-id.txt" "${npsc_build_id}\n")
string(REPLACE ";" "\n" npsc_input_lines "${npsc_inputs}")
file(WRITE "${npsc_build}/identity-inputs.txt" "${npsc_input_lines}\n")
target_sources(a733-npu-convolution-single-core PRIVATE "${npsc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${npsc_inputs})
target_link_options(a733-npu-convolution-single-core PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__device_pool_size=2097152 -Wl,--undefined=npu_single_core_geometry
 -Wl,--undefined=a733_npu_session_layout "-Wl,-T,${npsc_app}/link.ld" "-Wl,-Map,${npsc_build}/image.map")
set_target_properties(a733-npu-convolution-single-core PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${npsc_app}/link.ld")
set(npsc_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@npsc_build@/manifest.json" "{\"target\":\"a733-npu-convolution-single-core\",\"execution_profile\":\"npu-single-core-supervised-v1\",\"workload\":\"trusted-convolution-repeat\",\"build_id\":\"@npsc_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${npsc_manifest_script}" npsc_manifest_script @ONLY)
file(WRITE "${npsc_build}/finish-manifest.cmake" "${npsc_manifest_script}")
add_custom_command(TARGET a733-npu-convolution-single-core POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:a733-npu-convolution-single-core>" -P "${npsc_build}/finish-manifest.cmake")
