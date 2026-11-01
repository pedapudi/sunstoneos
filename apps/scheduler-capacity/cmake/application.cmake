add_sunstone_application(cpu-capacity
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../native.c" "${CMAKE_CURRENT_LIST_DIR}/../kernel.c"
  MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../manifest.json"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/..")
# Explicit SIMD assembly in kernel.c preserves the OS scalar compiler policy.
