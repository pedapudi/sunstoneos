add_sunstone_application(latency-cost
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../native.c"
  MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../manifest.json")
target_compile_options(latency-cost PRIVATE -mgeneral-regs-only)
if(SUNSTONE_LATENCY)
  target_compile_definitions(latency-cost PRIVATE SUNSTONE_LATENCY=1)
endif()
