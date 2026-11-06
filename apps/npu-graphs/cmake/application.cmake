# SunstoneOS test graphs through the OS NPU service: every graph of the model list runs once through the SDK and its
# output is compared with the simulator's. The OS image links no graph: the application carries every graph of the model list
# and registers it with the OS at run time (ss_npu_register_graph). The OS composition is that of the CPU/NPU diagnostic.
if(NOT SUNSTONE_A733_OS_CPU_RETURN OR NOT SUNSTONE_A733_OS_NPU OR SUNSTONE_A733_OS_GPU OR
   SUNSTONE_A733_OS_DMA OR SUNSTONE_A733_OS_USB_CLASSES)
  message(FATAL_ERROR "The NPU graph application requires the supervised CPU-return composition with the NPU service")
endif()
set(npu_graph_vectors "${CMAKE_CURRENT_BINARY_DIR}/npu-graph-vectors.c")
set(SUNSTONE_NPU_GRAPHS_ARTIFACTS "" CACHE FILEPATH "The artifacts map of the graphs in the model list (name -> model, input and reference files)")
set(SUNSTONE_NPU_GRAPHS_MODELS "" CACHE FILEPATH "The model list ({\"npu_models\": [names]}) of the graphs the application registers with the OS at run time")
if(NOT EXISTS "${SUNSTONE_NPU_GRAPHS_ARTIFACTS}" OR NOT EXISTS "${SUNSTONE_NPU_GRAPHS_MODELS}")
  message(FATAL_ERROR "The NPU graph application requires SUNSTONE_NPU_GRAPHS_ARTIFACTS and SUNSTONE_NPU_GRAPHS_MODELS")
endif()
# No graph is linked into the OS image: the list the vectors tool takes for linked models is empty.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/npu-graphs-linked.json" "{\"npu_models\": []}\n")
set(npu_graph_runtime_option --runtime-models "${SUNSTONE_NPU_GRAPHS_MODELS}")
set(SUNSTONE_NPU_GRAPHS_EXTERNAL_VECTORS "" CACHE FILEPATH "Vectors held by a runner input region or the card, named by address and length (tools/npu-external-pack.py)")
if(NOT SUNSTONE_NPU_GRAPHS_EXTERNAL_VECTORS STREQUAL "")
  list(APPEND npu_graph_runtime_option --external "${SUNSTONE_NPU_GRAPHS_EXTERNAL_VECTORS}")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../vectors.py"
  --artifacts "${SUNSTONE_NPU_GRAPHS_ARTIFACTS}" --models "${CMAKE_CURRENT_BINARY_DIR}/npu-graphs-linked.json" --output "${npu_graph_vectors}"
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
  if(SUNSTONE_NPU_GRAPHS_MANIFEST STREQUAL "")
    set(npu_graph_manifest "${CMAKE_CURRENT_LIST_DIR}/../manifest-queue.json")
  endif()
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
