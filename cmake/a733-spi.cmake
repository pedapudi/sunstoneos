option(SUNSTONE_SPI_DMA "Finite SPI3 command/4096-byte response through DMA0" OFF)
set(spi_profile "spi-supervised-v1")
set(spi_dma_sources)
set(spi_dma_definitions)
if(SUNSTONE_SPI_DMA)
 set(spi_profile "spi-dma-supervised-v3")
 set(spi_dma_definitions SUNSTONE_SPI_DMA=1)
 set(spi_dma_sources apps/spi/dma.c platform/a733/dma_startup.c platform/a733/dma_native.c
  drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
  drivers/dma/a733_dma_platform.c drivers/dma/a733_dma_platform_native.c)
endif()
# Same supervised board provider composition with one fixed native SPI task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(SPI)
sunstone_board_application(spi PLATFORM_PATCH
 TAG "${spi_profile};supervised60;spi3-pe-cs0;write-read32;poll-or-IRQ59;retained-bldo1;retained"
 CONTRACT_MAGIC 0x4153373353504331ULL RESULT_MAGIC 0x4153373353505231ULL
 PROFILE "${spi_profile}"
 DEFINITIONS ${spi_dma_definitions}
 SOURCES drivers/gpu/gicv3_spi.c drivers/spi/a733_spi.c drivers/spi/a733_spi_platform.c
  drivers/spi/a733_spi_platform_native.c
  apps/spi/main.c apps/spi/native.c apps/spi/irq.c apps/spi/run.c apps/spi/usb0.c
  apps/spi/admission.c apps/spi/snapshot.c ${spi_dma_sources}
 INPUTS include/gicv3_spi.h include/a733_spi.h include/a733_spi_platform.h include/a733_spi_platform_native.h)
