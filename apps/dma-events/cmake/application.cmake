# Include after sunstone-os-core.cmake and sunstone-image.cmake.
if(NOT SUNSTONE_A733_OS_DMA)
  message(FATAL_ERROR "dma-events needs SUNSTONE_A733_OS_DMA=ON")
endif()
add_sunstone_application(dma-events
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../main.c"
  MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../manifest.json"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/..")
add_sunstone_image(sunstone-a733-os-dma-events LIFETIME return APPLICATION dma-events)
