# Reuse the GPU provider closure and one cached architecture image. No USB owner.
set(SUNSTONE_GPU_SUPERVISED ON CACHE BOOL "Supervised GPU admission" FORCE)
set(SUNSTONE_GPU_RUNTIME_TEXTURE ON CACHE BOOL "Indexed texture support" FORCE)
set(SUNSTONE_GPU_COMPUTE_APPLICATION OFF CACHE BOOL "Fixed compute application" FORCE)
if(NOT TARGET sunstone-a733-gpu-render)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-gpu-application.cmake")
endif()
if(NOT TARGET sunstone-a733-gpu-render)
 message(FATAL_ERROR "GPU display requires the pinned GPU firmware")
endif()
set(gd_root "${SUNSTONE_SOURCE_ROOT}")
set(gd_build "${CMAKE_CURRENT_BINARY_DIR}/gpu-display-application")
file(MAKE_DIRECTORY "${gd_build}")
get_target_property(gd_sources sunstone-a733-gpu-render SOURCES)
foreach(source IN ITEMS apps/gpu-render/main.c apps/gpu-render/admission.c apps/gpu-render/session.c
 apps/gpu-render/usb.c apps/gpu-render/texture.c platform/a733/native_usb.c
 platform/a733/maintenance_watchdog.c drivers/usb/a733_usb_platform.c drivers/usb/a733_usb_platform_native.c)
 list(REMOVE_ITEM gd_sources "${gd_root}/${source}")
endforeach()
list(REMOVE_ITEM gd_sources "${gpu_app_boot}" "${gpu_app_build}/platform.c" "${gpu_app_build}/build-id.c"
 "${storage_build}/tinyusb/dcd_sunxi_musb.c" "${TINYUSB_DIR}/src/tusb.c"
 "${TINYUSB_DIR}/src/common/tusb_fifo.c" "${TINYUSB_DIR}/src/device/usbd.c"
 "${TINYUSB_DIR}/src/class/vendor/vendor_device.c")
foreach(pair "boot.c|boot.patch" "platform.c|platform.patch" "link-devices.ld|link.patch")
 string(REPLACE "|" ";" parts "${pair}")
 list(GET parts 0 source)
 list(GET parts 1 patch)
 apply_build_patch("${gd_build}/${source}" "${gd_root}/platform/a733-smp-cached/${source}"
  "${gd_root}/apps/gpu-display/${patch}")
endforeach()
list(APPEND gd_sources "${gd_build}/boot.c" "${gd_build}/platform.c")
foreach(source IN ITEMS apps/gpu-display/admission.c apps/gpu-display/snapshot.c apps/gpu-display/privilege.c
 drivers/display/a733_hdmi.c drivers/display/a733_hdmi_phy.c drivers/display/a733_tcon_tv.c
 platform/a733/hdmi_native.c platform/a733/display_native_io.c drivers/iommu/a733_iommu.c
 drivers/clock/a733_media_clock.c drivers/clock/a733_media_clock_native.c
 drivers/display/a733_display_engine.c drivers/display/a733_tcon_lcd.c
 drivers/display/a733_dsi_video.c drivers/display/a733_dsi_phy.c)
 list(APPEND gd_sources "${gd_root}/${source}")
endforeach()
list(REMOVE_DUPLICATES gd_sources)
add_executable(sunstone-a733-gpu-display EXCLUDE_FROM_ALL ${gd_sources})
foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_OPTIONS)
 get_target_property(value sunstone-a733-gpu-render ${property})
 set_property(TARGET sunstone-a733-gpu-display PROPERTY ${property} "${value}")
endforeach()
include("${gd_root}/apps/gpu-display/CMakeLists.txt")
sunstone_gpu_display_application(sunstone-a733-gpu-display)
target_compile_definitions(sunstone-a733-gpu-display PRIVATE SUNSTONE_SECURE_MONITOR=1
 SUNSTONE_A733_SMP_DIAGNOSTIC=1 SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1
 A733_SMP_CONTRACT_MAGIC=0x4153373347444331ULL A733_SMP_RESULT_MAGIC=0x4153373347445231ULL)
target_link_options(sunstone-a733-gpu-display PRIVATE -nostdlib -static -Wl,--gc-sections
 "-Wl,-T,${gd_build}/link-devices.ld" "-Wl,-Map,${gd_build}/image.map")
set_target_properties(sunstone-a733-gpu-display PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${gd_build}/link-devices.ld")
get_target_property(gd_sources sunstone-a733-gpu-display SOURCES)
file(GLOB gd_headers "${gd_root}/apps/gpu-display/*.h" "${gd_root}/apps/hdmi/*.h")
set(gd_inputs ${gpu_app_inputs} ${gd_sources} ${gd_headers} "${CMAKE_CURRENT_LIST_FILE}"
 "${gd_root}/apps/gpu-display/native/CMakeLists.txt" "${gd_root}/apps/gpu-display/CMakeLists.txt"
 "${gd_root}/apps/gpu-display/boot.patch" "${gd_root}/apps/gpu-display/platform.patch"
 "${gd_root}/apps/gpu-display/link.patch" "${gd_build}/link-devices.ld")
list(REMOVE_DUPLICATES gd_inputs)
get_target_property(gd_options sunstone-a733-gpu-display COMPILE_OPTIONS)
get_target_property(gd_definitions sunstone-a733-gpu-display COMPILE_DEFINITIONS)
get_target_property(gd_links sunstone-a733-gpu-display LINK_OPTIONS)
set(gd_identity "gpu-display-v1;supervised20;copied-RGBA8-to-two-RGB565;TV0-720p60;extended-pools;${gpu_app_cc_hash};${gpu_app_ld_hash};${gd_options};${gd_definitions};${gd_links};${CMAKE_C_FLAGS};${CMAKE_ASM_FLAGS};${CMAKE_BUILD_TYPE}")
foreach(input IN LISTS gd_inputs)
 file(SHA256 "${input}" digest)
 string(APPEND gd_identity "${digest}")
endforeach()
string(SHA256 gd_build_id "${gd_identity}")
string(REGEX REPLACE "(..)" "0x\\1," gd_id_bytes "${gd_build_id}")
file(WRITE "${gd_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${gd_id_bytes}};\n")
file(WRITE "${gd_build}/build-id.txt" "${gd_build_id}\n")
string(REPLACE ";" "\n" gd_input_lines "${gd_inputs}")
file(WRITE "${gd_build}/identity-inputs.txt" "${gd_input_lines}\n")
target_sources(sunstone-a733-gpu-display PRIVATE "${gd_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${gd_inputs})
set(gd_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@gd_build@/manifest.json" "{\"target\":\"sunstone-a733-gpu-display\",\"execution_profile\":\"gpu-display-supervised-v1\",\"workload\":\"copied-two-frame-HDMI-720p60\",\"build_id\":\"@gd_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${gd_manifest_script}" gd_manifest_script @ONLY)
file(WRITE "${gd_build}/finish-manifest.cmake" "${gd_manifest_script}")
add_custom_command(TARGET sunstone-a733-gpu-display POST_BUILD
 COMMAND "${CMAKE_COMMAND}" "-DIMAGE=$<TARGET_FILE:sunstone-a733-gpu-display>"
 -P "${gd_build}/finish-manifest.cmake" VERBATIM)
