# SPDX-License-Identifier: GPL-2.0-only
if(SUNSTONE_APPLICATION STREQUAL "cpu-split")
  set(capacity_main split.c)
  set(capacity_manifest split-manifest.json)
else()
  set(capacity_main native.c)
  set(capacity_manifest manifest.json)
endif()
add_sunstone_application(${SUNSTONE_APPLICATION}
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../${capacity_main}" "${CMAKE_CURRENT_LIST_DIR}/../kernel.c"
  MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../${capacity_manifest}"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/..")
# Explicit SIMD assembly in kernel.c preserves the OS scalar compiler policy.
