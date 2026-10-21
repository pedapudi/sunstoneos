# Same supervised board provider composition with one retained G2D fill or source-copy task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(G2D)
set(g2d_build "${CMAKE_CURRENT_BINARY_DIR}/g2d-application")
file(MAKE_DIRECTORY "${g2d_build}")
apply_build_patch("${g2d_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/g2d/boot.patch")
apply_build_patch("${g2d_build}/boot.c" "${g2d_build}/boot-compact.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/g2d/extended-boot.patch")
apply_build_patch("${g2d_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/g2d/extended-link.patch")
sunstone_board_application(g2d BOOT "${g2d_build}/boot.c" LINK_SCRIPT "${g2d_build}/link.ld"
 TAG "runtime-native-g2d1;supervised20;ARGB8888-fill-copy-max1280x720;private-copy;extended-NC-guards;retained-DE_SYS-on"
 CONTRACT_MAGIC 0x4153373347324331ULL RESULT_MAGIC 0x4153373347325231ULL
 DEFINITIONS SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1 A733_BOARD_SUPERVISED_SECONDS=20
 PROFILE g2d-supervised-v1
 SOURCES platform/a733/device_memory_layout.c apps/g2d/main.c apps/g2d/input.c apps/g2d/memory.c
  apps/g2d/native.c apps/g2d/usb.c apps/g2d/admission.c apps/g2d/snapshot.c drivers/g2d/a733_g2d.c
  drivers/iommu/a733_iommu.c drivers/power/a733_de_sys_power.c
 INPUTS apps/g2d/extended-boot.patch apps/g2d/extended-link.patch
  platform/a733-smp-cached/link-devices.ld include/a733_g2d_application.h include/a733_g2d.h
  include/a733_de_sys_power.h drivers/g2d/g2d_mixer_type.h apps/g2d/sources.json)
