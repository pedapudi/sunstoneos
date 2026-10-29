# Same supervised board provider composition with one fixed native ADC task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(ADC)
sunstone_board_application(adc PLATFORM_PATCH
 TAG "native-adc1;supervised60;gpadc03;optional-compare-v3;two-sets;irq70;retained-cldo3;retained"
 CONTRACT_MAGIC 0x4153373341444331ULL RESULT_MAGIC 0x4153373341445231ULL
 PROFILE adc-supervised-v1
 SOURCES drivers/adc/a733_adc.c drivers/adc/a733_lradc.c drivers/adc/a733_adc_native.c drivers/gpu/gicv3_spi.c
  apps/adc/main.c apps/adc/native.c apps/adc/run.c apps/adc/collect.c apps/adc/usb0.c
  apps/adc/admission.c apps/adc/snapshot.c
 INPUTS include/a733_lradc.h apps/adc/lradc-sources.json include/a733_adc.h include/a733_adc_native.h)
