# Default: supervised TV0 hardware colorbar. Optional retained DE framebuffer.
option(SUNSTONE_HDMI_FRAMEBUFFER "Build fixed720p RGB565 framebuffer HDMI image" OFF)
option(SUNSTONE_HDMI_AUDIO "Build finite fixed48kHz stereo PCM over internal I2S3" OFF)
option(SUNSTONE_HDMI_AUDIO_USB "Copy finite HDMI audio results through USB0 PIO" OFF)
if(SUNSTONE_HDMI_AUDIO_USB AND NOT SUNSTONE_HDMI_AUDIO)
 message(FATAL_ERROR "HDMI audio observation requires the finite audio profile")
endif()
if(SUNSTONE_HDMI_AUDIO AND SUNSTONE_HDMI_FRAMEBUFFER)
 message(FATAL_ERROR "HDMI audio currently uses the TV0 pattern only")
endif()
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
foreach(source IN ITEMS apps/hdmi/main.c apps/hdmi/admission.c apps/hdmi/snapshot.c apps/hdmi/output.c
 drivers/display/a733_hdmi.c drivers/display/a733_hdmi_phy.c drivers/display/a733_tcon_tv.c
 platform/a733/hdmi_native.c)
 list(APPEND hdmi_sources "${hdmi_root}/${source}")
endforeach()
if(SUNSTONE_HDMI_FRAMEBUFFER)
 foreach(source IN ITEMS apps/hdmi/framebuffer.c runtime/device_memory_pool.c
  platform/a733/memory_uncached.c drivers/iommu/a733_iommu.c
  drivers/clock/a733_media_clock.c drivers/clock/a733_media_clock_native.c
  drivers/display/a733_display_engine.c drivers/display/a733_tcon_lcd.c
  drivers/display/a733_dsi_video.c drivers/display/a733_dsi_phy.c)
  list(APPEND hdmi_sources "${hdmi_root}/${source}")
 endforeach()
endif()
if(SUNSTONE_HDMI_AUDIO)
 foreach(source IN ITEMS apps/hdmi-audio/native.c drivers/audio/a733_hdmi_audio.c
  runtime/device_memory_pool.c platform/a733/memory_uncached.c
  platform/a733/dma_startup.c platform/a733/dma_native.c drivers/dma/dma_memory.c
  drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c drivers/dma/a733_dma_platform.c
  drivers/dma/a733_dma_platform_native.c drivers/dma/dma_service.c drivers/dma/dma_service_i2s.c
  drivers/dma/dma_service_i2s_stream.c drivers/audio/a733_i2s.c drivers/audio/a733_i2s_stream.c
  drivers/audio/a733_i2s_platform.c drivers/audio/a733_i2s_platform_native.c drivers/audio/a733_audio_leases.c)
  list(APPEND hdmi_sources "${hdmi_root}/${source}")
 endforeach()
endif()
if(SUNSTONE_HDMI_AUDIO_USB)
 foreach(source IN ITEMS apps/hdmi-audio/usb.c apps/hdmi-audio/observation.c
  drivers/usb/a733_usb_platform.c drivers/usb/a733_usb_platform_native.c)
  list(APPEND hdmi_sources "${hdmi_root}/${source}")
 endforeach()
 list(APPEND hdmi_sources "${storage_build}/tinyusb/dcd_sunxi_musb.c" "${TINYUSB_DIR}/src/tusb.c"
  "${TINYUSB_DIR}/src/common/tusb_fifo.c" "${TINYUSB_DIR}/src/device/usbd.c"
  "${TINYUSB_DIR}/src/class/vendor/vendor_device.c")
