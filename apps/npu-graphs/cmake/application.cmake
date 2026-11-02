# SunstoneOS test graphs through the OS NPU service: every graph of the model list runs once through the SDK and its
# output is compared with the simulator's. The OS composition is that of the CPU/NPU diagnostic.
if(NOT SUNSTONE_A733_OS_CPU_RETURN OR NOT SUNSTONE_A733_OS_CPU_CLOCK OR NOT SUNSTONE_A733_OS_NPU OR
   NOT SUNSTONE_OS_NPU_ALL_MODELS OR NOT A733_A76_ADMITTED OR SUNSTONE_A733_OS_GPU OR SUNSTONE_A733_OS_PERF OR
   SUNSTONE_A733_OS_AUDIO OR SUNSTONE_A733_OS_RADIO OR SUNSTONE_A733_OS_DMA OR
   SUNSTONE_A733_OS_HEADSET OR SUNSTONE_A733_OS_USB_CLASSES)
  message(FATAL_ERROR "The NPU graph application requires the supervised CPU-return composition with the NPU service and all listed models")
endif()
set(npu_graph_vectors "${CMAKE_CURRENT_BINARY_DIR}/npu-graph-vectors.c")
set(SUNSTONE_NPU_GRAPHS_RUNTIME_MODELS "" CACHE STRING "A model list of graphs the application registers with the OS at run time")
set(npu_graph_runtime_option)
if(NOT SUNSTONE_NPU_GRAPHS_RUNTIME_MODELS STREQUAL "")
  set(npu_graph_runtime_option --runtime-models "${SUNSTONE_NPU_GRAPHS_RUNTIME_MODELS}")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../vectors.py"
  --artifacts "${SUNSTONE_OS_NPU_CATALOGUE}" --models "${SUNSTONE_OS_NPU_MODELS}" --output "${npu_graph_vectors}"
  ${npu_graph_runtime_option}
  COMMAND_ERROR_IS_FATAL ANY)
# The backpressure study: the application opens the EL0 ring on the routed core (a Cortex-A55) and declares the 31 slots
# and the 64 in-flight jobs the queue image holds. The image is built with SUNSTONE_A733_OS_NPU_QUEUE.
set(SUNSTONE_NPU_GRAPHS_QUEUE OFF CACHE BOOL "Run the EL0 queue backpressure study instead of the one-run-each loop")
# A build of graphs that need other resources than the default declaration names a manifest of its own.
set(SUNSTONE_NPU_GRAPHS_MANIFEST "" CACHE FILEPATH "Manifest for the graphs application (empty: apps/npu-graphs/manifest.json)")
set(npu_graph_manifest "${CMAKE_CURRENT_LIST_DIR}/../manifest.json")
if(NOT SUNSTONE_NPU_GRAPHS_MANIFEST STREQUAL "")
  set(npu_graph_manifest "${SUNSTONE_NPU_GRAPHS_MANIFEST}")
endif()
if(SUNSTONE_NPU_GRAPHS_QUEUE)
  if(NOT SUNSTONE_A733_OS_NPU_QUEUE)
    message(FATAL_ERROR "SUNSTONE_NPU_GRAPHS_QUEUE requires SUNSTONE_A733_OS_NPU_QUEUE")
  endif()
  set(npu_graph_manifest "${CMAKE_CURRENT_LIST_DIR}/../manifest-queue.json")
endif()
add_sunstone_application(npu-graphs
  SOURCES "${CMAKE_CURRENT_LIST_DIR}/../main.c" "${npu_graph_vectors}"
  MANIFEST "${npu_graph_manifest}"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/.."
  INPUTS "${CMAKE_CURRENT_LIST_DIR}/../vectors.py" "${CMAKE_CURRENT_LIST_DIR}/../vectors.h" "${CMAKE_CURRENT_LIST_DIR}/../receipt.h")
set(SUNSTONE_NPU_GRAPHS_QUEUE_DEFS "" CACHE STRING "Definitions for the queue study: QUEUE_JOBS=n, QUEUE_KMAX=n, NPU_GRAPHS_QUEUE_NO_LINK=1")
if(SUNSTONE_NPU_GRAPHS_QUEUE)
  target_compile_definitions(npu-graphs PRIVATE NPU_GRAPHS_QUEUE=1 ${SUNSTONE_NPU_GRAPHS_QUEUE_DEFS})
endif()
