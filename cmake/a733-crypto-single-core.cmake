# Supervised fixed SHA-256/AES application on the boot CPU.
include("${CMAKE_CURRENT_LIST_DIR}/a733-application-single-core.cmake")
sunstone_single_core_application(crypto POOLS WORKLOAD fixed-sha256-aes128
 PATCHES session apps/board/session.c main apps/board/main.c
 DEFINITIONS SUNSTONE_CRYPTO_SINGLE_CORE=1 SUNSTONE_SD_SINGLE_CORE=1)
