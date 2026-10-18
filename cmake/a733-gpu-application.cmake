# One fixed render client, one native shared provider, and lifetime retention.
if(NOT DEFINED SUNSTONE_SOURCE_ROOT)
  set(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
if(NOT TARGET sunstone-a733-storage-maintenance)
  include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-storage-maintenance.cmake")
endif()
option(SUNSTONE_GPU_COMPUTE_APPLICATION "Run repeated native numerical GPU jobs" OFF)
option(SUNSTONE_GPU_SUPERVISED "Use reviewed one-attempt supervised RAM-only GPU admission" OFF)
set(SUNSTONE_GPU_FIRMWARE "" CACHE FILEPATH "Pinned A733 GPU firmware")
if(NOT SUNSTONE_GPU_FIRMWARE)
  message(STATUS "Native GPU application requires SUNSTONE_GPU_FIRMWARE")
  return()
endif()
file(SHA256 "${SUNSTONE_GPU_FIRMWARE}" gpu_app_fw_hash)
if(NOT gpu_app_fw_hash STREQUAL "1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2")
  message(FATAL_ERROR "Native GPU application firmware identity refused")
endif()
set(gpu_app_root "${SUNSTONE_SOURCE_ROOT}")
set(gpu_app_build "${CMAKE_CURRENT_BINARY_DIR}/gpu-application")
file(MAKE_DIRECTORY "${gpu_app_build}")
get_target_property(gpu_app_sources sunstone-a733-storage-maintenance SOURCES)
foreach(source IN ITEMS platform/a733-storage-maintenance/main.c platform/a733-storage-maintenance/session.c
  platform/a733-storage-maintenance/admission.c platform/a733-storage-update/bootstrap.c
  platform/a733-storage-update/worker.c platform/a733-storage-update/usb.c
  drivers/storage/a733_storage_update.c drivers/storage/storage_update_usb.c
  drivers/storage/a733_mmc_platform.c drivers/storage/a733_mmc_platform_native.c drivers/storage/sunxi_mmc.c
  drivers/watchdog/a733_maintenance_watchdog.c)
  list(REMOVE_ITEM gpu_app_sources "${gpu_app_root}/${source}")
endforeach()
list(REMOVE_ITEM gpu_app_sources "${storage_build}/boot.c" "${storage_build}/platform.c" "${storage_build}/build-id.c")
foreach(pair "boot.c|boot.patch" "platform.c|platform.patch")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 source)
  list(GET parts 1 patch)
  apply_build_patch("${gpu_app_build}/${source}" "${gpu_app_root}/platform/a733-smp-cached/${source}"
    "${gpu_app_root}/apps/gpu-render/${patch}")
endforeach()
set(gpu_app_boot "${gpu_app_build}/boot.c")
if(SUNSTONE_GPU_SUPERVISED)
  apply_build_patch("${gpu_app_build}/boot-supervised.c" "${gpu_app_boot}"
    "${gpu_app_root}/platform/a733-smp-supervised/boot.patch")
  set(gpu_app_boot "${gpu_app_build}/boot-supervised.c")
endif()
apply_build_patch("${gpu_app_build}/link.ld" "${gpu_app_root}/platform/a733-smp-cached/link-devices.ld"
  "${gpu_app_root}/apps/gpu-render/link.patch")
file(WRITE "${gpu_app_build}/firmware.S" ".section .rodata.gpu_firmware,\"a\"\n.balign 16\n.global gpu_firmware\ngpu_firmware:\n.incbin \"${SUNSTONE_GPU_FIRMWARE}\"\n.global gpu_firmware_end\ngpu_firmware_end:\n")
set(gpu_app_extra
  apps/gpu-render/main.c apps/gpu-render/admission.c apps/gpu-render/session.c apps/gpu-render/usb.c
  platform/a733/device_memory_layout.c platform/a733/memory_reservations.c platform/a733/memory_uncached.c
  runtime/device_memory_pool.c platform/a733/gpu_reserved.c platform/a733/gpu_compute_reserved.c
  platform/a733/gpu_graphics_memory.c platform/a733/gpu_memory.c platform/a733/gpu_vm_memory.c platform/a733/gpu_backend.c
  drivers/gpu/a733_gpu_power.c drivers/gpu/a733_gpu_platform.c drivers/gpu/a733_gpu_platform_native.c
  drivers/gpu/gicv3_spi.c drivers/gpu/pvr_compute_mode.c drivers/gpu/pvr_compute_service.c
  drivers/gpu/pvr_compute_job.c drivers/gpu/pvr_compute_programs.c drivers/gpu/pvr_compute_context.c
  drivers/gpu/pvr_cccb.c drivers/gpu/pvr_gpu_vm.c drivers/gpu/pvr_render_target.c
  drivers/gpu/pvr_render_programs.c drivers/gpu/pvr_render_context.c drivers/gpu/pvr_render_job.c
  drivers/gpu/pvr_render_service.c drivers/gpu/pvr_gpu_service.c drivers/gpu/pvr_firmware.c drivers/gpu/pvr_mips.c
  drivers/gpu/pvr_fw_state.c drivers/gpu/pvr_ccb.c drivers/gpu/pvr_gpu_start.c drivers/gpu/pvr_gpu_control.c
  drivers/gpu/pvr_gpu_stop.c drivers/gpu/pvr_free_list.c drivers/gpu/pvr_gpu_events.c
)
set_source_files_properties("${gpu_app_root}/drivers/gpu/a733_gpu_platform_native.c" PROPERTIES
  COMPILE_OPTIONS "-iquote${gpu_app_root}/platform/a733")
