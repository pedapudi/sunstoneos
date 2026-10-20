# One retained boot-CPU application with one shared provider owner.
if(NOT TARGET a733-npu-convolution-single-core OR NOT A733_NPU_NN_POOL_GRAPH)
  message(FATAL_ERROR "Accelerators require the complete retained NPU profile")
endif()
set(acc_root "${npsc_root}")
set(acc_build "${CMAKE_CURRENT_BINARY_DIR}/accelerators")
file(MAKE_DIRECTORY "${acc_build}")
set(SUNSTONE_GPU_FIRMWARE "" CACHE FILEPATH "Pinned A733 GPU firmware")
if(NOT SUNSTONE_GPU_FIRMWARE)
  message(FATAL_ERROR "Accelerators require SUNSTONE_GPU_FIRMWARE")
endif()
file(SHA256 "${SUNSTONE_GPU_FIRMWARE}" acc_fw_hash)
if(NOT acc_fw_hash STREQUAL "1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2")
  message(FATAL_ERROR "GPU firmware identity refused")
endif()
get_target_property(acc_sources a733-npu-convolution-single-core SOURCES)
list(REMOVE_ITEM acc_sources "${npsc_build}/build-id.c"
  "${acc_root}/apps/npu-convolution/admission.c"
  "${acc_root}/apps/npu-runtime/main.c"
  "${acc_root}/apps/npu-runtime-large/runtime.c"
  "${acc_root}/apps/npu-runtime-large/usb.c")
set(acc_gpu_sources
  platform/a733/memory_uncached.c
  platform/a733/gpu_reserved.c platform/a733/gpu_compute_reserved.c
  platform/a733/gpu_graphics_memory.c platform/a733/gpu_memory.c
  platform/a733/gpu_vm_memory.c platform/a733/gpu_backend.c
  drivers/gpu/a733_gpu_power.c drivers/gpu/a733_gpu_platform.c
  drivers/gpu/a733_gpu_platform_native.c drivers/gpu/gicv3_spi.c
  drivers/gpu/pvr_compute_mode.c drivers/gpu/pvr_compute_service.c
  drivers/gpu/pvr_compute_package.c drivers/gpu/pvr_compute_job.c
  drivers/gpu/pvr_compute_programs.c drivers/gpu/pvr_compute_context.c
  drivers/gpu/pvr_cccb.c drivers/gpu/pvr_gpu_vm.c drivers/gpu/pvr_render_target.c
  drivers/gpu/pvr_render_programs.c drivers/gpu/pvr_render_draw.c
  drivers/gpu/pvr_render_texture.c drivers/gpu/pvr_render_context.c
  drivers/gpu/pvr_render_job.c drivers/gpu/pvr_render_service.c
  drivers/gpu/pvr_gpu_service.c drivers/gpu/pvr_firmware.c drivers/gpu/pvr_mips.c
  drivers/gpu/pvr_fw_state.c drivers/gpu/pvr_ccb.c drivers/gpu/pvr_gpu_start.c
  drivers/gpu/pvr_gpu_control.c drivers/gpu/pvr_gpu_stop.c
  drivers/gpu/pvr_free_list.c drivers/gpu/pvr_gpu_events.c
  apps/accelerators/admission.c)
list(TRANSFORM acc_gpu_sources PREPEND "${acc_root}/")
list(APPEND acc_sources ${acc_gpu_sources})
list(REMOVE_DUPLICATES acc_sources)
file(WRITE "${acc_build}/firmware.S"
  ".section .rodata.gpu_firmware,\"a\"\n.balign 16\n.global gpu_firmware\ngpu_firmware:\n.incbin \"${SUNSTONE_GPU_FIRMWARE}\"\n.global gpu_firmware_end\ngpu_firmware_end:\n")
