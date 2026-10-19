# Same supervised board provider composition with one bounded native H.264 task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(h264_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST h264_parent_definitions)
 message(FATAL_ERROR "H.264 cannot inherit an armed board target or generated boot")
endif()
set(SUNSTONE_H264_FIXTURE "" CACHE FILEPATH "Bounded AnnexB48x32 Baseline IDR stream (1..4096 bytes)")
set(SUNSTONE_H264_REFERENCE "" CACHE FILEPATH "Independent full-range YUV420p2304-byte reference")
if(NOT SUNSTONE_H264_FIXTURE OR NOT SUNSTONE_H264_REFERENCE)
 message(FATAL_ERROR "H.264 requires bounded stream and independent reference")
endif()
file(SIZE "${SUNSTONE_H264_FIXTURE}" h264_fixture_bytes)
file(SIZE "${SUNSTONE_H264_REFERENCE}" h264_reference_bytes)
file(SHA256 "${SUNSTONE_H264_FIXTURE}" h264_fixture_hash)
file(SHA256 "${SUNSTONE_H264_REFERENCE}" h264_reference_hash)
if(h264_fixture_bytes LESS 1 OR h264_fixture_bytes GREATER 4096 OR NOT h264_reference_bytes EQUAL 2304)
 message(FATAL_ERROR "H.264 stream/reference exceeds bounded48x32 application geometry")
endif()
set(h264_build "${CMAKE_CURRENT_BINARY_DIR}/h264-application")
file(MAKE_DIRECTORY "${h264_build}")
get_target_property(h264_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM h264_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${h264_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264/boot.patch")
list(APPEND h264_sources "${h264_build}/boot.c")
foreach(source IN ITEMS apps/h264/main.c apps/h264/native.c apps/h264/usb0.c apps/h264/admission.c apps/h264/snapshot.c
 drivers/video/a733_video_h264.c drivers/video/a733_video_platform.c
 drivers/iommu/a733_iommu.c)
 list(APPEND h264_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
file(WRITE "${h264_build}/fixture.S" ".section .rodata.h264,\"a\"\n.global h264_fixture,h264_fixture_end,h264_reference,h264_reference_end\nh264_fixture:\n.incbin \"${SUNSTONE_H264_FIXTURE}\"\nh264_fixture_end:\nh264_reference:\n.incbin \"${SUNSTONE_H264_REFERENCE}\"\nh264_reference_end:\n")
list(APPEND h264_sources "${h264_build}/fixture.S")
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
if(h264_links)
 target_link_libraries(sunstone-a733-h264 PRIVATE ${h264_links})
endif()
file(GLOB h264_headers "${SUNSTONE_SOURCE_ROOT}/apps/h264/*.h")
set(h264_inputs ${h264_sources} ${h264_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/include/a733_h264_application.h" "${CMAKE_CURRENT_LIST_FILE}"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_video_h264.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_video_platform.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264/sources.json"
 "${SUNSTONE_SOURCE_ROOT}/apps/h264/boot.patch"
 "${SUNSTONE_H264_FIXTURE}" "${SUNSTONE_H264_REFERENCE}")
list(REMOVE_DUPLICATES h264_inputs)
set(h264_identity "bounded-native-h2641;supervised60;48x32;YU12;exact-private-copy;NC-guards;${CMAKE_C_COMPILER_VERSION}")
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
target_link_options(sunstone-a733-h264 PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${h264_build}/image.map")
set_target_properties(sunstone-a733-h264 PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
