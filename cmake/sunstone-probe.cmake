# Attach the host-armed watchdog and native-probe ABI to a returning image.
function(sunstone_enable_probe target)
  get_filename_component(probe_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  target_sources(${target} PRIVATE "${probe_root}/platform/a733/probe.c" "${probe_root}/platform/a733/probe.S")
  target_include_directories(${target} PRIVATE "${probe_root}/include")
  target_compile_definitions(${target} PRIVATE SUNSTONE_BOARD_PROBE=1 SUNSTONE_CRUMBS=1 SUNSTONE_USB_RUNTIME=1)
  set_property(SOURCE "${probe_root}/platform/a733/probe.c" APPEND PROPERTY COMPILE_OPTIONS -Os -mgeneral-regs-only)
  set_property(SOURCE "${probe_root}/platform/a733/monitor.c" APPEND PROPERTY COMPILE_OPTIONS -mgeneral-regs-only)
endfunction()
