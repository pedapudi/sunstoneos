# Same supervised board provider composition with one fixed recorded-raw ISP0 processing task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(ISP0)
sunstone_board_application(isp0
 TAG "runtime-native-isp01;supervised20;640x480-BGGR12-NV16;private-copy;NC-guards"
 CONTRACT_MAGIC 0x4153373349504331ULL RESULT_MAGIC 0x4153373349505231ULL
 DEFINITIONS A733_BOARD_SUPERVISED_SECONDS=20
 SOURCES apps/isp0/main.c apps/isp0/input.c apps/isp0/memory.c apps/isp0/native.c apps/isp0/usb.c
  apps/isp0/admission.c apps/isp0/snapshot.c drivers/camera/a733_isp0.c
  drivers/camera/a733_camera_dma.c drivers/clock/a733_media_clock.c
  drivers/clock/a733_media_clock_native.c drivers/iommu/a733_iommu.c
 INPUTS drivers/camera/isp0-sources.json drivers/camera/isp600_default.h include/a733_isp0.h
  include/a733_isp0_application.h)
