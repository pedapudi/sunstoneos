option(SUNSTONE_H264_HIGH "Runtime bounded High/CABAC short-term I/P/B profile" OFF)
set(SUNSTONE_H264_GSTREAMER_SOURCE "" CACHE PATH "Pinned GStreamer H264 standalone source cache")
if(SUNSTONE_H264_HIGH)
 set(SUNSTONE_H264_FULLHD ON CACHE BOOL "High uses existing extended memory" FORCE)
endif()
option(SUNSTONE_H264_FULLHD "Admitted retained full-HD extended-memory profile" OFF)
if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one bounded native H.264 task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(h264_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST h264_parent_definitions)
 message(FATAL_ERROR "H.264 cannot inherit an armed board target or generated boot")
endif()
set(h264_build "${CMAKE_CURRENT_BINARY_DIR}/h264-application")
file(MAKE_DIRECTORY "${h264_build}")
get_target_property(h264_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM h264_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${h264_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264/fullhd-boot.patch" "${SUNSTONE_SOURCE_ROOT}/apps/h264/fullhd-link.patch"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld" "${h264_linker}")
if(SUNSTONE_H264_FULLHD)
 apply_build_patch("${h264_build}/boot.c" "${h264_build}/boot-compact.c"
  "${SUNSTONE_SOURCE_ROOT}/apps/h264/fullhd-boot.patch")
 apply_build_patch("${h264_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
  "${SUNSTONE_SOURCE_ROOT}/apps/h264/fullhd-link.patch")
 set(h264_linker "${h264_build}/link.ld")
 list(APPEND h264_sources "${SUNSTONE_SOURCE_ROOT}/platform/a733/device_memory_layout.c")
else()
 configure_file("${h264_build}/boot-compact.c" "${h264_build}/boot.c" COPYONLY)
 set(h264_linker "${storage_build}/link.ld")
endif()
list(APPEND h264_sources "${h264_build}/boot.c")
foreach(source IN ITEMS apps/h264/main.c apps/h264/input.c apps/h264/stream.c apps/h264/stream-storage.c apps/h264/stream-usb.c apps/h264/native.c apps/h264/usb0.c apps/h264/admission.c apps/h264/snapshot.c
 drivers/video/a733_video_h264.c drivers/video/a733_video_platform.c
 drivers/iommu/a733_iommu.c)
 list(APPEND h264_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
if(SUNSTONE_H264_HIGH)
 if(NOT EXISTS "${SUNSTONE_H264_GSTREAMER_SOURCE}/pins.json")
  message(FATAL_ERROR "High profile requires pinned standalone GStreamer source cache")
 endif()
 set(h264_gst "${h264_build}/gst")
 execute_process(COMMAND "${Python3_EXECUTABLE}" "${SUNSTONE_SOURCE_ROOT}/apps/h264/gst-source.py"
  --source "${SUNSTONE_H264_GSTREAMER_SOURCE}" --output "${h264_gst}" COMMAND_ERROR_IS_FATAL ANY)
 list(APPEND h264_sources "${SUNSTONE_SOURCE_ROOT}/apps/h264/high-parse.c" "${h264_gst}/gst-h264-parser.c" "${h264_gst}/gst-nal.c")
endif()
add_executable(sunstone-a733-h264 EXCLUDE_FROM_ALL ${h264_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(h264_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(h264_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(h264_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(h264_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER h264_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-h264 PROPERTY COMPILE_OPTIONS "${h264_options}")
target_include_directories(sunstone-a733-h264 BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/h264")
target_include_directories(sunstone-a733-h264 PRIVATE ${h264_includes})
target_compile_definitions(sunstone-a733-h264 PRIVATE ${h264_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x4153373348324331ULL A733_SMP_RESULT_MAGIC=0x4153373348325231ULL)
if(SUNSTONE_H264_FULLHD)
 target_compile_definitions(sunstone-a733-h264 PRIVATE SUNSTONE_H264_FULLHD=1 SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1)
 target_link_options(sunstone-a733-h264 PRIVATE -Wl,--undefined=h264_fullhd_geometry)
endif()
if(SUNSTONE_H264_HIGH)
 target_compile_definitions(sunstone-a733-h264 PRIVATE SUNSTONE_H264_HIGH=1)
 target_include_directories(sunstone-a733-h264 PRIVATE "${h264_gst}")
 target_link_options(sunstone-a733-h264 PRIVATE -Wl,--undefined=h264_high_geometry)
endif()
if(h264_links)
 target_link_libraries(sunstone-a733-h264 PRIVATE ${h264_links})
endif()
file(GLOB h264_headers "${SUNSTONE_SOURCE_ROOT}/apps/h264/*.h")
set(h264_inputs ${h264_sources} ${h264_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/include/a733_h264_application.h" "${CMAKE_CURRENT_LIST_FILE}"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_video_h264.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_video_platform.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264/sources.json"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264/fullhd-boot.patch" "${SUNSTONE_SOURCE_ROOT}/apps/h264/fullhd-link.patch"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld" "${h264_linker}"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_h264_input.h")
list(REMOVE_DUPLICATES h264_inputs)
if(SUNSTONE_H264_HIGH)
 foreach(name IN ITEMS gsth264parser.c gsth264parser.h nalutils.c nalutils.h gsth264decoder.c gsth264picture.c COPYING pins.json)
  list(APPEND h264_inputs "${SUNSTONE_H264_GSTREAMER_SOURCE}/${name}")
 endforeach()
 list(APPEND h264_inputs "${SUNSTONE_SOURCE_ROOT}/apps/h264/gst-source.py" "${SUNSTONE_SOURCE_ROOT}/include/a733_h264_high.h" "${h264_gst}/gst-h264-types.h" "${h264_gst}/gst-nal.h")
endif()
set(h264_identity "high=${SUNSTONE_H264_HIGH};profile=${SUNSTONE_H264_FULLHD};uploaded-native-h264-sequence2;supervised60;48x32;YU12;exact-private-copy;NC-guards;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS h264_inputs)
 file(SHA256 "${input}" h264_hash)
 string(APPEND h264_identity "${h264_hash}")
endforeach()
string(SHA256 h264_build_id "${h264_identity}")
string(REGEX REPLACE "(..)" "0x\\1," h264_id_bytes "${h264_build_id}")
file(WRITE "${h264_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${h264_id_bytes}};\n")
file(WRITE "${h264_build}/build-id.txt" "${h264_build_id}\n")
target_sources(sunstone-a733-h264 PRIVATE "${h264_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${h264_inputs})
if(NOT SUNSTONE_H264_FULLHD)
 target_link_options(sunstone-a733-h264 PRIVATE -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152)
endif()
target_link_options(sunstone-a733-h264 PRIVATE -nostdlib -static -Wl,--gc-sections
 "-Wl,-T,${h264_linker}" "-Wl,-Map,${h264_build}/image.map")
set_target_properties(sunstone-a733-h264 PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${h264_linker}")
