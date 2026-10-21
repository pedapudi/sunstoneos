# Same supervised board provider composition with one fixed native JPEG_ENCODE task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(JPEG_ENCODE)
sunstone_board_application(jpeg-encode BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/jpeg_encode-application"
 TAG "runtime-native-jpeg-encode1;supervised60;VGA-NV12-quality75;private-copy;NC-guards"
 CONTRACT_MAGIC 0x415337334a454331ULL RESULT_MAGIC 0x415337334a455231ULL
 SOURCES apps/jpeg-encode/main.c apps/jpeg-encode/input.c apps/jpeg-encode/native.c
  apps/jpeg-encode/usb0.c apps/jpeg-encode/admission.c apps/jpeg-encode/snapshot.c
  drivers/video/a733_video_jpeg_encode.c drivers/video/a733_video_jpeg_encode_tables.c
  drivers/video/a733_video_encode_platform.c drivers/video/a733_video_unit.c
  drivers/iommu/a733_iommu.c
 INPUTS apps/jpeg-encode/sources.json apps/jpeg-encode/producer.py
  include/a733_jpeg_encode_application.h)
