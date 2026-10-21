if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one retained DI301 WEAVE, initial MOTION or fixed-strength TNR task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(deinterlace_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST deinterlace_parent_definitions)
 message(FATAL_ERROR "DEINTERLACE cannot inherit an armed board target or generated boot")
endif()
set(deinterlace_build "${CMAKE_CURRENT_BINARY_DIR}/deinterlace-application")
file(MAKE_DIRECTORY "${deinterlace_build}")
get_target_property(deinterlace_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM deinterlace_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${deinterlace_build}/boot-compact.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/boot.patch")
apply_build_patch("${deinterlace_build}/boot.c" "${deinterlace_build}/boot-compact.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/extended-boot.patch")
apply_build_patch("${deinterlace_build}/link.ld" "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/extended-link.patch")
list(APPEND deinterlace_sources "${deinterlace_build}/boot.c"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733/device_memory_layout.c")
foreach(source IN ITEMS apps/deinterlace/main.c apps/deinterlace/input.c apps/deinterlace/memory.c apps/deinterlace/native.c apps/deinterlace/usb.c apps/deinterlace/admission.c apps/deinterlace/snapshot.c
 drivers/deinterlace/a733_deinterlace.c drivers/iommu/a733_iommu.c)
 list(APPEND deinterlace_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
set(di_generated "${deinterlace_build}/di301")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/adapt.py"
 --cache "$ENV{HOME}/.cache/sunstoneos/sources/deinterlace-native" --output "${di_generated}"
 RESULT_VARIABLE di_adapt_status)
if(NOT di_adapt_status EQUAL 0)
 message(FATAL_ERROR "Pinned DI301 configuration adaptation failed")
endif()
list(APPEND deinterlace_sources "${di_generated}/di301_alg.c" "${di_generated}/di-formats.c")
# Original Linux debug-only locals are unused when diagnostics are disabled.
set_source_files_properties("${SUNSTONE_SOURCE_ROOT}/drivers/deinterlace/a733_deinterlace.c"
 "${di_generated}/di301_alg.c" "${di_generated}/di-formats.c" PROPERTIES COMPILE_OPTIONS "-w")
add_executable(sunstone-a733-deinterlace EXCLUDE_FROM_ALL ${deinterlace_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(deinterlace_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(deinterlace_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(deinterlace_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(deinterlace_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER deinterlace_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-deinterlace PROPERTY COMPILE_OPTIONS "${deinterlace_options}")
target_include_directories(sunstone-a733-deinterlace BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace")
target_include_directories(sunstone-a733-deinterlace PRIVATE ${deinterlace_includes} "${di_generated}" "${SUNSTONE_SOURCE_ROOT}/drivers/deinterlace")
target_compile_definitions(sunstone-a733-deinterlace PRIVATE ${deinterlace_definitions}
 SUNSTONE_BOARD_APPLICATION=1 SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1
 A733_BOARD_SUPERVISED_SECONDS=20 A733_SMP_CONTRACT_MAGIC=0x4153373344494331ULL A733_SMP_RESULT_MAGIC=0x4153373344495231ULL)
if(deinterlace_links)
 target_link_libraries(sunstone-a733-deinterlace PRIVATE ${deinterlace_links})
endif()
file(GLOB di_generated_inputs "${di_generated}/*")
file(GLOB deinterlace_headers "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/*.h")
set(deinterlace_inputs ${deinterlace_sources} ${deinterlace_headers} ${board_inputs} ${di_generated_inputs}
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/adapt.py"
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/extended-boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/extended-link.patch"
 "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached/link-devices.ld"
 "${deinterlace_build}/link.ld"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_deinterlace_application.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_deinterlace.h"
 "${SUNSTONE_SOURCE_ROOT}/drivers/deinterlace/di-port.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/deinterlace/sources.json" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES deinterlace_inputs)
set(deinterlace_identity "runtime-native-di301-weave-initial-motion1;supervised20;NV12-640x480;private-copy;extended-NC-guards;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS deinterlace_inputs)
 file(SHA256 "${input}" deinterlace_hash)
 string(APPEND deinterlace_identity "${deinterlace_hash}")
endforeach()
string(SHA256 deinterlace_build_id "${deinterlace_identity}")
string(REGEX REPLACE "(..)" "0x\\1," deinterlace_id_bytes "${deinterlace_build_id}")
file(WRITE "${deinterlace_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${deinterlace_id_bytes}};\n")
file(WRITE "${deinterlace_build}/build-id.txt" "${deinterlace_build_id}\n")
target_sources(sunstone-a733-deinterlace PRIVATE "${deinterlace_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${deinterlace_inputs})
target_link_options(sunstone-a733-deinterlace PRIVATE -nostdlib -static -Wl,--gc-sections
 "-Wl,-T,${deinterlace_build}/link.ld" "-Wl,-Map,${deinterlace_build}/image.map")
set_target_properties(sunstone-a733-deinterlace PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${deinterlace_build}/link.ld")

set(deinterlace_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@deinterlace_build@/manifest.json" "{\"target\":\"sunstone-a733-deinterlace\",\"execution_profile\":\"deinterlace-supervised-v1\",\"workload\":\"copied-NV12-offline-weave-motion-or-fixed-tnr\",\"build_id\":\"@deinterlace_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${deinterlace_manifest_script}" deinterlace_manifest_script @ONLY)
file(WRITE "${deinterlace_build}/finish-manifest.cmake" "${deinterlace_manifest_script}")
add_custom_command(TARGET sunstone-a733-deinterlace POST_BUILD
 COMMAND "${CMAKE_COMMAND}" "-DIMAGE=$<TARGET_FILE:sunstone-a733-deinterlace>"
 -P "${deinterlace_build}/finish-manifest.cmake" VERBATIM)
