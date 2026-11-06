# The same arithmetic runs as a CPU control when the application is built without its graph.
if(SUNSTONE_A733_OS_GPU OR
   SUNSTONE_A733_OS_DMA OR SUNSTONE_A733_OS_USB_CLASSES)
  message(FATAL_ERROR "The CPU/NPU diagnostic requires an OS image without the GPU, DMA and USB class services")
endif()
set(cpu_npu_sources "${CMAKE_CURRENT_LIST_DIR}/../main.c")
# Neither the OS image nor the application links the graph or the input: the runner uploads them as the manifest's two input
# regions (tools/npu-app-image.py packs them) and the application registers the graph from the first at run time. The artifacts
# give the application its reference output. With them the application declares the NPU in its manifest, and its session
# grants the NPU service; without them it is the CPU control.
set(SUNSTONE_CPU_NPU_ARTIFACTS "" CACHE FILEPATH "Artifacts map of the reviewed shufflenet-v2-published graph and vectors (empty: the CPU control)")
set(cpu_npu_manifest "${CMAKE_CURRENT_LIST_DIR}/../manifest.json")
set(SUNSTONE_CPU_NPU_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../manifest-npu.json" CACHE FILEPATH "Manifest of the combined profile (tools/npu-app-image.py passes the one with the regions' digests)")
if(NOT SUNSTONE_CPU_NPU_ARTIFACTS STREQUAL "")
  if(NOT EXISTS "${SUNSTONE_CPU_NPU_ARTIFACTS}")
    message(FATAL_ERROR "CPU/NPU overlap diagnostic: SUNSTONE_CPU_NPU_ARTIFACTS names no file")
  endif()
  set(cpu_npu_manifest "${SUNSTONE_CPU_NPU_MANIFEST}")
  set(cpu_npu_vectors "${CMAKE_CURRENT_BINARY_DIR}/cpu-npu-vectors.c")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../vectors.py"
    --artifacts "${SUNSTONE_CPU_NPU_ARTIFACTS}" --models "${SUNSTONE_SOURCE_ROOT}/checks/data/npu-test-model-list.json"
    --name shufflenet-v2-published --output "${cpu_npu_vectors}"
    COMMAND_ERROR_IS_FATAL ANY)
  list(APPEND cpu_npu_sources "${cpu_npu_vectors}")
endif()
add_sunstone_application(cpu-npu-load
  SOURCES ${cpu_npu_sources}
  MANIFEST "${cpu_npu_manifest}"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/.."
  INPUTS "${CMAKE_CURRENT_LIST_DIR}/../vectors.py" "${CMAKE_CURRENT_LIST_DIR}/../vectors.h" "${CMAKE_CURRENT_LIST_DIR}/../receipt.h" "${CMAKE_CURRENT_LIST_DIR}/../gemm.h")
target_compile_options(cpu-npu-load PRIVATE -ffp-contract=off)
if(NOT SUNSTONE_CPU_NPU_ARTIFACTS STREQUAL "")
  target_compile_definitions(cpu-npu-load PRIVATE CPU_NPU_WITH_NPU=1)
endif()
# OS/service code intentionally uses general registers. This FP application
# uses the already-enabled/saved task FP context; override those two target
# features for this source only, after inherited service options.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/../main.c" PROPERTIES
  COMPILE_OPTIONS "-Xclang;-target-feature;-Xclang;+fp-armv8;-Xclang;-target-feature;-Xclang;+neon")
