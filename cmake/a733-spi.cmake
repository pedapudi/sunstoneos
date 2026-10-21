# Same supervised board provider composition with one fixed native SPI task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(SPI)
sunstone_board_application(spi PLATFORM_PATCH
 TAG "native-spi1;supervised60;spi3-pe-cs0;write-read32;poll-or-IRQ59;retained-bldo1;retained"
 CONTRACT_MAGIC 0x4153373353504331ULL RESULT_MAGIC 0x4153373353505231ULL
 PROFILE spi-supervised-v1
 SOURCES drivers/gpu/gicv3_spi.c drivers/spi/a733_spi.c drivers/spi/a733_spi_platform.c
  drivers/spi/a733_spi_platform_native.c
  apps/spi/main.c apps/spi/native.c apps/spi/irq.c apps/spi/run.c apps/spi/usb0.c
  apps/spi/admission.c apps/spi/snapshot.c
 INPUTS include/gicv3_spi.h include/a733_spi.h include/a733_spi_platform.h include/a733_spi_platform_native.h)
