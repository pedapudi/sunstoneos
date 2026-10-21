# Same supervised board provider composition with one read-only native SD0 task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(STORAGE_READ)
sunstone_board_application(storage-read
 TAG "native-sd0-read-service2;supervised60;two-task-clients;bounded-ranges8;PIO-private-copy;retained"
 CONTRACT_MAGIC 0x4153373352444331ULL RESULT_MAGIC 0x4153373352445231ULL
 SOURCES apps/storage-read/modes.c apps/storage-read/main.c apps/storage-read/native.c
  apps/storage-read/client.c apps/storage-read/usb0.c apps/storage-read/admission.c
  apps/storage-read/snapshot.c drivers/storage/a733_mmc_platform.c
  drivers/storage/a733_mmc_platform_native.c drivers/storage/sunxi_mmc.c
  drivers/storage/a733_storage_update.c)
