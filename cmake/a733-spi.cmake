# Same supervised board provider composition with one fixed native SPI task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(spi_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST spi_parent_definitions)
 message(FATAL_ERROR "SPI cannot inherit an armed board target or generated boot")
endif()
set(spi_build "${CMAKE_CURRENT_BINARY_DIR}/spi-application")
file(MAKE_DIRECTORY "${spi_build}")
get_target_property(spi_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM spi_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${spi_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/spi/boot.patch")
list(APPEND spi_sources "${spi_build}/boot.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/spi/a733_spi.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/spi/a733_spi_platform.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/spi/a733_spi_platform_native.c")
foreach(source IN ITEMS apps/spi/main.c apps/spi/native.c apps/spi/run.c apps/spi/usb0.c apps/spi/admission.c apps/spi/snapshot.c)
 list(APPEND spi_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-spi EXCLUDE_FROM_ALL ${spi_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(spi_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(spi_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(spi_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(spi_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER spi_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-spi PROPERTY COMPILE_OPTIONS "${spi_options}")
target_include_directories(sunstone-a733-spi BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/spi")
target_include_directories(sunstone-a733-spi PRIVATE ${spi_includes})
target_compile_definitions(sunstone-a733-spi PRIVATE ${spi_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x4153373353504331ULL A733_SMP_RESULT_MAGIC=0x4153373353505231ULL)
if(spi_links)
 target_link_libraries(sunstone-a733-spi PRIVATE ${spi_links})
endif()
file(GLOB spi_headers "${SUNSTONE_SOURCE_ROOT}/apps/spi/*.h")
set(spi_inputs ${spi_sources} ${spi_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/include/a733_spi.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_spi_platform.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_spi_platform_native.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/spi/boot.patch"
 "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES spi_inputs)
set(spi_identity "native-spi1;supervised60;spi3-pe-cs0;write-read32;retained-bldo1;retained;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS spi_inputs)
 file(SHA256 "${input}" spi_hash)
 string(APPEND spi_identity "${spi_hash}")
endforeach()
string(SHA256 spi_build_id "${spi_identity}")
string(REGEX REPLACE "(..)" "0x\\1," spi_id_bytes "${spi_build_id}")
file(WRITE "${spi_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${spi_id_bytes}};\n")
file(WRITE "${spi_build}/build-id.txt" "${spi_build_id}\n")
target_sources(sunstone-a733-spi PRIVATE "${spi_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${spi_inputs})
target_link_options(sunstone-a733-spi PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${spi_build}/image.map")
set_target_properties(sunstone-a733-spi PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
set(spi_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@spi_build@/manifest.json" "{\"target\":\"sunstone-a733-spi\",\"execution_profile\":\"spi-supervised-v1\",\"build_id\":\"@spi_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${spi_manifest_script}" spi_manifest_script @ONLY)
file(WRITE "${spi_build}/finish-manifest.cmake" "${spi_manifest_script}")
add_custom_command(TARGET sunstone-a733-spi POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-spi>" -P "${spi_build}/finish-manifest.cmake" VERBATIM)
