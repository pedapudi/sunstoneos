# Same supervised board provider composition with two native monitor owner tasks.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(MONITOR)
sunstone_board_application(monitor PLATFORM_PATCH
 TAG "native-monitor1;supervised60;gpadc03-ths5;repeated;irq70-71;configured-disable;retained-cldo3;retained"
 CONTRACT_MAGIC 0x415337334d4f4331ULL RESULT_MAGIC 0x415337334d4f5231ULL
 PROFILE monitor-supervised-v1
 SOURCES drivers/adc/a733_adc.c drivers/adc/a733_adc_native.c drivers/gpu/gicv3_spi.c
  drivers/thermal/a733_thermal.c drivers/thermal/a733_thermal_native.c
  apps/monitor/main.c apps/monitor/native.c apps/monitor/run.c apps/monitor/service.c
  apps/monitor/parent.c apps/monitor/usb0.c apps/monitor/admission.c apps/monitor/snapshot.c
 INPUTS include/a733_adc.h include/a733_adc_native.h include/a733_thermal.h include/a733_thermal_native.h)
