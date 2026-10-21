# Supervised fixed RTC adoption, initialization and owner-task operations.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(RTC)
file(GLOB rtc_headers "${SUNSTONE_SOURCE_ROOT}/include/a733_rtc*.h")
sunstone_board_application(rtc PLATFORM_PATCH NO_POOLS
 TAG "native-rtc4;owner-service;startup6;supervised60;TypeA;external32768;alarm1..10;copied88"
 CONTRACT_MAGIC 0x4153373352544331ULL RESULT_MAGIC 0x4153373352545231ULL
 DEFINITIONS A733_BOARD_STARTUP_SECONDS=6
 SOURCES apps/rtc/main.c apps/rtc/initialize.c apps/rtc/runtime.c apps/rtc/service.c
  apps/rtc/service-port.c apps/rtc/native.c apps/rtc/irq.c apps/rtc/usb0.c apps/rtc/admission.c
  apps/rtc/snapshot.c drivers/rtc/a733_rtc.c drivers/rtc/a733_rtc_initialize.c
  drivers/rtc/a733_rtc_boot.c drivers/gpu/gicv3_spi.c
 INPUTS ${rtc_headers} apps/rtc/sources.json)
