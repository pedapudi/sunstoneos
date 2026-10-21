# Same supervised board provider composition with one fixed native THERMAL task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(THERMAL)
sunstone_board_application(thermal PLATFORM_PATCH
 TAG "native-thermal1;supervised60;five-calibrated-channels;two-fresh-sets;retained"
 CONTRACT_MAGIC 0x4153373354484331ULL RESULT_MAGIC 0x4153373354485231ULL
 PROFILE thermal-supervised-v1
 SOURCES drivers/thermal/a733_thermal.c drivers/thermal/a733_thermal_native.c drivers/gpu/gicv3_spi.c
  apps/thermal/main.c apps/thermal/native.c apps/thermal/collect.c apps/thermal/usb0.c
  apps/thermal/admission.c apps/thermal/snapshot.c
 INPUTS include/a733_thermal.h include/a733_thermal_native.h include/gicv3_spi.h)
