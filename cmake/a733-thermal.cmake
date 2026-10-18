# Same supervised board provider composition with one fixed native THERMAL task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(thermal_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST thermal_parent_definitions)
 message(FATAL_ERROR "THERMAL cannot inherit an armed board target or generated boot")
endif()
set(thermal_build "${CMAKE_CURRENT_BINARY_DIR}/thermal-application")
file(MAKE_DIRECTORY "${thermal_build}")
get_target_property(thermal_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM thermal_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${thermal_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/thermal/boot.patch")
list(REMOVE_ITEM thermal_sources "${storage_build}/platform.c")
apply_build_patch("${thermal_build}/platform.c" "${storage_build}/platform.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/thermal/platform.patch")
list(APPEND thermal_sources "${thermal_build}/boot.c" "${thermal_build}/platform.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/thermal/a733_thermal.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/thermal/a733_thermal_native.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/gpu/gicv3_spi.c")
foreach(source IN ITEMS apps/thermal/main.c apps/thermal/native.c apps/thermal/collect.c apps/thermal/usb0.c apps/thermal/admission.c apps/thermal/snapshot.c)
 list(APPEND thermal_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-thermal EXCLUDE_FROM_ALL ${thermal_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(thermal_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(thermal_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(thermal_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(thermal_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER thermal_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-thermal PROPERTY COMPILE_OPTIONS "${thermal_options}")
target_include_directories(sunstone-a733-thermal BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/thermal")
target_include_directories(sunstone-a733-thermal PRIVATE ${thermal_includes})
target_compile_definitions(sunstone-a733-thermal PRIVATE ${thermal_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x4153373354484331ULL A733_SMP_RESULT_MAGIC=0x4153373354485231ULL)
if(thermal_links)
 target_link_libraries(sunstone-a733-thermal PRIVATE ${thermal_links})
endif()
file(GLOB thermal_headers "${SUNSTONE_SOURCE_ROOT}/apps/thermal/*.h")
set(thermal_inputs ${thermal_sources} ${thermal_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/include/a733_thermal.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_thermal_native.h" "${SUNSTONE_SOURCE_ROOT}/include/gicv3_spi.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/thermal/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/thermal/platform.patch" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES thermal_inputs)
set(thermal_identity "native-thermal1;supervised60;five-calibrated-channels;two-fresh-sets;retained;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS thermal_inputs)
 file(SHA256 "${input}" thermal_hash)
 string(APPEND thermal_identity "${thermal_hash}")
endforeach()
string(SHA256 thermal_build_id "${thermal_identity}")
string(REGEX REPLACE "(..)" "0x\\1," thermal_id_bytes "${thermal_build_id}")
file(WRITE "${thermal_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${thermal_id_bytes}};\n")
file(WRITE "${thermal_build}/build-id.txt" "${thermal_build_id}\n")
target_sources(sunstone-a733-thermal PRIVATE "${thermal_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${thermal_inputs})
target_link_options(sunstone-a733-thermal PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${thermal_build}/image.map")
set_target_properties(sunstone-a733-thermal PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
set(thermal_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@thermal_build@/manifest.json" "{\"target\":\"sunstone-a733-thermal\",\"execution_profile\":\"thermal-supervised-v1\",\"build_id\":\"@thermal_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${thermal_manifest_script}" thermal_manifest_script @ONLY)
file(WRITE "${thermal_build}/finish-manifest.cmake" "${thermal_manifest_script}")
add_custom_command(TARGET sunstone-a733-thermal POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-thermal>" -P "${thermal_build}/finish-manifest.cmake" VERBATIM)
