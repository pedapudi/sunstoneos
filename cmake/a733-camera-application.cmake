# One retained OV5640 exposure, existing native shared providers and USB0 sink.
if(NOT TARGET sunstone-a733-storage-maintenance)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-storage-maintenance.cmake")
endif()
option(SUNSTONE_CAMERA_SUPERVISED "Retained camera session with manual whole-board power removal" OFF)
set(camera_root "${SUNSTONE_SOURCE_ROOT}")
set(camera_build "${CMAKE_CURRENT_BINARY_DIR}/camera-application")
file(MAKE_DIRECTORY "${camera_build}")
get_target_property(camera_sources sunstone-a733-storage-maintenance SOURCES)
foreach(source IN ITEMS
 platform/a733-storage-maintenance/main.c platform/a733-storage-maintenance/session.c
 platform/a733-storage-maintenance/admission.c platform/a733-storage-update/bootstrap.c
 platform/a733-storage-update/worker.c platform/a733-storage-update/usb.c
 drivers/storage/a733_storage_update.c drivers/storage/storage_update_usb.c
 drivers/storage/a733_mmc_platform.c drivers/storage/a733_mmc_platform_native.c
 drivers/storage/sunxi_mmc.c drivers/watchdog/a733_maintenance_watchdog.c)
 list(REMOVE_ITEM camera_sources "${camera_root}/${source}")
endforeach()
list(REMOVE_ITEM camera_sources "${storage_build}/build-id.c" "${storage_build}/boot.c")
if(SUNSTONE_CAMERA_SUPERVISED)
 list(REMOVE_ITEM camera_sources "${storage_build}/boot.c" "${camera_root}/platform/a733/maintenance_watchdog.c")
 apply_build_patch("${camera_build}/boot-supervised.c" "${storage_build}/boot.c"
  "${camera_root}/platform/a733-smp-supervised/boot.patch")
 set(camera_boot "${camera_build}/boot-supervised.c")
else()
 set(camera_boot "${storage_build}/boot.c")
endif()
apply_build_patch("${camera_build}/boot.c" "${camera_boot}" "${camera_root}/apps/camera/boot.patch")
list(APPEND camera_sources "${camera_build}/boot.c")
# Retain maintained boot/platform/EL3 return mechanics. Its boot patch publishes
# the immutable 4KiB session page and does not decode a storage grant.
foreach(source IN ITEMS apps/camera/main.c apps/camera/admission.c apps/camera/snapshot.c apps/camera/capture.c apps/camera/usb0.c
 drivers/camera/ov5640.c drivers/camera/a733_camera_platform.c drivers/camera/a733_csi_receiver.c
 drivers/camera/a733_camera_route.c drivers/camera/a733_camera_dma.c drivers/camera/a733_camera_capture.c
 drivers/camera/a733_camera_native.c drivers/iommu/a733_iommu.c drivers/i2c/a733_twi.c
 drivers/i2c/a733_twi_native.c drivers/clock/a733_media_clock.c drivers/clock/a733_media_clock_native.c
 platform/a733/memory_reservations.c runtime/device_memory_pool.c)
 list(APPEND camera_sources "${camera_root}/${source}")
endforeach()
add_executable(sunstone-a733-camera EXCLUDE_FROM_ALL ${camera_sources})
get_target_property(camera_options sunstone-a733-storage-maintenance COMPILE_OPTIONS)
set_property(TARGET sunstone-a733-camera PROPERTY COMPILE_OPTIONS "${camera_options}")
target_include_directories(sunstone-a733-camera BEFORE PRIVATE "${camera_root}/apps/camera"
 "${camera_root}/platform/a733-storage-update" "${camera_root}/platform/a733-smp-cached"
 "${camera_root}/drivers/usb" "${TINYUSB_DIR}/src" "${camera_root}/include"
 "${camera_root}/runtime/include" "${FREERTOS_DIR}/include" "${storage_base}")
target_compile_definitions(sunstone-a733-camera PRIVATE SUNSTONE_SECURE_MONITOR=1 SUNSTONE_A733_SMP_DIAGNOSTIC=1
 SUNSTONE_A733_USB=1)
if(NOT SUNSTONE_CAMERA_SUPERVISED)
 target_compile_definitions(sunstone-a733-camera PRIVATE
  A733_SMP_CONTRACT_MAGIC=0x4153373343414332ULL A733_SMP_RESULT_MAGIC=0x4153373343415232ULL)
endif()
if(SUNSTONE_CAMERA_SUPERVISED)
 target_compile_definitions(sunstone-a733-camera PRIVATE SUNSTONE_CAMERA_SUPERVISED=1)
 set_property(TARGET sunstone-a733-camera APPEND PROPERTY COMPILE_DEFINITIONS
  A733_SMP_CONTRACT_MAGIC=0x4153373343414333ULL A733_SMP_RESULT_MAGIC=0x4153373343415233ULL)
endif()
file(GLOB camera_headers "${camera_root}/apps/camera/*.h")
set(camera_inputs ${camera_sources} ${camera_headers} ${storage_inputs}
 "${camera_root}/apps/camera/boot.patch"
 "${CMAKE_CURRENT_LIST_FILE}" "${camera_root}/apps/camera/CMakeLists.txt" "${camera_root}/platform/a733-smp-supervised/boot.patch")
list(REMOVE_DUPLICATES camera_inputs)
set(camera_identity "native-camera-1;${SUNSTONE_CAMERA_SUPERVISED};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};8;OV5640;640x480;NV16;USB0-sink;retained")
foreach(input IN LISTS camera_inputs)
 file(SHA256 "${input}" camera_digest)
 string(APPEND camera_identity "${camera_digest}")
endforeach()
string(SHA256 camera_build_id "${camera_identity}")
string(REGEX REPLACE "(..)" "0x\\1," camera_id_bytes "${camera_build_id}")
file(WRITE "${camera_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${camera_id_bytes}};\n")
file(WRITE "${camera_build}/build-id.txt" "${camera_build_id}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${camera_inputs})
target_sources(sunstone-a733-camera PRIVATE "${camera_build}/build-id.c")
target_link_options(sunstone-a733-camera PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__device_pool_size=2097152 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${camera_build}/image.map")
set_target_properties(sunstone-a733-camera PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
