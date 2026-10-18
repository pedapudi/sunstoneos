# Supervised TV0 hardware colorbar. No DE, framebuffer, USB, storage or DMA.
if(NOT TARGET sunstone-a733-storage-maintenance)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-storage-maintenance.cmake")
endif()
set(hdmi_root "${SUNSTONE_SOURCE_ROOT}")
set(hdmi_build "${CMAKE_CURRENT_BINARY_DIR}/hdmi-application")
file(MAKE_DIRECTORY "${hdmi_build}")
get_target_property(hdmi_sources sunstone-a733-storage-maintenance SOURCES)
foreach(source IN ITEMS
 platform/a733-storage-maintenance/main.c platform/a733-storage-maintenance/session.c
 platform/a733-storage-maintenance/admission.c platform/a733-storage-update/bootstrap.c
 platform/a733-storage-update/worker.c platform/a733-storage-update/usb.c
 drivers/storage/a733_storage_update.c drivers/storage/storage_update_usb.c
 drivers/storage/a733_mmc_platform.c drivers/storage/a733_mmc_platform_native.c
 drivers/storage/sunxi_mmc.c drivers/watchdog/a733_maintenance_watchdog.c
 platform/a733/maintenance_watchdog.c platform/a733/native_usb.c
 drivers/usb/a733_usb_platform.c drivers/usb/a733_usb_platform_native.c)
 list(REMOVE_ITEM hdmi_sources "${hdmi_root}/${source}")
endforeach()
list(REMOVE_ITEM hdmi_sources "${storage_build}/build-id.c" "${storage_build}/boot.c"
 "${storage_build}/tinyusb/dcd_sunxi_musb.c" "${TINYUSB_DIR}/src/tusb.c"
 "${TINYUSB_DIR}/src/common/tusb_fifo.c" "${TINYUSB_DIR}/src/device/usbd.c"
 "${TINYUSB_DIR}/src/class/vendor/vendor_device.c")
apply_build_patch("${hdmi_build}/boot.c" "${storage_build}/boot.c" "${hdmi_root}/apps/hdmi/boot.patch")
list(APPEND hdmi_sources "${hdmi_build}/boot.c")
foreach(source IN ITEMS apps/hdmi/main.c apps/hdmi/admission.c apps/hdmi/output.c
 drivers/display/a733_hdmi.c drivers/display/a733_hdmi_phy.c drivers/display/a733_tcon_tv.c
 platform/a733/hdmi_native.c)
 list(APPEND hdmi_sources "${hdmi_root}/${source}")
endforeach()
add_executable(sunstone-a733-hdmi EXCLUDE_FROM_ALL ${hdmi_sources})
get_target_property(hdmi_options sunstone-a733-storage-maintenance COMPILE_OPTIONS)
set_property(TARGET sunstone-a733-hdmi PROPERTY COMPILE_OPTIONS "${hdmi_options}")
target_include_directories(sunstone-a733-hdmi BEFORE PRIVATE "${hdmi_root}/apps/hdmi"
 "${hdmi_root}/platform/a733-smp-cached" "${hdmi_root}/include" "${hdmi_root}/runtime/include"
 "${FREERTOS_DIR}/include" "${storage_base}")
target_compile_definitions(sunstone-a733-hdmi PRIVATE SUNSTONE_SECURE_MONITOR=1
 SUNSTONE_A733_SMP_DIAGNOSTIC=1 A733_SMP_CONTRACT_MAGIC=0x4153373348444331ULL
 A733_SMP_RESULT_MAGIC=0x4153373348445231ULL)
file(GLOB hdmi_headers "${hdmi_root}/apps/hdmi/*.h")
set(hdmi_inputs ${hdmi_sources} ${hdmi_headers} ${storage_inputs}
 "${CMAKE_CURRENT_LIST_FILE}" "${hdmi_root}/apps/hdmi/CMakeLists.txt"
 "${hdmi_root}/apps/hdmi/boot.patch" "${hdmi_root}/apps/hdmi/prepare.py"
 "${hdmi_root}/platform/a733-smp-supervised/launch.c"
 "${hdmi_root}/platform/a733-smp-observe/launch.S" "${hdmi_root}/platform/a733-smp-observe/launch.ld")
list(REMOVE_DUPLICATES hdmi_inputs)
set(hdmi_identity "native-hdmi-1;${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};8;720p60;limited;TV0-pattern;supervised;no-DMA")
foreach(input IN LISTS hdmi_inputs)
 file(SHA256 "${input}" hdmi_digest)
 string(APPEND hdmi_identity "${hdmi_digest}")
endforeach()
string(SHA256 hdmi_build_id "${hdmi_identity}")
string(REGEX REPLACE "(..)" "0x\\1," hdmi_id_bytes "${hdmi_build_id}")
file(WRITE "${hdmi_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${hdmi_id_bytes}};\n")
file(WRITE "${hdmi_build}/build-id.txt" "${hdmi_build_id}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${hdmi_inputs})
target_sources(sunstone-a733-hdmi PRIVATE "${hdmi_build}/build-id.c")
target_link_options(sunstone-a733-hdmi PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--undefined=a733_hdmi_session_bytes "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${hdmi_build}/image.map")
set_target_properties(sunstone-a733-hdmi PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
