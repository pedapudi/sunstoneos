if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one retained DisplayPort output task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(displayport_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST displayport_parent_definitions)
 message(FATAL_ERROR "DisplayPort cannot inherit an armed board target or generated boot")
endif()
set(displayport_build "${CMAKE_CURRENT_BINARY_DIR}/displayport-application")
file(MAKE_DIRECTORY "${displayport_build}")
get_target_property(displayport_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM displayport_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${displayport_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/displayport/boot.patch")
apply_build_patch("${displayport_build}/boot.c" "${displayport_build}/boot-compact.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/displayport/extended-boot.patch")
apply_build_patch("${displayport_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/displayport/extended-link.patch")
list(APPEND displayport_sources "${displayport_build}/boot.c"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733/device_memory_layout.c")
foreach(source IN ITEMS apps/displayport/main.c apps/displayport/memory.c apps/displayport/native.c apps/displayport/output.c apps/displayport/admission.c apps/displayport/snapshot.c
 drivers/display/a733_dp.c drivers/display/a733_dp_cadence.c drivers/display/a733_dp_profile.c
 drivers/display/a733_display_engine.c drivers/display/a733_display_framebuffer.c
 drivers/power/a733_de_sys_power.c drivers/display/a733_tcon_lcd.c
 drivers/display/a733_dsi_video.c drivers/display/a733_dsi_phy.c
 drivers/clock/a733_media_clock.c drivers/clock/a733_media_clock_native.c
 drivers/iommu/a733_iommu.c)
 list(APPEND displayport_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-displayport EXCLUDE_FROM_ALL ${displayport_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(displayport_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(displayport_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(displayport_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(displayport_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER displayport_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-displayport PROPERTY COMPILE_OPTIONS "${displayport_options}")
target_include_directories(sunstone-a733-displayport BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/displayport")
target_include_directories(sunstone-a733-displayport PRIVATE ${displayport_includes})
target_compile_definitions(sunstone-a733-displayport PRIVATE ${displayport_definitions}
 SUNSTONE_BOARD_APPLICATION=1 SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1
 A733_BOARD_SUPERVISED_SECONDS=20 A733_SMP_CONTRACT_MAGIC=0x4153373344504331ULL A733_SMP_RESULT_MAGIC=0x4153373344505231ULL)
if(displayport_links)
 target_link_libraries(sunstone-a733-displayport PRIVATE ${displayport_links})
endif()
file(GLOB displayport_headers "${SUNSTONE_SOURCE_ROOT}/apps/displayport/*.h")
set(displayport_inputs ${displayport_sources} ${displayport_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/apps/displayport/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/displayport/extended-boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/displayport/extended-link.patch"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${displayport_build}/link.ld"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_displayport_application.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_dp.h"
 "${SUNSTONE_SOURCE_ROOT}/drivers/display/a733_dp_internal.h"
 "${SUNSTONE_SOURCE_ROOT}/drivers/display/a733_dp_regs.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/displayport/sources.json" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES displayport_inputs)
set(displayport_identity "runtime-native-displayport1;supervised20;RGB565-1280x720-TV1-DP4lane-RBR;PD2-C-E;private-copy;extended-NC;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS displayport_inputs)
 file(SHA256 "${input}" displayport_hash)
 string(APPEND displayport_identity "${displayport_hash}")
endforeach()
string(SHA256 displayport_build_id "${displayport_identity}")
string(REGEX REPLACE "(..)" "0x\\1," displayport_id_bytes "${displayport_build_id}")
file(WRITE "${displayport_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${displayport_id_bytes}};\n")
file(WRITE "${displayport_build}/build-id.txt" "${displayport_build_id}\n")
target_sources(sunstone-a733-displayport PRIVATE "${displayport_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${displayport_inputs})
target_link_options(sunstone-a733-displayport PRIVATE -nostdlib -static -Wl,--gc-sections
 "-Wl,-T,${displayport_build}/link.ld" "-Wl,-Map,${displayport_build}/image.map")
set_target_properties(sunstone-a733-displayport PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${displayport_build}/link.ld")

set(displayport_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@displayport_build@/manifest.json" "{\"target\":\"sunstone-a733-displayport\",\"execution_profile\":\"displayport-supervised-v1\",\"build_id\":\"@displayport_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${displayport_manifest_script}" displayport_manifest_script @ONLY)
file(WRITE "${displayport_build}/finish-manifest.cmake" "${displayport_manifest_script}")
add_custom_command(TARGET sunstone-a733-displayport POST_BUILD
 COMMAND "${CMAKE_COMMAND}" "-DIMAGE=$<TARGET_FILE:sunstone-a733-displayport>"
 -P "${displayport_build}/finish-manifest.cmake" VERBATIM)
