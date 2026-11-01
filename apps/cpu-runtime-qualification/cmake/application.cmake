# Use the existing OS services, scheduler and returning lifetime.
add_sunstone_application(cpu-reliability
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../reliability.c"
    "${SUNSTONE_SOURCE_ROOT}/checks/fp-context.S"
  MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../reliability-manifest.json"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/..")
