option(SUNSTONE_UART_DMA "Finite UART2 byte DMA instead of IRQ FIFO" OFF)
# Same supervised board provider composition with one fixed native UART task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(UART)
set(uart_profile "uart-supervised-v1")
set(uart_dma_sources)
set(uart_dma_definitions)
if(SUNSTONE_UART_DMA)
 set(uart_profile "uart-dma-supervised-v2")
 set(uart_dma_sources apps/uart/dma.c platform/a733/dma_startup.c platform/a733/dma_native.c
  drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
  drivers/dma/a733_dma_platform.c drivers/dma/a733_dma_platform_native.c drivers/dma/dma_service.c
  drivers/dma/dma_service_i2s.c drivers/dma/dma_service_i2s_stream.c
  drivers/audio/a733_i2s.c drivers/audio/a733_i2s_stream.c)
 set(uart_dma_definitions SUNSTONE_UART_DMA=1)
endif()
sunstone_board_application(uart PLATFORM_PATCH
 TAG "${uart_profile};native-uart2;supervised60;pb0-pb1-passive-loopback;8n1-115200;IRQ36;closed"
 CONTRACT_MAGIC 0x4153373355414331ULL RESULT_MAGIC 0x4153373355415231ULL
 DEFINITIONS ${uart_dma_definitions}
 PROFILE "${uart_profile}"
 SOURCES drivers/gpu/gicv3_spi.c drivers/uart/a733_uart.c drivers/uart/a733_uart_platform.c
  drivers/uart/a733_uart_platform_native.c apps/uart/main.c apps/uart/native.c apps/uart/irq.c
  apps/uart/run.c apps/uart/usb0.c apps/uart/admission.c apps/uart/snapshot.c ${uart_dma_sources}
 INPUTS include/gicv3_spi.h include/a733_uart.h include/a733_uart_platform.h
  include/a733_uart_platform_native.h)
