# Same supervised board provider composition with one retained DisplayPort output task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(DisplayPort)
set(displayport_build "${CMAKE_CURRENT_BINARY_DIR}/displayport-application")
file(MAKE_DIRECTORY "${displayport_build}")
apply_build_patch("${displayport_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/displayport/boot.patch")
apply_build_patch("${displayport_build}/boot.c" "${displayport_build}/boot-compact.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/displayport/extended-boot.patch")
apply_build_patch("${displayport_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/displayport/extended-link.patch")
sunstone_board_application(displayport BOOT "${displayport_build}/boot.c"
 LINK_SCRIPT "${displayport_build}/link.ld"
 TAG "runtime-native-displayport1;supervised20;RGB565-1280x720-TV1-DP4lane-RBR;PD2-C-E;private-copy;extended-NC"
 CONTRACT_MAGIC 0x4153373344504331ULL RESULT_MAGIC 0x4153373344505231ULL
 DEFINITIONS SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1 A733_BOARD_SUPERVISED_SECONDS=20
 PROFILE displayport-supervised-v1
 SOURCES platform/a733/device_memory_layout.c apps/displayport/main.c apps/displayport/memory.c
  apps/displayport/native.c apps/displayport/output.c apps/displayport/admission.c
  apps/displayport/snapshot.c drivers/display/a733_dp.c drivers/display/a733_dp_cadence.c
  drivers/display/a733_dp_profile.c drivers/display/a733_display_engine.c
  drivers/display/a733_display_framebuffer.c
  drivers/display/a733_tcon_lcd.c drivers/display/a733_dsi_video.c drivers/display/a733_dsi_phy.c
  drivers/clock/a733_media_clock.c drivers/clock/a733_media_clock_native.c
  drivers/iommu/a733_iommu.c
 INPUTS apps/displayport/extended-boot.patch apps/displayport/extended-link.patch
  platform/a733-smp-cached/link-devices.ld include/a733_displayport_application.h include/a733_dp.h
  drivers/display/a733_dp_internal.h drivers/display/a733_dp_regs.h apps/displayport/sources.json)
