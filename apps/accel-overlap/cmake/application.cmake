# Concurrent GPU, NPU and CPU streaming work (main.c), with the completion latency of both accelerators on one counter. The OS
# takes the relu graph from the models region at admission (npu.graph_region); tools/npu-app-image.py writes the manifest.
if(NOT SUNSTONE_A733_OS_GPU OR NOT SUNSTONE_GPU_FAST_TIMING OR NOT SUNSTONE_NPU_WAKE_TIMING OR NOT SUNSTONE_ACCEL_PLL_WATCH)
  message(FATAL_ERROR "accel-overlap needs SUNSTONE_A733_OS_GPU, SUNSTONE_GPU_FAST_TIMING, SUNSTONE_NPU_WAKE_TIMING and "
                      "SUNSTONE_ACCEL_PLL_WATCH: it reads both completion stamps and the PLL_NPU watch")
endif()
if(SUNSTONE_A733_OS_DMA OR SUNSTONE_A733_OS_USB_CLASSES)
  message(FATAL_ERROR "accel-overlap runs without the DMA and USB class services")
endif()
set(SUNSTONE_ACCEL_OVERLAP_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../manifest.json" CACHE FILEPATH "Manifest of the accel-overlap application")
add_sunstone_application(accel-overlap
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../main.c"
  MANIFEST "${SUNSTONE_ACCEL_OVERLAP_MANIFEST}"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/.."
  INPUTS "${CMAKE_CURRENT_LIST_DIR}/../result.h" "${SUNSTONE_SOURCE_ROOT}/apps/queue-add/program.h")
