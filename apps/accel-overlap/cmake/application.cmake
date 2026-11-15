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
  INPUTS "${CMAKE_CURRENT_LIST_DIR}/../result.h" "${SUNSTONE_SOURCE_ROOT}/apps/queue-add/program.h"
         "${CMAKE_CURRENT_LIST_DIR}/../add4/program.h")
# The streaming sweep (main.c, ACCEL_OVERLAP_SWEEP): both accelerators under five streaming sets, with a dependent-load chase.
option(SUNSTONE_ACCEL_OVERLAP_SWEEP "Build accel-overlap as the streaming sweep" OFF)
# The larger-job sweep (ACCEL_OVERLAP_LARGE): 262,144-element GPU adds, a 1024x176 relu (the graphs directory names it), nine
# phases with device-alone controls and interleaved streams. It needs manifest-large.json (npu-app-image.py --manifest).
option(SUNSTONE_ACCEL_OVERLAP_LARGE "Build accel-overlap as the larger-job streaming sweep" OFF)
# The GPU-width run (ACCEL_OVERLAP_WIDTH): the larger-job build with the four-elements-per-lane add (add4/) and only the
# GPU-alone phase. It needs manifest-large.json, as the larger-job sweep does.
option(SUNSTONE_ACCEL_OVERLAP_WIDTH "Build accel-overlap as the GPU-alone run of the four-elements-per-lane add" OFF)
# The NSI counter calibration (ACCEL_OVERLAP_COUNTERS): the GPU-width build with four phases (idle, the GPU alone, the NPU
# alone, the core-2 stream alone), each counted on NSI ports 6 and 18 by the OS counter owner, then two 100 ms window probes.
# It needs manifest-counters.json (manifest-large.json with room for the 10,088-byte result) and the OS built with
# SUNSTONE_ACCEL_NSI_COUNTERS.
option(SUNSTONE_ACCEL_OVERLAP_COUNTERS "Build accel-overlap as the NSI port 6 and 18 counter calibration" OFF)
if(SUNSTONE_ACCEL_OVERLAP_COUNTERS)
  if(NOT SUNSTONE_ACCEL_NSI_COUNTERS)
    message(FATAL_ERROR "SUNSTONE_ACCEL_OVERLAP_COUNTERS needs the OS counter owner (SUNSTONE_ACCEL_NSI_COUNTERS)")
  endif()
  target_compile_definitions(accel-overlap PRIVATE ACCEL_OVERLAP_SWEEP=1 ACCEL_OVERLAP_LARGE=1 ACCEL_OVERLAP_WIDTH=1
                             ACCEL_OVERLAP_COUNTERS=1 SUNSTONE_ACCEL_NSI_COUNTERS=1)
  set_property(TARGET accel-overlap APPEND PROPERTY SUNSTONE_APPLICATION_INPUTS
               "${SUNSTONE_SOURCE_ROOT}/include/a733_nsi_counters.h")
elseif(SUNSTONE_ACCEL_OVERLAP_WIDTH)
  target_compile_definitions(accel-overlap PRIVATE ACCEL_OVERLAP_SWEEP=1 ACCEL_OVERLAP_LARGE=1 ACCEL_OVERLAP_WIDTH=1)
elseif(SUNSTONE_ACCEL_OVERLAP_LARGE)
  target_compile_definitions(accel-overlap PRIVATE ACCEL_OVERLAP_SWEEP=1 ACCEL_OVERLAP_LARGE=1)
elseif(SUNSTONE_ACCEL_OVERLAP_SWEEP)
  target_compile_definitions(accel-overlap PRIVATE ACCEL_OVERLAP_SWEEP=1)
endif()
