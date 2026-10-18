# Native fixed convolution with explicit supervised, retained recovery.
# This selects the same application/runtime composition as the standalone build;
# the host must supply the authenticated session page before any launch.
set(A733_NPU_SUPERVISED ON)
add_subdirectory("${SUNSTONE_SOURCE_ROOT}/apps/npu-convolution"
  "${CMAKE_CURRENT_BINARY_DIR}/npu-supervised")
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-npu-single-core.cmake")
