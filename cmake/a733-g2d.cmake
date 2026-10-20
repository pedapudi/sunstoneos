if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one retained G2D fill or source-copy task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(g2d_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST g2d_parent_definitions)
 message(FATAL_ERROR "G2D cannot inherit an armed board target or generated boot")
endif()
set(g2d_build "${CMAKE_CURRENT_BINARY_DIR}/g2d-application")
file(MAKE_DIRECTORY "${g2d_build}")
get_target_property(g2d_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM g2d_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${g2d_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/g2d/boot.patch")
apply_build_patch("${g2d_build}/boot.c" "${g2d_build}/boot-compact.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/g2d/extended-boot.patch")
apply_build_patch("${g2d_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/g2d/extended-link.patch")
list(APPEND g2d_sources "${g2d_build}/boot.c"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733/device_memory_layout.c")
foreach(source IN ITEMS apps/g2d/main.c apps/g2d/input.c apps/g2d/memory.c apps/g2d/native.c apps/g2d/usb.c apps/g2d/admission.c apps/g2d/snapshot.c
 drivers/g2d/a733_g2d.c drivers/iommu/a733_iommu.c)
 list(APPEND g2d_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-g2d EXCLUDE_FROM_ALL ${g2d_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(g2d_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(g2d_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(g2d_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(g2d_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER g2d_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-g2d PROPERTY COMPILE_OPTIONS "${g2d_options}")
target_include_directories(sunstone-a733-g2d BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/g2d")
target_include_directories(sunstone-a733-g2d PRIVATE ${g2d_includes})
target_compile_definitions(sunstone-a733-g2d PRIVATE ${g2d_definitions}
 SUNSTONE_BOARD_APPLICATION=1 SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1
 A733_BOARD_SUPERVISED_SECONDS=20 A733_SMP_CONTRACT_MAGIC=0x4153373347324331ULL A733_SMP_RESULT_MAGIC=0x4153373347325231ULL)
if(g2d_links)
 target_link_libraries(sunstone-a733-g2d PRIVATE ${g2d_links})
endif()
file(GLOB g2d_headers "${SUNSTONE_SOURCE_ROOT}/apps/g2d/*.h")
set(g2d_inputs ${g2d_sources} ${g2d_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/apps/g2d/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/g2d/extended-boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/g2d/extended-link.patch"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${g2d_build}/link.ld"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_g2d_application.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_g2d.h"
 "${SUNSTONE_SOURCE_ROOT}/drivers/g2d/g2d_mixer_type.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/g2d/sources.json" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES g2d_inputs)
set(g2d_identity "runtime-native-g2d1;supervised20;ARGB8888-fill-copy-max1280x720;private-copy;extended-NC-guards;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS g2d_inputs)
 file(SHA256 "${input}" g2d_hash)
 string(APPEND g2d_identity "${g2d_hash}")
endforeach()
string(SHA256 g2d_build_id "${g2d_identity}")
string(REGEX REPLACE "(..)" "0x\\1," g2d_id_bytes "${g2d_build_id}")
file(WRITE "${g2d_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${g2d_id_bytes}};\n")
file(WRITE "${g2d_build}/build-id.txt" "${g2d_build_id}\n")
target_sources(sunstone-a733-g2d PRIVATE "${g2d_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${g2d_inputs})
target_link_options(sunstone-a733-g2d PRIVATE -nostdlib -static -Wl,--gc-sections
 "-Wl,-T,${g2d_build}/link.ld" "-Wl,-Map,${g2d_build}/image.map")
set_target_properties(sunstone-a733-g2d PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${g2d_build}/link.ld")

set(g2d_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@g2d_build@/manifest.json" "{\"target\":\"sunstone-a733-g2d\",\"execution_profile\":\"g2d-supervised-v1\",\"build_id\":\"@g2d_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${g2d_manifest_script}" g2d_manifest_script @ONLY)
file(WRITE "${g2d_build}/finish-manifest.cmake" "${g2d_manifest_script}")
add_custom_command(TARGET sunstone-a733-g2d POST_BUILD
 COMMAND "${CMAKE_COMMAND}" "-DIMAGE=$<TARGET_FILE:sunstone-a733-g2d>"
 -P "${g2d_build}/finish-manifest.cmake" VERBATIM)
