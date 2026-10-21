# Same supervised board provider composition with one fixed native PWM task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(PWM)
sunstone_board_application(pwm PLATFORM_PATCH
 TAG "native-pwm-service2;supervised60;PD5-40k-continuous;bounded-duty-stop-restart;PL7-GPIO;IRQ67;retained-low"
 CONTRACT_MAGIC 0x4153373350574331ULL RESULT_MAGIC 0x4153373350575231ULL
 PROFILE pwm-service-supervised-v1
 SOURCES drivers/pwm/a733_pwm.c drivers/pwm/a733_pwm_native.c drivers/gpu/gicv3_spi.c
  apps/pwm/main.c apps/pwm/native.c apps/pwm/usb0.c apps/pwm/admission.c apps/pwm/snapshot.c
 INPUTS include/a733_pwm.h include/a733_pwm_native.h include/gicv3_spi.h)
