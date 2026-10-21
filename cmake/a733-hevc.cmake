# Same supervised board provider composition with one retained two-picture
# HEVC decoder task in the existing extended-memory profile.
set(SUNSTONE_HEVC_SOURCE "" CACHE PATH "Pinned GStreamer HEVC parser and Linux Cedrus source cache")
set(SUNSTONE_HEVC_READER_SOURCE "" CACHE PATH "Pinned GStreamer NAL reader source cache")
# The parser, register field definitions and NAL reader are generated from
# pinned third-party files that stay outside the tree (apps/hevc/sources.json).
if(NOT EXISTS "${SUNSTONE_HEVC_SOURCE}/gsth265parser.c" OR
   NOT EXISTS "${SUNSTONE_HEVC_SOURCE}/cedrus_regs.h" OR
   NOT EXISTS "${SUNSTONE_HEVC_READER_SOURCE}/pins.json")
 message(FATAL_ERROR "Native HEVC decoding requires SUNSTONE_HEVC_SOURCE, holding the pinned "
  "GStreamer H.265 parser and Linux Cedrus files, and SUNSTONE_HEVC_READER_SOURCE, holding the "
  "pinned GStreamer NAL reader. See docs/hevc-native.md")
endif()
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(HEVC)
set(hevc_build "${CMAKE_CURRENT_BINARY_DIR}/hevc-application")
file(MAKE_DIRECTORY "${hevc_build}")
apply_build_patch("${hevc_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/boot.patch")
apply_build_patch("${hevc_build}/boot.c" "${hevc_build}/boot-compact.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/extended-boot.patch")
apply_build_patch("${hevc_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/extended-link.patch")
set(hevc_gst "${hevc_build}/gst")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${SUNSTONE_SOURCE_ROOT}/apps/hevc/gst-source.py"
 --source "${SUNSTONE_HEVC_SOURCE}" --reader "${SUNSTONE_HEVC_READER_SOURCE}" --output "${hevc_gst}"
 COMMAND_ERROR_IS_FATAL ANY)
set(hevc_pinned)
foreach(name IN ITEMS gsth265parser.c gsth265parser.h cedrus_regs.h cedrus_h265.c)
 list(APPEND hevc_pinned "${SUNSTONE_HEVC_SOURCE}/${name}")
endforeach()
foreach(name IN ITEMS nalutils.c nalutils.h pins.json)
 list(APPEND hevc_pinned "${SUNSTONE_HEVC_READER_SOURCE}/${name}")
endforeach()
sunstone_board_application(hevc BOOT "${hevc_build}/boot.c" LINK_SCRIPT "${hevc_build}/link.ld"
 TAG "runtime-native-hevc1;supervised60;Main8-VGA-CTB64-IDR-P-NV12;private-copy;extended-NC-guards"
 PROFILE hevc-supervised-v1
 CONTRACT_MAGIC 0x4153373348354331ULL RESULT_MAGIC 0x4153373348355231ULL
 DEFINITIONS SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1 INCLUDES "${hevc_gst}"
 LINK_OPTIONS -Wl,--undefined=hevc_geometry
 SOURCES apps/hevc/main.c apps/hevc/input.c apps/hevc/storage.c apps/hevc/native.c
  apps/hevc/parse.c apps/hevc/usb0.c apps/hevc/admission.c apps/hevc/snapshot.c
  "${hevc_gst}/gst-hevc-parser.c" "${hevc_gst}/gst-nal.c"
  drivers/video/a733_video_hevc.c drivers/video/a733_video_platform.c
  drivers/video/a733_video_unit.c drivers/iommu/a733_iommu.c platform/a733/device_memory_layout.c
 INPUTS include/a733_hevc_application.h include/a733_video_hevc.h include/a733_video_platform.h
  include/a733_video_unit.h apps/hevc/sources.json apps/hevc/gst-source.py apps/h264/gst-port.h
  apps/hevc/extended-boot.patch apps/hevc/extended-link.patch platform/a733-smp-cached/link-devices.ld
  "${hevc_gst}/gst-hevc-types.h" "${hevc_gst}/gst-nal.h" "${hevc_gst}/cedrus-hevc-regs.h"
  "${hevc_gst}/gst-inputs.json" ${hevc_pinned})
