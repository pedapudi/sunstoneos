# SPDX-License-Identifier: GPL-2.0-only
add_sunstone_application(dram-nsi
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../main.c" "${CMAKE_CURRENT_LIST_DIR}/../kernel.c"
  MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../manifest.json"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/..")