add_executable(sunstone-a733-accelerators ${acc_sources} "${acc_build}/firmware.S")
foreach(property IN ITEMS COMPILE_OPTIONS INCLUDE_DIRECTORIES)
  get_target_property(value a733-npu-convolution-single-core ${property})
  set_property(TARGET sunstone-a733-accelerators PROPERTY ${property} "${value}")
endforeach()
get_target_property(acc_definitions a733-npu-convolution-single-core COMPILE_DEFINITIONS)
list(FILTER acc_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
target_compile_definitions(sunstone-a733-accelerators PRIVATE ${acc_definitions}
  SUNSTONE_ACCELERATORS=1 SUNSTONE_GPU_COMPUTE_APPLICATION=1
  SUNSTONE_GPU_PROGRAM_APPLICATION=1 SUNSTONE_GPU_RUNTIME_INPUT=1
  A733_SMP_CONTRACT_MAGIC=0x4153373341434331ULL A733_SMP_RESULT_MAGIC=0x4153373341435231ULL)
sunstone_accelerator_application(sunstone-a733-accelerators)
set_source_files_properties("${acc_root}/drivers/gpu/a733_gpu_platform_native.c"
  PROPERTIES COMPILE_OPTIONS "-iquote${acc_root}/platform/a733")
# The shared monitor/platform must use this target's private boot view.
set_property(SOURCE "${npsc_shared}/monitor.c" "${npsc_shared}/runtime.c"
  "${npsc_build}/platform.c" APPEND PROPERTY COMPILE_OPTIONS
  "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-accelerators>:-include>"
  "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-accelerators>:${acc_root}/apps/accelerators/boot.h>")
target_link_options(sunstone-a733-accelerators PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--defsym=__npu_large_graph=1 -Wl,--defsym=__device_pool_size=4194304
  -Wl,--defsym=__uncached_pool_size=2097152
  -Wl,--undefined=accelerator_single_core_geometry
  -Wl,--undefined=accelerator_session_layout
  "-Wl,-T,${npsc_app}/link.ld" "-Wl,-Map,${acc_build}/image.map")
set_target_properties(sunstone-a733-accelerators PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${npsc_app}/link.ld")
get_target_property(acc_sources sunstone-a733-accelerators SOURCES)
file(GLOB acc_headers "${acc_root}/apps/accelerators/*.h")
set(acc_inputs ${npsc_inputs} ${acc_sources} ${acc_headers}
  "${acc_root}/apps/accelerators/CMakeLists.txt" "${CMAKE_CURRENT_LIST_FILE}"
  "${npsc_app}/link.ld" "${SUNSTONE_GPU_FIRMWARE}")
list(REMOVE_DUPLICATES acc_inputs)
get_target_property(acc_options sunstone-a733-accelerators COMPILE_OPTIONS)
get_target_property(acc_definitions sunstone-a733-accelerators COMPILE_DEFINITIONS)
get_target_property(acc_links sunstone-a733-accelerators LINK_OPTIONS)
file(SHA256 "${CMAKE_C_COMPILER}" acc_cc_hash)
file(SHA256 "${SUNSTONE_LLD}" acc_ld_hash)
set(acc_identity "combined-gpu-nn;retained-single-core;20s;WB416-41a;NC41c-41e;${acc_cc_hash};${acc_ld_hash};${acc_options};${acc_definitions};${acc_links};${CMAKE_C_FLAGS};${CMAKE_ASM_FLAGS}")
foreach(input IN LISTS acc_inputs)
  file(SHA256 "${input}" hash)
  string(APPEND acc_identity "${hash}")
endforeach()
string(SHA256 acc_build_id "${acc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," acc_id_bytes "${acc_build_id}")
file(WRITE "${acc_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${acc_id_bytes}};\n")
file(WRITE "${acc_build}/build-id.txt" "${acc_build_id}\n")
string(REPLACE ";" "\n" acc_input_lines "${acc_inputs}")
file(WRITE "${acc_build}/identity-inputs.txt" "${acc_input_lines}\n")
target_sources(sunstone-a733-accelerators PRIVATE "${acc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${acc_inputs})
