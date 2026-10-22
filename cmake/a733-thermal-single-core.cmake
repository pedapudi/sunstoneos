# Supervised five-sensor thermal observation on the boot CPU.
include("${CMAKE_CURRENT_LIST_DIR}/a733-application-single-core.cmake")
sunstone_single_core_application(thermal WORKLOAD thermal-five-sensor
 PATCHES session apps/board/session.c snapshot apps/thermal/snapshot.c main apps/board/main.c
 DEFINITIONS SUNSTONE_THERMAL_SINGLE_CORE=1 SUNSTONE_SD_SINGLE_CORE=1)
