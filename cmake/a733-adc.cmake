# Same supervised board provider composition with one fixed native ADC task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(adc_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST adc_parent_definitions)
 message(FATAL_ERROR "ADC cannot inherit an armed board target or generated boot")
endif()
set(adc_build "${CMAKE_CURRENT_BINARY_DIR}/adc-application")
file(MAKE_DIRECTORY "${adc_build}")
get_target_property(adc_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM adc_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${adc_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/adc/boot.patch")
apply_build_patch("${adc_build}/platform.c" "${storage_build}/platform.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/adc/platform.patch")
list(REMOVE_ITEM adc_sources "${storage_build}/platform.c")
list(APPEND adc_sources "${adc_build}/platform.c" "${adc_build}/boot.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/adc/a733_adc.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/adc/a733_adc_native.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/gpu/gicv3_spi.c")
foreach(source IN ITEMS apps/adc/main.c apps/adc/native.c apps/adc/run.c apps/adc/collect.c apps/adc/usb0.c apps/adc/admission.c apps/adc/snapshot.c)
 list(APPEND adc_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-adc EXCLUDE_FROM_ALL ${adc_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(adc_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(adc_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(adc_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(adc_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER adc_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-adc PROPERTY COMPILE_OPTIONS "${adc_options}")
target_include_directories(sunstone-a733-adc BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/adc")
target_include_directories(sunstone-a733-adc PRIVATE ${adc_includes})
target_compile_definitions(sunstone-a733-adc PRIVATE ${adc_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x4153373341444331ULL A733_SMP_RESULT_MAGIC=0x4153373341445231ULL)
if(adc_links)
 target_link_libraries(sunstone-a733-adc PRIVATE ${adc_links})
endif()
file(GLOB adc_headers "${SUNSTONE_SOURCE_ROOT}/apps/adc/*.h")
set(adc_inputs ${adc_sources} ${adc_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/include/a733_adc.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_adc_native.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/adc/boot.patch" "${SUNSTONE_SOURCE_ROOT}/apps/adc/platform.patch"
 "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES adc_inputs)
set(adc_identity "native-adc1;supervised60;gpadc03;two-sets;irq70;retained-cldo3;retained;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS adc_inputs)
 file(SHA256 "${input}" adc_hash)
 string(APPEND adc_identity "${adc_hash}")
endforeach()
string(SHA256 adc_build_id "${adc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," adc_id_bytes "${adc_build_id}")
file(WRITE "${adc_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${adc_id_bytes}};\n")
file(WRITE "${adc_build}/build-id.txt" "${adc_build_id}\n")
target_sources(sunstone-a733-adc PRIVATE "${adc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${adc_inputs})
target_link_options(sunstone-a733-adc PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${adc_build}/image.map")
set_target_properties(sunstone-a733-adc PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
set(adc_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@adc_build@/manifest.json" "{\"target\":\"sunstone-a733-adc\",\"execution_profile\":\"adc-supervised-v1\",\"build_id\":\"@adc_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${adc_manifest_script}" adc_manifest_script @ONLY)
file(WRITE "${adc_build}/finish-manifest.cmake" "${adc_manifest_script}")
add_custom_command(TARGET sunstone-a733-adc POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-adc>" -P "${adc_build}/finish-manifest.cmake" VERBATIM)
