if(SUNSTONE_APPLICATION STREQUAL "latency-dma")
  set(latency_name latency-dma)
  set(latency_manifest "${CMAKE_CURRENT_LIST_DIR}/../manifest-dma.json")
else()
  set(latency_name latency)
  set(latency_manifest "${CMAKE_CURRENT_LIST_DIR}/../manifest.json")
endif()
add_sunstone_application(${latency_name}
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../native.c" "${CMAKE_CURRENT_LIST_DIR}/../workload.c"
  MANIFEST "${latency_manifest}"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/..")
target_compile_definitions(${latency_name} PRIVATE SUNSTONE_LATENCY=1)
target_compile_options(${latency_name} PRIVATE "$<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>")
if(latency_name STREQUAL "latency-dma")
  target_compile_definitions(${latency_name} PRIVATE SUNSTONE_LATENCY_DMA=1)
endif()
