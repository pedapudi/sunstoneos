# Supervised RTC owner and native clients on the boot CPU.
include("${CMAKE_CURRENT_LIST_DIR}/a733-application-single-core.cmake")
sunstone_single_core_application(rtc WORKLOAD rtc-owner-service
 PATCHES session apps/board/session.c snapshot apps/rtc/snapshot.c initialize apps/rtc/initialize.c
  main apps/board/main.c
 DEFINITIONS SUNSTONE_RTC_SINGLE_CORE=1 SUNSTONE_SD_SINGLE_CORE=1)
