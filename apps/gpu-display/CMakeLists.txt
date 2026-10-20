# The root-owned target supplies the retained session, architecture admission,
# extended memory and existing GPU/HDMI/DE/IOMMU/provider dependency units.
function(sunstone_gpu_display_application target)
  target_sources(${target} PRIVATE
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/main.c"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/input.c"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/render.c"
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/display.c")
  target_include_directories(${target} BEFORE PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  target_compile_definitions(${target} PRIVATE SUNSTONE_GPU_DISPLAY=1 SUNSTONE_HDMI_FRAMEBUFFER=1)
endfunction()
