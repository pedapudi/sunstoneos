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
# The streaming sweep (main.c, ACCEL_OVERLAP_SWEEP): both accelerators under five streaming sets, with a dependent-load chase.
option(SUNSTONE_ACCEL_OVERLAP_SWEEP "Build accel-overlap as the streaming sweep" OFF)
# The larger-job sweep (ACCEL_OVERLAP_LARGE): 262,144-element GPU adds, a 1024x176 relu (the graphs directory names it), nine
# phases with device-alone controls and interleaved streams. It needs manifest-large.json (npu-app-image.py --manifest).
option(SUNSTONE_ACCEL_OVERLAP_LARGE "Build accel-overlap as the larger-job streaming sweep" OFF)
if(SUNSTONE_ACCEL_OVERLAP_LARGE)
  target_compile_definitions(accel-overlap PRIVATE ACCEL_OVERLAP_SWEEP=1 ACCEL_OVERLAP_LARGE=1)
elseif(SUNSTONE_ACCEL_OVERLAP_SWEEP)
  target_compile_definitions(accel-overlap PRIVATE ACCEL_OVERLAP_SWEEP=1)
endif()
