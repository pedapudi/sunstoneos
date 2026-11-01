include_guard(GLOBAL)
option(SUNSTONE_LATENCY "Record maximum kernel and device response times" OFF)
set_source_files_properties("${SUNSTONE_SOURCE_ROOT}/runtime/latency.c"
  "${SUNSTONE_SOURCE_ROOT}/runtime/latency_irq.c" PROPERTIES COMPILE_OPTIONS -mgeneral-regs-only)
function(sunstone_enable_latency target)
  target_sources(${target} PRIVATE
    "${SUNSTONE_SOURCE_ROOT}/runtime/latency.c"
    "${SUNSTONE_SOURCE_ROOT}/runtime/latency_irq.c")
  target_compile_definitions(${target} PRIVATE SUNSTONE_LATENCY=1)
  target_link_options(${target} PRIVATE -Wl,--wrap=vApplicationIRQHandler)
endfunction()
