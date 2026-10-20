if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one retained two-picture H264 encoder task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(h264_encode_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST h264_encode_parent_definitions)
 message(FATAL_ERROR "H264_ENCODE cannot inherit an armed board target or generated boot")
endif()
set(h264_encode_build "${CMAKE_CURRENT_BINARY_DIR}/h264_encode-application")
file(MAKE_DIRECTORY "${h264_encode_build}")
get_target_property(h264_encode_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM h264_encode_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${h264_encode_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode/boot.patch")
apply_build_patch("${h264_encode_build}/boot.c" "${h264_encode_build}/boot-compact.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode/extended-boot.patch")
apply_build_patch("${h264_encode_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode/extended-link.patch")
list(APPEND h264_encode_sources "${h264_encode_build}/boot.c"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733/device_memory_layout.c")
foreach(source IN ITEMS apps/h264-encode/main.c apps/h264-encode/input.c apps/h264-encode/storage.c apps/h264-encode/native.c apps/h264-encode/usb0.c apps/h264-encode/admission.c apps/h264-encode/snapshot.c
 drivers/video/a733_video_h264_encode.c drivers/video/a733_video_encode_platform.c
 drivers/iommu/a733_iommu.c)
 list(APPEND h264_encode_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-h264-encode EXCLUDE_FROM_ALL ${h264_encode_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(h264_encode_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(h264_encode_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(h264_encode_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(h264_encode_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER h264_encode_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-h264-encode PROPERTY COMPILE_OPTIONS "${h264_encode_options}")
target_include_directories(sunstone-a733-h264-encode BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode")
target_include_directories(sunstone-a733-h264-encode PRIVATE ${h264_encode_includes})
target_compile_definitions(sunstone-a733-h264-encode PRIVATE ${h264_encode_definitions}
 SUNSTONE_BOARD_APPLICATION=1 SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1 A733_SMP_CONTRACT_MAGIC=0x4153373348454331ULL A733_SMP_RESULT_MAGIC=0x4153373348455231ULL)
if(h264_encode_links)
 target_link_libraries(sunstone-a733-h264-encode PRIVATE ${h264_encode_links})
endif()
file(GLOB h264_encode_headers "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode/*.h")
set(h264_encode_inputs ${h264_encode_sources} ${h264_encode_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode/extended-boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264-encode/extended-link.patch"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${h264_encode_build}/link.ld"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_h264_encode_application.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_video_h264_encode.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_video_encode_platform.h" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES h264_encode_inputs)
set(h264_encode_identity "runtime-native-h264-encode1;supervised60;two-VGA-NV12-Baseline-CAVLC-QP26;private-copy;extended-NC-guards;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS h264_encode_inputs)
 file(SHA256 "${input}" h264_encode_hash)
 string(APPEND h264_encode_identity "${h264_encode_hash}")
endforeach()
string(SHA256 h264_encode_build_id "${h264_encode_identity}")
string(REGEX REPLACE "(..)" "0x\\1," h264_encode_id_bytes "${h264_encode_build_id}")
file(WRITE "${h264_encode_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${h264_encode_id_bytes}};\n")
file(WRITE "${h264_encode_build}/build-id.txt" "${h264_encode_build_id}\n")
target_sources(sunstone-a733-h264-encode PRIVATE "${h264_encode_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${h264_encode_inputs})
target_link_options(sunstone-a733-h264-encode PRIVATE -nostdlib -static -Wl,--gc-sections
 "-Wl,-T,${h264_encode_build}/link.ld" "-Wl,-Map,${h264_encode_build}/image.map")
set_target_properties(sunstone-a733-h264-encode PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${h264_encode_build}/link.ld")
