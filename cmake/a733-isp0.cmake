if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one fixed recorded-raw ISP0 processing task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(isp0_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST isp0_parent_definitions)
 message(FATAL_ERROR "ISP0 cannot inherit an armed board target or generated boot")
endif()
set(isp0_build "${CMAKE_CURRENT_BINARY_DIR}/isp0-application")
file(MAKE_DIRECTORY "${isp0_build}")
get_target_property(isp0_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM isp0_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${isp0_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/isp0/boot.patch")
list(APPEND isp0_sources "${isp0_build}/boot.c")
foreach(source IN ITEMS apps/isp0/main.c apps/isp0/input.c apps/isp0/memory.c apps/isp0/native.c apps/isp0/usb.c apps/isp0/admission.c apps/isp0/snapshot.c
 drivers/camera/a733_isp0.c drivers/camera/a733_camera_dma.c
 drivers/clock/a733_media_clock.c drivers/clock/a733_media_clock_native.c
 drivers/iommu/a733_iommu.c)
 list(APPEND isp0_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-isp0 EXCLUDE_FROM_ALL ${isp0_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(isp0_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(isp0_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(isp0_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(isp0_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER isp0_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-isp0 PROPERTY COMPILE_OPTIONS "${isp0_options}")
target_include_directories(sunstone-a733-isp0 BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/isp0")
target_include_directories(sunstone-a733-isp0 PRIVATE ${isp0_includes})
target_compile_definitions(sunstone-a733-isp0 PRIVATE ${isp0_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_BOARD_SUPERVISED_SECONDS=20
 A733_SMP_CONTRACT_MAGIC=0x4153373349504331ULL A733_SMP_RESULT_MAGIC=0x4153373349505231ULL)
if(isp0_links)
 target_link_libraries(sunstone-a733-isp0 PRIVATE ${isp0_links})
endif()
file(GLOB isp0_headers "${SUNSTONE_SOURCE_ROOT}/apps/isp0/*.h")
set(isp0_inputs ${isp0_sources} ${isp0_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/apps/isp0/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/drivers/camera/isp0-sources.json"
 "${SUNSTONE_SOURCE_ROOT}/drivers/camera/isp600_default.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_isp0.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_isp0_application.h" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES isp0_inputs)
set(isp0_identity "runtime-native-isp01;supervised20;640x480-BGGR12-NV16;private-copy;NC-guards;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS isp0_inputs)
 file(SHA256 "${input}" isp0_hash)
 string(APPEND isp0_identity "${isp0_hash}")
endforeach()
string(SHA256 isp0_build_id "${isp0_identity}")
string(REGEX REPLACE "(..)" "0x\\1," isp0_id_bytes "${isp0_build_id}")
file(WRITE "${isp0_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${isp0_id_bytes}};\n")
file(WRITE "${isp0_build}/build-id.txt" "${isp0_build_id}\n")
target_sources(sunstone-a733-isp0 PRIVATE "${isp0_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${isp0_inputs})
target_link_options(sunstone-a733-isp0 PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${isp0_build}/image.map")
set_target_properties(sunstone-a733-isp0 PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
