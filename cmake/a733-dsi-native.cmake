# Private integration fragment: attach to an existing privileged native image.
# The image supplies admitted media/PLL/IOMMU producers and selected-panel hooks.
function(sunstone_a733_dsi_native target)
 target_sources(${target} PRIVATE
  "${SUNSTONE_SOURCE_ROOT}/drivers/display/a733_display_native.c"
  "${SUNSTONE_SOURCE_ROOT}/platform/a733/display_native_io.c"
  "${SUNSTONE_SOURCE_ROOT}/drivers/display/a733_display_handoff.c"
  "${SUNSTONE_SOURCE_ROOT}/drivers/display/display_scanout.c"
  "${SUNSTONE_SOURCE_ROOT}/drivers/display/a733_display_pll.c"
  "${SUNSTONE_SOURCE_ROOT}/drivers/display/a733_dsi.c"
  "${SUNSTONE_SOURCE_ROOT}/drivers/display/a733_dsi_phy.c"
  "${SUNSTONE_SOURCE_ROOT}/drivers/display/a733_dsi_video.c"
  "${SUNSTONE_SOURCE_ROOT}/drivers/display/a733_tcon_lcd.c"
  "${SUNSTONE_SOURCE_ROOT}/drivers/display/a733_display_engine.c"
  "${SUNSTONE_SOURCE_ROOT}/drivers/clock/a733_media_clock.c"
  "${SUNSTONE_SOURCE_ROOT}/drivers/iommu/a733_iommu.c"
  "${SUNSTONE_SOURCE_ROOT}/runtime/device_memory_pool.c")
endfunction()
