# Same supervised board provider composition with one fixed native JPEG task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(JPEG)
sunstone_board_application(jpeg
 TAG "runtime-native-jpeg2;supervised60;bounded420;optional-oracle;private-copy;NC-guards"
 CONTRACT_MAGIC 0x415337334a504332ULL RESULT_MAGIC 0x415337334a505232ULL
 SOURCES apps/jpeg/main.c apps/jpeg/input.c apps/jpeg/native.c apps/jpeg/usb0.c
  apps/jpeg/admission.c apps/jpeg/snapshot.c drivers/video/a733_video_jpeg.c
  drivers/video/a733_video_jpeg_run.c drivers/video/a733_video_platform.c
  drivers/video/a733_video_unit.c drivers/iommu/a733_iommu.c
 INPUTS apps/jpeg/fixtures.json apps/jpeg/fixture.py include/a733_jpeg_application.h)
