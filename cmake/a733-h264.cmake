option(SUNSTONE_H264_HIGH "Runtime bounded High/CABAC short-term I/P/B profile" OFF)
set(SUNSTONE_H264_GSTREAMER_SOURCE "" CACHE PATH "Pinned GStreamer H264 standalone source cache")
if(SUNSTONE_H264_HIGH)
 set(SUNSTONE_H264_FULLHD ON CACHE BOOL "High uses existing extended memory" FORCE)
endif()
option(SUNSTONE_H264_FULLHD "Admitted retained full-HD extended-memory profile" OFF)
# Same supervised board provider composition with one bounded native H.264 task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(H.264)
set(h264_build "${CMAKE_CURRENT_BINARY_DIR}/h264-application")
file(MAKE_DIRECTORY "${h264_build}")
apply_build_patch("${h264_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264/boot.patch")
set(h264_script)
set(h264_sources)
set(h264_definitions)
set(h264_includes)
set(h264_extra_inputs)
set(h264_link_options)
if(SUNSTONE_H264_FULLHD)
 apply_build_patch("${h264_build}/boot.c" "${h264_build}/boot-compact.c"
  "${SUNSTONE_SOURCE_ROOT}/apps/h264/fullhd-boot.patch")
 apply_build_patch("${h264_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
  "${SUNSTONE_SOURCE_ROOT}/apps/h264/fullhd-link.patch")
 set(h264_script LINK_SCRIPT "${h264_build}/link.ld")
 list(APPEND h264_sources platform/a733/device_memory_layout.c)
 list(APPEND h264_definitions SUNSTONE_H264_FULLHD=1 SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1)
 list(APPEND h264_link_options -Wl,--undefined=h264_fullhd_geometry)
else()
 configure_file("${h264_build}/boot-compact.c" "${h264_build}/boot.c" COPYONLY)
endif()
if(SUNSTONE_H264_HIGH)
 if(NOT EXISTS "${SUNSTONE_H264_GSTREAMER_SOURCE}/pins.json")
  message(FATAL_ERROR "High profile requires pinned standalone GStreamer source cache")
 endif()
 set(h264_gst "${h264_build}/gst")
 execute_process(COMMAND "${Python3_EXECUTABLE}" "${SUNSTONE_SOURCE_ROOT}/apps/h264/gst-source.py"
  --source "${SUNSTONE_H264_GSTREAMER_SOURCE}" --output "${h264_gst}" COMMAND_ERROR_IS_FATAL ANY)
 list(APPEND h264_sources apps/h264/high-parse.c "${h264_gst}/gst-h264-parser.c" "${h264_gst}/gst-nal.c")
 list(APPEND h264_definitions SUNSTONE_H264_HIGH=1)
 list(APPEND h264_includes "${h264_gst}")
 foreach(name IN ITEMS gsth264parser.c gsth264parser.h nalutils.c nalutils.h gsth264decoder.c gsth264picture.c COPYING pins.json)
  list(APPEND h264_extra_inputs "${SUNSTONE_H264_GSTREAMER_SOURCE}/${name}")
 endforeach()
 list(APPEND h264_extra_inputs apps/h264/gst-source.py include/a733_h264_high.h
  "${h264_gst}/gst-h264-types.h" "${h264_gst}/gst-nal.h")
 list(APPEND h264_link_options -Wl,--undefined=h264_high_geometry)
endif()
sunstone_board_application(h264 BOOT "${h264_build}/boot.c" ${h264_script}
 TAG "high=${SUNSTONE_H264_HIGH};profile=${SUNSTONE_H264_FULLHD};uploaded-native-h264-sequence2;supervised60;48x32;YU12;exact-private-copy;NC-guards"
 CONTRACT_MAGIC 0x4153373348324331ULL RESULT_MAGIC 0x4153373348325231ULL
 DEFINITIONS ${h264_definitions} INCLUDES ${h264_includes} LINK_OPTIONS ${h264_link_options}
 SOURCES ${h264_sources} apps/h264/main.c apps/h264/input.c apps/h264/stream.c
  apps/h264/stream-storage.c apps/h264/stream-usb.c apps/h264/native.c apps/h264/usb0.c
  apps/h264/admission.c apps/h264/snapshot.c drivers/video/a733_video_h264.c
  drivers/video/a733_video_platform.c drivers/video/a733_video_unit.c drivers/iommu/a733_iommu.c
 INPUTS include/a733_h264_application.h include/a733_video_h264.h include/a733_video_platform.h
  include/a733_video_unit.h apps/h264/sources.json apps/h264/fullhd-boot.patch
  apps/h264/fullhd-link.patch platform/a733-smp-cached/link-devices.ld include/a733_h264_input.h
  ${h264_extra_inputs})
