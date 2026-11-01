# The same arithmetic runs as a CPU control when the NPU service is absent.
if(NOT SUNSTONE_A733_OS_CPU_RETURN OR NOT SUNSTONE_A733_OS_CPU_CLOCK OR
   NOT A733_A76_ADMITTED OR SUNSTONE_A733_OS_GPU OR SUNSTONE_A733_OS_PERF OR
   SUNSTONE_A733_OS_AUDIO OR SUNSTONE_A733_OS_RADIO OR SUNSTONE_A733_OS_DMA OR
   SUNSTONE_A733_OS_HEADSET OR SUNSTONE_A733_OS_USB_CLASSES)
  message(FATAL_ERROR "CPU/NPU diagnostic requires its supervised CPU-return composition")
endif()
set(cpu_npu_sources "${CMAKE_CURRENT_LIST_DIR}/../main.c")
if(SUNSTONE_A733_OS_NPU)
  if(NOT SUNSTONE_A733_OS_NPU_OBSERVE)
    message(FATAL_ERROR "CPU/NPU overlap diagnostic requires opt-in NPU observation")
  endif()
  if(NOT SUNSTONE_OS_NPU_MODEL_NAME STREQUAL "shufflenet-v2-published")
    message(FATAL_ERROR "CPU/NPU overlap diagnostic requires name-selected shufflenet-v2-published")
  endif()
  set(cpu_npu_vectors "${CMAKE_CURRENT_BINARY_DIR}/cpu-npu-vectors.c")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/../vectors.py"
    --artifacts "${SUNSTONE_OS_NPU_CATALOGUE}" --models "${SUNSTONE_OS_NPU_MODELS}"
    --name "${SUNSTONE_OS_NPU_MODEL_NAME}" --output "${cpu_npu_vectors}"
    COMMAND_ERROR_IS_FATAL ANY)
  list(APPEND cpu_npu_sources "${cpu_npu_vectors}")
endif()
add_sunstone_application(cpu-npu-load
  SOURCES ${cpu_npu_sources}
  MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../manifest.json"
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/.."
  INPUTS "${CMAKE_CURRENT_LIST_DIR}/../vectors.py" "${CMAKE_CURRENT_LIST_DIR}/../vectors.h" "${CMAKE_CURRENT_LIST_DIR}/../receipt.h" "${CMAKE_CURRENT_LIST_DIR}/../gemm.h")
target_compile_options(cpu-npu-load PRIVATE -ffp-contract=off)
# OS/service code intentionally uses general registers. This FP application
# uses the already-enabled/saved task FP context; override those two target
# features for this source only, after inherited service options.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/../main.c" PROPERTIES
  COMPILE_OPTIONS "-Xclang;-target-feature;-Xclang;+fp-armv8;-Xclang;-target-feature;-Xclang;+neon")
