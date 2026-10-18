# Same supervised board provider composition with one fixed native JPEG task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(jpeg_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST jpeg_parent_definitions)
 message(FATAL_ERROR "JPEG cannot inherit an armed board target or generated boot")
endif()
set(SUNSTONE_JPEG_FIXTURE "" CACHE FILEPATH "Pinned155-byte48x32 baseline JPEG")
set(SUNSTONE_JPEG_REFERENCE "" CACHE FILEPATH "Independent full-range YUV420p2304-byte reference")
if(NOT SUNSTONE_JPEG_FIXTURE OR NOT SUNSTONE_JPEG_REFERENCE)
 message(FATAL_ERROR "JPEG requires exact fixed fixture and independent reference")
endif()
file(SIZE "${SUNSTONE_JPEG_FIXTURE}" jpeg_fixture_bytes)
file(SIZE "${SUNSTONE_JPEG_REFERENCE}" jpeg_reference_bytes)
file(SHA256 "${SUNSTONE_JPEG_FIXTURE}" jpeg_fixture_hash)
file(SHA256 "${SUNSTONE_JPEG_REFERENCE}" jpeg_reference_hash)
if(NOT jpeg_fixture_bytes EQUAL 155 OR NOT jpeg_reference_bytes EQUAL 2304 OR
 NOT jpeg_reference_hash STREQUAL "dea313a03db46a224a28a8a79febff397b832f3256b9c9ce236e79ee90e6ed7f" OR
 NOT jpeg_fixture_hash STREQUAL "3689fc87a6594a114026b65c2e104589ae3a872188bce0cb12ae08ab92ede398")
 message(FATAL_ERROR "JPEG fixed fixture/reference geometry mismatch")
endif()
set(jpeg_build "${CMAKE_CURRENT_BINARY_DIR}/jpeg-application")
file(MAKE_DIRECTORY "${jpeg_build}")
get_target_property(jpeg_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM jpeg_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
foreach(source IN ITEMS apps/jpeg/main.c apps/jpeg/native.c apps/jpeg/usb0.c apps/jpeg/admission.c
 drivers/video/a733_video_jpeg.c drivers/video/a733_video_jpeg_run.c drivers/video/a733_video_platform.c
 drivers/iommu/a733_iommu.c)
 list(APPEND jpeg_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
file(WRITE "${jpeg_build}/fixture.S" ".section .rodata.jpeg,\"a\"\n.global jpeg_fixture,jpeg_fixture_end,jpeg_reference,jpeg_reference_end\njpeg_fixture:\n.incbin \"${SUNSTONE_JPEG_FIXTURE}\"\njpeg_fixture_end:\njpeg_reference:\n.incbin \"${SUNSTONE_JPEG_REFERENCE}\"\njpeg_reference_end:\n")
list(APPEND jpeg_sources "${jpeg_build}/fixture.S")
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
 "${SUNSTONE_SOURCE_ROOT}/include/a733_jpeg_application.h" "${CMAKE_CURRENT_LIST_FILE}"
 "${SUNSTONE_JPEG_FIXTURE}" "${SUNSTONE_JPEG_REFERENCE}")
list(REMOVE_DUPLICATES jpeg_inputs)
set(jpeg_identity "fixed-native-jpeg1;supervised60;48x32;YU12;exact-private-copy;NC-guards;${CMAKE_C_COMPILER_VERSION}")
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
