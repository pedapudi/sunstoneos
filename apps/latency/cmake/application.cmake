add_sunstone_application(latency
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../native.c" "${CMAKE_CURRENT_LIST_DIR}/../workload.c"
  MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../manifest.json"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/..")
target_compile_definitions(latency PRIVATE SUNSTONE_LATENCY=1)
target_compile_options(latency PRIVATE "$<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>")
