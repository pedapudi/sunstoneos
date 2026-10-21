if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one fixed native JPEG_ENCODE task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(jpeg_encode_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST jpeg_encode_parent_definitions)
 message(FATAL_ERROR "JPEG_ENCODE cannot inherit an armed board target or generated boot")
endif()
set(jpeg_encode_build "${CMAKE_CURRENT_BINARY_DIR}/jpeg_encode-application")
file(MAKE_DIRECTORY "${jpeg_encode_build}")
get_target_property(jpeg_encode_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM jpeg_encode_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${jpeg_encode_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/jpeg-encode/boot.patch")
list(APPEND jpeg_encode_sources "${jpeg_encode_build}/boot.c")
foreach(source IN ITEMS apps/jpeg-encode/main.c apps/jpeg-encode/input.c apps/jpeg-encode/native.c apps/jpeg-encode/usb0.c apps/jpeg-encode/admission.c apps/jpeg-encode/snapshot.c
 drivers/video/a733_video_jpeg_encode.c drivers/video/a733_video_jpeg_encode_tables.c drivers/video/a733_video_encode_platform.c drivers/video/a733_video_unit.c
 drivers/iommu/a733_iommu.c)
 list(APPEND jpeg_encode_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-jpeg-encode EXCLUDE_FROM_ALL ${jpeg_encode_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(jpeg_encode_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(jpeg_encode_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(jpeg_encode_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(jpeg_encode_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER jpeg_encode_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-jpeg-encode PROPERTY COMPILE_OPTIONS "${jpeg_encode_options}")
target_include_directories(sunstone-a733-jpeg-encode BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/jpeg-encode")
target_include_directories(sunstone-a733-jpeg-encode PRIVATE ${jpeg_encode_includes})
target_compile_definitions(sunstone-a733-jpeg-encode PRIVATE ${jpeg_encode_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x415337334a454331ULL A733_SMP_RESULT_MAGIC=0x415337334a455231ULL)
if(jpeg_encode_links)
 target_link_libraries(sunstone-a733-jpeg-encode PRIVATE ${jpeg_encode_links})
endif()
file(GLOB jpeg_encode_headers "${SUNSTONE_SOURCE_ROOT}/apps/jpeg-encode/*.h")
set(jpeg_encode_inputs ${jpeg_encode_sources} ${jpeg_encode_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/apps/jpeg-encode/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/jpeg-encode/sources.json" "${SUNSTONE_SOURCE_ROOT}/apps/jpeg-encode/producer.py"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_jpeg_encode_application.h" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES jpeg_encode_inputs)
set(jpeg_encode_identity "runtime-native-jpeg-encode1;supervised60;VGA-NV12-quality75;private-copy;NC-guards;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS jpeg_encode_inputs)
 file(SHA256 "${input}" jpeg_encode_hash)
 string(APPEND jpeg_encode_identity "${jpeg_encode_hash}")
endforeach()
string(SHA256 jpeg_encode_build_id "${jpeg_encode_identity}")
string(REGEX REPLACE "(..)" "0x\\1," jpeg_encode_id_bytes "${jpeg_encode_build_id}")
file(WRITE "${jpeg_encode_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${jpeg_encode_id_bytes}};\n")
file(WRITE "${jpeg_encode_build}/build-id.txt" "${jpeg_encode_build_id}\n")
target_sources(sunstone-a733-jpeg-encode PRIVATE "${jpeg_encode_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${jpeg_encode_inputs})
target_link_options(sunstone-a733-jpeg-encode PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${jpeg_encode_build}/image.map")
set_target_properties(sunstone-a733-jpeg-encode PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
