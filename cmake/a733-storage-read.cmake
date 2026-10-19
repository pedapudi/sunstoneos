if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one read-only native SD0 task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(storage_read_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST storage_read_parent_definitions)
 message(FATAL_ERROR "STORAGE_READ cannot inherit an armed board target or generated boot")
endif()
set(storage_read_build "${CMAKE_CURRENT_BINARY_DIR}/storage-read-application")
file(MAKE_DIRECTORY "${storage_read_build}")
get_target_property(storage_read_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM storage_read_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${storage_read_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/storage-read/boot.patch")
list(APPEND storage_read_sources "${storage_read_build}/boot.c")
foreach(source IN ITEMS apps/storage-read/modes.c apps/storage-read/main.c apps/storage-read/native.c apps/storage-read/client.c apps/storage-read/usb0.c apps/storage-read/admission.c apps/storage-read/snapshot.c
 drivers/storage/a733_mmc_platform.c drivers/storage/a733_mmc_platform_native.c
 drivers/storage/sunxi_mmc.c drivers/storage/a733_storage_update.c)
 list(APPEND storage_read_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-storage-read EXCLUDE_FROM_ALL ${storage_read_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(storage_read_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(storage_read_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(storage_read_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(storage_read_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER storage_read_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-storage-read PROPERTY COMPILE_OPTIONS "${storage_read_options}")
target_include_directories(sunstone-a733-storage-read BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/storage-read")
target_include_directories(sunstone-a733-storage-read PRIVATE ${storage_read_includes})
target_compile_definitions(sunstone-a733-storage-read PRIVATE ${storage_read_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x4153373352444331ULL A733_SMP_RESULT_MAGIC=0x4153373352445231ULL)
if(storage_read_links)
 target_link_libraries(sunstone-a733-storage-read PRIVATE ${storage_read_links})
endif()
file(GLOB storage_read_headers "${SUNSTONE_SOURCE_ROOT}/apps/storage-read/*.h")
set(storage_read_inputs ${storage_read_sources} ${storage_read_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/apps/storage-read/boot.patch"
 "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES storage_read_inputs)
set(storage_read_identity "native-sd0-read-service2;supervised60;two-task-clients;bounded-ranges8;PIO-private-copy;retained;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS storage_read_inputs)
 file(SHA256 "${input}" storage_read_hash)
 string(APPEND storage_read_identity "${storage_read_hash}")
endforeach()
string(SHA256 storage_read_build_id "${storage_read_identity}")
string(REGEX REPLACE "(..)" "0x\\1," storage_read_id_bytes "${storage_read_build_id}")
file(WRITE "${storage_read_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${storage_read_id_bytes}};\n")
file(WRITE "${storage_read_build}/build-id.txt" "${storage_read_build_id}\n")
target_sources(sunstone-a733-storage-read PRIVATE "${storage_read_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${storage_read_inputs})
target_link_options(sunstone-a733-storage-read PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${storage_read_build}/image.map")
set_target_properties(sunstone-a733-storage-read PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
