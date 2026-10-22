# Supervised PWM fan and optional LED service on the boot CPU.
include("${CMAKE_CURRENT_LIST_DIR}/a733-application-single-core.cmake")
sunstone_single_core_application(pwm POOLS WORKLOAD pwm-fan-led
 PATCHES session apps/board/session.c main apps/board/main.c
 PATCHED_INCLUDES board-admission apps/board/admission.c
 DEFINITIONS SUNSTONE_PWM_SINGLE_CORE=1 SUNSTONE_SD_SINGLE_CORE=1)
