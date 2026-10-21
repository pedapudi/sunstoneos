# Same supervised board provider composition with one retained two-picture H264 encoder task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(H264_ENCODE)
set(h264_encode_build "${CMAKE_CURRENT_BINARY_DIR}/h264_encode-application")
file(MAKE_DIRECTORY "${h264_encode_build}")
apply_build_patch("${h264_encode_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode/boot.patch")
apply_build_patch("${h264_encode_build}/boot.c" "${h264_encode_build}/boot-compact.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode/extended-boot.patch")
apply_build_patch("${h264_encode_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode/extended-link.patch")
sunstone_board_application(h264-encode BUILD_DIR "${h264_encode_build}"
 BOOT "${h264_encode_build}/boot.c" LINK_SCRIPT "${h264_encode_build}/link.ld"
 TAG "runtime-native-h264-encode1;supervised60;two-VGA-NV12-Baseline-CAVLC-QP26;private-copy;extended-NC-guards"
 CONTRACT_MAGIC 0x4153373348454331ULL RESULT_MAGIC 0x4153373348455231ULL
 DEFINITIONS SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1
 SOURCES platform/a733/device_memory_layout.c apps/h264-encode/main.c apps/h264-encode/input.c
  apps/h264-encode/storage.c apps/h264-encode/native.c apps/h264-encode/usb0.c
  apps/h264-encode/admission.c apps/h264-encode/snapshot.c drivers/video/a733_video_h264_encode.c
  drivers/video/a733_video_encode_platform.c drivers/video/a733_video_unit.c
  drivers/iommu/a733_iommu.c
 INPUTS apps/h264-encode/extended-boot.patch apps/h264-encode/extended-link.patch
  platform/a733-smp-cached/link-devices.ld include/a733_h264_encode_application.h
  include/a733_video_h264_encode.h include/a733_video_encode_platform.h include/a733_video_unit.h)