list(TRANSFORM gpu_app_extra PREPEND "${gpu_app_root}/")
list(APPEND gpu_app_sources ${gpu_app_extra} "${gpu_app_boot}" "${gpu_app_build}/platform.c" "${gpu_app_build}/firmware.S")
list(REMOVE_DUPLICATES gpu_app_sources)
add_executable(sunstone-a733-gpu-render EXCLUDE_FROM_ALL ${gpu_app_sources})
foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_OPTIONS)
  get_target_property(value sunstone-a733-storage-maintenance ${property})
  set_property(TARGET sunstone-a733-gpu-render PROPERTY ${property} "${value}")
endforeach()
set(gpu_app_workload "fixed-clear")
if(SUNSTONE_GPU_COMPUTE_APPLICATION)
  set(gpu_app_workload "fixed-arithmetic-repeat")
  target_compile_definitions(sunstone-a733-gpu-render PRIVATE SUNSTONE_GPU_COMPUTE_APPLICATION=1)
endif()
target_include_directories(sunstone-a733-gpu-render BEFORE PRIVATE "${gpu_app_root}/apps/gpu-render")
target_compile_definitions(sunstone-a733-gpu-render PRIVATE SUNSTONE_SECURE_MONITOR=1
  SUNSTONE_A733_SMP_DIAGNOSTIC=1 SUNSTONE_A733_USB=1 SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1
)
if(SUNSTONE_GPU_SUPERVISED)
  target_compile_definitions(sunstone-a733-gpu-render PRIVATE SUNSTONE_GPU_SUPERVISED=1
    A733_SMP_CONTRACT_MAGIC=0x4153373347534332ULL A733_SMP_RESULT_MAGIC=0x4153373347535232ULL)
else()
  target_compile_definitions(sunstone-a733-gpu-render PRIVATE
    A733_SMP_CONTRACT_MAGIC=0x4153373347504331ULL A733_SMP_RESULT_MAGIC=0x4153373347505231ULL)
endif()
target_link_options(sunstone-a733-gpu-render PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${gpu_app_build}/link.ld" "-Wl,-Map,${gpu_app_build}/image.map")
set_target_properties(sunstone-a733-gpu-render PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${gpu_app_build}/link.ld")
# Include all shared boot dependencies and the application input closure. Keep
# final compiler dependency files as evidence; this identity is not qualification.
file(GLOB gpu_app_headers "${gpu_app_root}/apps/gpu-render/*.h")
set(gpu_app_inputs ${storage_inputs} ${gpu_app_sources} ${gpu_app_headers} "${SUNSTONE_GPU_FIRMWARE}"
  "${CMAKE_CURRENT_LIST_FILE}" "${gpu_app_root}/apps/gpu-render/boot.patch"
  "${gpu_app_root}/apps/gpu-render/platform.patch" "${gpu_app_root}/apps/gpu-render/link.patch"
  "${gpu_app_root}/platform/a733-smp-supervised/boot.patch"
  "${gpu_app_root}/platform/a733-smp-cached/link-devices.ld" "${gpu_app_build}/link.ld")
list(REMOVE_DUPLICATES gpu_app_inputs)
get_target_property(gpu_app_options sunstone-a733-gpu-render COMPILE_OPTIONS)
get_target_property(gpu_app_definitions sunstone-a733-gpu-render COMPILE_DEFINITIONS)
get_target_property(gpu_app_links sunstone-a733-gpu-render LINK_OPTIONS)
file(SHA256 "${CMAKE_C_COMPILER}" gpu_app_cc_hash)
file(SHA256 "${SUNSTONE_LLD}" gpu_app_ld_hash)
set(gpu_app_identity "native-gpu-application;${SUNSTONE_GPU_COMPUTE_APPLICATION};${gpu_app_cc_hash};${gpu_app_ld_hash};${gpu_app_options};${gpu_app_definitions};${gpu_app_links};${CMAKE_C_FLAGS};${CMAKE_ASM_FLAGS};${CMAKE_BUILD_TYPE}")
foreach(input IN LISTS gpu_app_inputs)
  file(SHA256 "${input}" digest)
  string(APPEND gpu_app_identity "${digest}")
endforeach()
string(SHA256 gpu_app_build_id "${gpu_app_identity}")
string(REGEX REPLACE "(..)" "0x\\1," gpu_app_id_bytes "${gpu_app_build_id}")
file(WRITE "${gpu_app_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${gpu_app_id_bytes}};\n")
file(WRITE "${gpu_app_build}/build-id.txt" "${gpu_app_build_id}\n")
string(REPLACE ";" "\n" gpu_app_input_lines "${gpu_app_inputs}")
file(WRITE "${gpu_app_build}/identity-inputs.txt" "${gpu_app_input_lines}\n")
target_sources(sunstone-a733-gpu-render PRIVATE "${gpu_app_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${gpu_app_inputs})

if(SUNSTONE_GPU_SUPERVISED)
  set(gpu_app_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@gpu_app_build@/manifest.json" "{\"target\":\"sunstone-a733-gpu-render\",\"execution_profile\":\"gpu-supervised-v2\",\"workload\":\"@gpu_app_workload@\",\"build_id\":\"@gpu_app_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
  string(CONFIGURE "${gpu_app_manifest_script}" gpu_app_manifest_script @ONLY)
  file(WRITE "${gpu_app_build}/finish-manifest.cmake" "${gpu_app_manifest_script}")
  add_custom_command(TARGET sunstone-a733-gpu-render POST_BUILD
    COMMAND "${CMAKE_COMMAND}" "-DIMAGE=$<TARGET_FILE:sunstone-a733-gpu-render>"
      -P "${gpu_app_build}/finish-manifest.cmake" VERBATIM)
endif()
