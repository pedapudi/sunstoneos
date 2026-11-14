if(DEFINED SUNSTONE_TLBI_CORE AND NOT SUNSTONE_TLBI_CORE STREQUAL "5")
  message(FATAL_ERROR "event-waits selects the TLBI control task on core 5")
endif()
if(NOT SUNSTONE_BG_LOAD OR NOT DEFINED SUNSTONE_TLBI_ISSUER OR NOT SUNSTONE_TLBI_ISSUER STREQUAL "0")
  message(FATAL_ERROR "event-waits requires SUNSTONE_BG_LOAD=ON and SUNSTONE_TLBI_ISSUER=0")
endif()
add_sunstone_application(event-waits
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../main.c"
  MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../manifest.json")
target_compile_options(event-waits PRIVATE -mgeneral-regs-only)
