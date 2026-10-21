if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one fixed native JPEG task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(jpeg_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST jpeg_parent_definitions)
 message(FATAL_ERROR "JPEG cannot inherit an armed board target or generated boot")
endif()
set(jpeg_build "${CMAKE_CURRENT_BINARY_DIR}/jpeg-application")
file(MAKE_DIRECTORY "${jpeg_build}")
get_target_property(jpeg_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM jpeg_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${jpeg_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/jpeg/boot.patch")
list(APPEND jpeg_sources "${jpeg_build}/boot.c")
foreach(source IN ITEMS apps/jpeg/main.c apps/jpeg/input.c apps/jpeg/native.c apps/jpeg/usb0.c apps/jpeg/admission.c apps/jpeg/snapshot.c
 drivers/video/a733_video_jpeg.c drivers/video/a733_video_jpeg_run.c drivers/video/a733_video_platform.c drivers/video/a733_video_unit.c
 drivers/iommu/a733_iommu.c)
 list(APPEND jpeg_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-jpeg EXCLUDE_FROM_ALL ${jpeg_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(jpeg_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(jpeg_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(jpeg_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(jpeg_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER jpeg_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-jpeg PROPERTY COMPILE_OPTIONS "${jpeg_options}")
target_include_directories(sunstone-a733-jpeg BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/jpeg")
target_include_directories(sunstone-a733-jpeg PRIVATE ${jpeg_includes})
target_compile_definitions(sunstone-a733-jpeg PRIVATE ${jpeg_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x415337334a504332ULL A733_SMP_RESULT_MAGIC=0x415337334a505232ULL)
if(jpeg_links)
 target_link_libraries(sunstone-a733-jpeg PRIVATE ${jpeg_links})
endif()
file(GLOB jpeg_headers "${SUNSTONE_SOURCE_ROOT}/apps/jpeg/*.h")
set(jpeg_inputs ${jpeg_sources} ${jpeg_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/apps/jpeg/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/jpeg/fixtures.json" "${SUNSTONE_SOURCE_ROOT}/apps/jpeg/fixture.py"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_jpeg_application.h" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES jpeg_inputs)
set(jpeg_identity "runtime-native-jpeg2;supervised60;bounded420;optional-oracle;private-copy;NC-guards;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS jpeg_inputs)
 file(SHA256 "${input}" jpeg_hash)
 string(APPEND jpeg_identity "${jpeg_hash}")
endforeach()
string(SHA256 jpeg_build_id "${jpeg_identity}")
string(REGEX REPLACE "(..)" "0x\\1," jpeg_id_bytes "${jpeg_build_id}")
file(WRITE "${jpeg_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${jpeg_id_bytes}};\n")
file(WRITE "${jpeg_build}/build-id.txt" "${jpeg_build_id}\n")
target_sources(sunstone-a733-jpeg PRIVATE "${jpeg_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${jpeg_inputs})
target_link_options(sunstone-a733-jpeg PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${jpeg_build}/image.map")
set_target_properties(sunstone-a733-jpeg PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