endif()
list(REMOVE_DUPLICATES hdmi_sources)
add_executable(sunstone-a733-hdmi EXCLUDE_FROM_ALL ${hdmi_sources})
target_compile_definitions(sunstone-a733-hdmi PRIVATE SUNSTONE_HDMI_FRAMEBUFFER=$<BOOL:${SUNSTONE_HDMI_FRAMEBUFFER}> SUNSTONE_HDMI_AUDIO=$<BOOL:${SUNSTONE_HDMI_AUDIO}> SUNSTONE_HDMI_AUDIO_USB=$<BOOL:${SUNSTONE_HDMI_AUDIO_USB}>)
if(SUNSTONE_HDMI_AUDIO_USB)
 target_compile_definitions(sunstone-a733-hdmi PRIVATE SUNSTONE_A733_USB=1)
 target_include_directories(sunstone-a733-hdmi PRIVATE "${TINYUSB_DIR}/src"
  "${hdmi_root}/platform/a733-storage-update" "${storage_build}/tinyusb")
endif()
get_target_property(hdmi_options sunstone-a733-storage-maintenance COMPILE_OPTIONS)
set_property(TARGET sunstone-a733-hdmi PROPERTY COMPILE_OPTIONS "${hdmi_options}")
target_include_directories(sunstone-a733-hdmi BEFORE PRIVATE "${hdmi_root}/apps/hdmi"
 "${hdmi_root}/platform/a733-smp-cached" "${hdmi_root}/include" "${hdmi_root}/runtime/include"
 "${FREERTOS_DIR}/include" "${storage_base}")
target_compile_definitions(sunstone-a733-hdmi PRIVATE SUNSTONE_SECURE_MONITOR=1
 SUNSTONE_A733_SMP_DIAGNOSTIC=1 A733_SMP_CONTRACT_MAGIC=0x4153373348444331ULL
 A733_SMP_RESULT_MAGIC=0x4153373348445231ULL)
if(SUNSTONE_HDMI_AUDIO)
 target_link_options(sunstone-a733-hdmi PRIVATE -Wl,--defsym=__uncached_pool_size=2097152
  -Wl,--undefined=a733_hdmi_audio_geometry -Wl,--undefined=hdmi_audio_result -Wl,--undefined=hdmi_audio_result_ready)
endif()
if(SUNSTONE_HDMI_FRAMEBUFFER)
 target_link_options(sunstone-a733-hdmi PRIVATE -Wl,--defsym=__uncached_pool_size=2097152
  -Wl,--undefined=a733_hdmi_framebuffer_offset)
endif()
target_link_options(sunstone-a733-hdmi PRIVATE -Wl,--undefined=a733_hdmi_framebuffer_mode)
file(GLOB hdmi_headers "${hdmi_root}/apps/hdmi/*.h")
if(SUNSTONE_HDMI_AUDIO)
 list(APPEND hdmi_headers "${hdmi_root}/apps/hdmi-audio/CMakeLists.txt"
  "${hdmi_root}/apps/hdmi-audio/prepare.py" "${hdmi_root}/apps/hdmi-audio/capture.py"
  "${hdmi_root}/apps/hdmi-audio/observe.py")
 file(GLOB audio_headers "${hdmi_root}/apps/hdmi-audio/*.h")
 list(APPEND hdmi_headers ${audio_headers} "${hdmi_root}/include/a733_hdmi_audio.h")
endif()
set(hdmi_inputs ${hdmi_sources} ${hdmi_headers} ${storage_inputs}
 "${CMAKE_CURRENT_LIST_FILE}" "${hdmi_root}/apps/hdmi/CMakeLists.txt"
 "${hdmi_root}/apps/hdmi/boot.patch" "${hdmi_root}/apps/hdmi/prepare.py"
 "${hdmi_root}/platform/a733-smp-supervised/launch.c"
 "${hdmi_root}/platform/a733-smp-observe/launch.S" "${hdmi_root}/platform/a733-smp-observe/launch.ld")
list(REMOVE_DUPLICATES hdmi_inputs)
if(SUNSTONE_HDMI_FRAMEBUFFER)
 set(hdmi_profile "DE1-TV0;RGB565;retained-DMA")
else()
 set(hdmi_profile "TV0-pattern;no-DMA")
endif()
set(hdmi_identity "native-hdmi-3;audio=${SUNSTONE_HDMI_AUDIO};audio-usb=${SUNSTONE_HDMI_AUDIO_USB};framebuffer=${SUNSTONE_HDMI_FRAMEBUFFER};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};8;720p60;limited;${hdmi_profile};supervised")
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
