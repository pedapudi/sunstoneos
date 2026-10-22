# Complete supervised board application on the boot CPU.
include("${CMAKE_CURRENT_LIST_DIR}/a733-application-single-core.cmake")
sunstone_single_core_application(board POOLS WORKLOAD board-usb-radio
 PATCHES session apps/board/session.c identity apps/board/identity.c admission apps/board/admission.c
  main apps/board/main.c
 DEFINITIONS SUNSTONE_BOARD_SINGLE_CORE=1)
