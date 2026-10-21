# Same supervised board provider composition with one retained DI301 WEAVE, initial MOTION or fixed-strength TNR task.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board-application.cmake")
sunstone_board_application_parent(DEINTERLACE)
set(deinterlace_build "${CMAKE_CURRENT_BINARY_DIR}/deinterlace-application")
file(MAKE_DIRECTORY "${deinterlace_build}")
apply_build_patch("${deinterlace_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/boot.patch")
apply_build_patch("${deinterlace_build}/boot.c" "${deinterlace_build}/boot-compact.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/extended-boot.patch")
apply_build_patch("${deinterlace_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/extended-link.patch")
set(di_generated "${deinterlace_build}/di301")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/adapt.py"
 --cache "$ENV{HOME}/.cache/sunstoneos/sources/deinterlace-native" --output "${di_generated}"
 RESULT_VARIABLE di_adapt_status)
if(NOT di_adapt_status EQUAL 0)
 message(FATAL_ERROR "Pinned DI301 configuration adaptation failed")
endif()
# Original Linux debug-only locals are unused when diagnostics are disabled.
set_source_files_properties("${SUNSTONE_SOURCE_ROOT}/drivers/deinterlace/a733_deinterlace.c"
 "${di_generated}/di301_alg.c" "${di_generated}/di-formats.c" PROPERTIES COMPILE_OPTIONS "-w")
file(GLOB di_generated_inputs "${di_generated}/*")
sunstone_board_application(deinterlace BOOT "${deinterlace_build}/boot.c"
 LINK_SCRIPT "${deinterlace_build}/link.ld"
 TAG "runtime-native-di301-weave-initial-motion1;supervised20;NV12-640x480;private-copy;extended-NC-guards"
 CONTRACT_MAGIC 0x4153373344494331ULL RESULT_MAGIC 0x4153373344495231ULL
 DEFINITIONS SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1 A733_BOARD_SUPERVISED_SECONDS=20
 PROFILE deinterlace-supervised-v1 WORKLOAD copied-NV12-offline-weave-motion-or-fixed-tnr
 SOURCES platform/a733/device_memory_layout.c apps/deinterlace/main.c apps/deinterlace/input.c
  apps/deinterlace/memory.c apps/deinterlace/native.c apps/deinterlace/usb.c
  apps/deinterlace/admission.c apps/deinterlace/snapshot.c drivers/deinterlace/a733_deinterlace.c
  drivers/iommu/a733_iommu.c "${di_generated}/di301_alg.c" "${di_generated}/di-formats.c"
 INCLUDES "${di_generated}" drivers/deinterlace
 INPUTS ${di_generated_inputs} apps/deinterlace/adapt.py apps/deinterlace/extended-boot.patch
  apps/deinterlace/extended-link.patch platform/a733-smp-cached/link-devices.ld
  include/a733_deinterlace_application.h include/a733_deinterlace.h drivers/deinterlace/di-port.h
  apps/deinterlace/sources.json)
