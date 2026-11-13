# NPU job timing (main.c): the OS takes the graphs from the models region at admission (npu.graph_region); the application times
# single and pipelined jobs of each through the EL0 ring. The manifest is written by tools/npu-app-image.py.
if(SUNSTONE_A733_OS_GPU OR SUNSTONE_A733_OS_DMA OR SUNSTONE_A733_OS_USB_CLASSES)
  message(FATAL_ERROR "The NPU timing application requires an OS image without the GPU, DMA or USB class services")
endif()
set(SUNSTONE_NPU_TIMING_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../manifest.json" CACHE FILEPATH "Manifest of the NPU timing application")
add_sunstone_application(npu-timing
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../main.c"
  MANIFEST "${SUNSTONE_NPU_TIMING_MANIFEST}"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/.."
  INPUTS "${CMAKE_CURRENT_LIST_DIR}/../timing.h")
