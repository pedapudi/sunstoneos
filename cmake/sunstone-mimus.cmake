# Transitional in-tree registration. The external application uses these same
# public kit functions after its sources move to MIMUS_DIR/sunstoneos.
if(NOT TARGET mimus_asr_smp)
  message(FATAL_ERROR "The mimus application requires its pinned MIMUS_DIR and SMP engine")
endif()
set(mimus_app_root "${SUNSTONE_SOURCE_ROOT}/apps/mimus-board")
set(mimus_common "${SUNSTONE_SOURCE_ROOT}/apps/mimus")
set(mimus_app_build "${CMAKE_CURRENT_BINARY_DIR}/mimus-board-application")
file(MAKE_DIRECTORY "${mimus_app_build}")
file(READ "${mimus_app_root}/manifest.json" manifest)
math(EXPR workers "${SUNSTONE_MIMUS_WORKERS}-1")
string(JSON manifest SET "${manifest}" cpu workers ${workers})
if(NOT A733_A76_ADMITTED)
  string(JSON manifest SET "${manifest}" cpu cluster "\"a55\"")
endif()
# Every OS variant uses the application's 12 MiB arena. The optional GPU
# output reservation stays in the manifest when the GPU is not granted.
file(WRITE "${mimus_app_build}/manifest-input.json" "${manifest}\n")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${mimus_app_root}/configure.py"
  --root "${SUNSTONE_SOURCE_ROOT}" --output "${mimus_app_build}" COMMAND_ERROR_IS_FATAL ANY)
set(mimus_sources "${mimus_app_root}/recognition-ss.cc" "${mimus_app_root}/ss-host.cc"
  "${mimus_app_root}/keyboard.c" "${mimus_common}/recorded.cc"
  "${mimus_common}/platform.cc" "${mimus_common}/parallel_runtime.cc"
  "${MIMUS_DIR}/firmware/components/pipeline/src/formatter.cc")
set(mimus_inputs "${mimus_app_root}/manifest.json" "${mimus_app_root}/recognition-ss.cc")
if(SUNSTONE_A733_OS_GPU_LINEAR)
  if(NOT SUNSTONE_A733_OS_MODEL_DIR)
    message(FATAL_ERROR "GPU recognition requires SUNSTONE_A733_OS_MODEL_DIR")
  endif()
  set(cores a55)
  if(A733_A76_ADMITTED)
    set(cores a76)
  endif()
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${SUNSTONE_SOURCE_ROOT}/apps/os/placement.py"
    --models "${SUNSTONE_A733_OS_MODEL_DIR}/base.mimus" "${SUNSTONE_A733_OS_MODEL_DIR}/overlay.mimus"
    --cores ${cores} --output "${os_build}/placement.h" COMMAND_ERROR_IS_FATAL ANY)
  list(APPEND mimus_sources "${SUNSTONE_SOURCE_ROOT}/apps/os/dispatch.c"
    "${SUNSTONE_SOURCE_ROOT}/apps/os/gpu_hook.cc")
  list(APPEND mimus_inputs "${SUNSTONE_A733_OS_MODEL_DIR}/base.mimus"
    "${SUNSTONE_A733_OS_MODEL_DIR}/overlay.mimus" "${os_build}/placement.h"
    "${SUNSTONE_SOURCE_ROOT}/apps/os/placement.py")
endif()
file(GLOB mimus_headers "${mimus_app_root}/*.h" "${mimus_common}/*.h")
add_sunstone_application(mimus SOURCES ${mimus_sources}
  MANIFEST "${mimus_app_build}/manifest-input.json"
  LIBRARIES mimus_asr_smp INCLUDE_DIRECTORIES "${mimus_app_root}" "${mimus_app_build}"
    "${MIMUS_DIR}/firmware/components/pipeline/include" "${os_build}"
  INPUTS ${mimus_inputs} ${mimus_headers})
target_compile_definitions(mimus PRIVATE SUNSTONE_MIMUS_NATIVE=1 SUNSTONE_MIMUS_SUPERVISED=1)
