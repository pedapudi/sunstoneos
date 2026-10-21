# The native HEVC parser, controller, decoder binding, application tasks and USB
# service are not integrated (docs/hevc-native.md). Refuse before any other
# check so that this target cannot be mistaken for a buildable one.
set(hevc_missing "")
foreach(source IN ITEMS apps/hevc/main.c apps/hevc/input.c apps/hevc/storage.c apps/hevc/native.c
 apps/hevc/parse.c apps/hevc/usb0.c apps/hevc/gst-source.py drivers/video/a733_video_hevc.c
 include/a733_video_hevc.h)
 if(NOT EXISTS "${SUNSTONE_SOURCE_ROOT}/${source}")
  list(APPEND hevc_missing "${source}")
 endif()
endforeach()
if(hevc_missing)
 list(JOIN hevc_missing ", " hevc_missing)
 message(FATAL_ERROR "Native HEVC decoding is not integrated; missing sources: ${hevc_missing}. See docs/hevc-native.md")
endif()
if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one retained two-picture HEVC decoder task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(hevc_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST hevc_parent_definitions)
 message(FATAL_ERROR "HEVC cannot inherit an armed board target or generated boot")
endif()
set(hevc_build "${CMAKE_CURRENT_BINARY_DIR}/hevc-application")
file(MAKE_DIRECTORY "${hevc_build}")
get_target_property(hevc_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM hevc_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${hevc_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/boot.patch")
apply_build_patch("${hevc_build}/boot.c" "${hevc_build}/boot-compact.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/extended-boot.patch")
apply_build_patch("${hevc_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/extended-link.patch")
list(APPEND hevc_sources "${hevc_build}/boot.c"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733/device_memory_layout.c")
foreach(source IN ITEMS apps/hevc/main.c apps/hevc/input.c apps/hevc/storage.c apps/hevc/native.c apps/hevc/parse.c apps/hevc/usb0.c apps/hevc/admission.c apps/hevc/snapshot.c
 drivers/video/a733_video_hevc.c drivers/video/a733_video_platform.c drivers/video/a733_video_unit.c
 drivers/iommu/a733_iommu.c)
 list(APPEND hevc_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
set(SUNSTONE_HEVC_SOURCE "" CACHE PATH "Pinned HEVC parser and Cedrus source cache")
set(SUNSTONE_HEVC_READER_SOURCE "" CACHE PATH "Pinned GStreamer NAL reader source cache")
if(NOT EXISTS "${SUNSTONE_HEVC_SOURCE}/gsth265parser.c" OR
   NOT EXISTS "${SUNSTONE_HEVC_READER_SOURCE}/pins.json")
 message(FATAL_ERROR "HEVC requires its pinned parser, Cedrus and NAL reader sources")
endif()
set(hevc_gst "${hevc_build}/gst")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${SUNSTONE_SOURCE_ROOT}/apps/hevc/gst-source.py"
 --source "${SUNSTONE_HEVC_SOURCE}" --reader "${SUNSTONE_HEVC_READER_SOURCE}" --output "${hevc_gst}"
 COMMAND_ERROR_IS_FATAL ANY)
list(APPEND hevc_sources "${hevc_gst}/gst-hevc-parser.c" "${hevc_gst}/gst-nal.c")
add_executable(sunstone-a733-hevc EXCLUDE_FROM_ALL ${hevc_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(hevc_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(hevc_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(hevc_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(hevc_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER hevc_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-hevc PROPERTY COMPILE_OPTIONS "${hevc_options}")
target_include_directories(sunstone-a733-hevc BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/hevc" "${hevc_gst}")
target_include_directories(sunstone-a733-hevc PRIVATE ${hevc_includes})
target_compile_definitions(sunstone-a733-hevc PRIVATE ${hevc_definitions}
 SUNSTONE_BOARD_APPLICATION=1 SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1 A733_SMP_CONTRACT_MAGIC=0x4153373348354331ULL A733_SMP_RESULT_MAGIC=0x4153373348355231ULL)
if(hevc_links)
 target_link_libraries(sunstone-a733-hevc PRIVATE ${hevc_links})
endif()
file(GLOB hevc_headers "${SUNSTONE_SOURCE_ROOT}/apps/hevc/*.h")
set(hevc_inputs ${hevc_sources} ${hevc_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/extended-boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/extended-link.patch"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${hevc_build}/link.ld"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_hevc_application.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_video_hevc.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_video_platform.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_video_unit.h"
 "${CMAKE_CURRENT_LIST_FILE}")
list(APPEND hevc_inputs
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/gst-source.py"
 "${SUNSTONE_SOURCE_ROOT}/apps/hevc/sources.json"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264/gst-port.h"
 "${hevc_gst}/gst-hevc-types.h" "${hevc_gst}/gst-nal.h"
 "${hevc_gst}/cedrus-hevc-regs.h" "${hevc_gst}/gst-inputs.json")
foreach(name IN ITEMS gsth265parser.c gsth265parser.h cedrus_regs.h)
 list(APPEND hevc_inputs "${SUNSTONE_HEVC_SOURCE}/${name}")
endforeach()
foreach(name IN ITEMS nalutils.c nalutils.h pins.json)
 list(APPEND hevc_inputs "${SUNSTONE_HEVC_READER_SOURCE}/${name}")
endforeach()
list(REMOVE_DUPLICATES hevc_inputs)
set(hevc_identity "runtime-native-hevc1;supervised60;Main8-VGA-CTB64-IDR-P-NV12;private-copy;extended-NC-guards;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS hevc_inputs)
 file(SHA256 "${input}" hevc_hash)
 string(APPEND hevc_identity "${hevc_hash}")
endforeach()
string(SHA256 hevc_build_id "${hevc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," hevc_id_bytes "${hevc_build_id}")
file(WRITE "${hevc_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${hevc_id_bytes}};\n")
file(WRITE "${hevc_build}/build-id.txt" "${hevc_build_id}\n")
target_sources(sunstone-a733-hevc PRIVATE "${hevc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${hevc_inputs})
target_link_options(sunstone-a733-hevc PRIVATE -nostdlib -static -Wl,--gc-sections
 "-Wl,-T,${hevc_build}/link.ld" "-Wl,-Map,${hevc_build}/image.map")
set_target_properties(sunstone-a733-hevc PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${hevc_build}/link.ld")

set(hevc_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@hevc_build@/manifest.json" "{\"target\":\"sunstone-a733-hevc\",\"execution_profile\":\"hevc-supervised-v1\",\"build_id\":\"@hevc_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${hevc_manifest_script}" hevc_manifest_script @ONLY)
file(WRITE "${hevc_build}/finish-manifest.cmake" "${hevc_manifest_script}")
add_custom_command(TARGET sunstone-a733-hevc POST_BUILD
 COMMAND "${CMAKE_COMMAND}" "-DIMAGE=$<TARGET_FILE:sunstone-a733-hevc>"
 -P "${hevc_build}/finish-manifest.cmake" VERBATIM)
