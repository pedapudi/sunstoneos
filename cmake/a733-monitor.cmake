if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with two native monitor owner tasks.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(monitor_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST monitor_parent_definitions)
 message(FATAL_ERROR "MONITOR cannot inherit an armed board target or generated boot")
endif()
set(monitor_build "${CMAKE_CURRENT_BINARY_DIR}/monitor-application")
file(MAKE_DIRECTORY "${monitor_build}")
get_target_property(monitor_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM monitor_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${monitor_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/monitor/boot.patch")
apply_build_patch("${monitor_build}/platform.c" "${storage_build}/platform.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/monitor/platform.patch")
list(REMOVE_ITEM monitor_sources "${storage_build}/platform.c")
list(APPEND monitor_sources "${monitor_build}/platform.c" "${monitor_build}/boot.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/adc/a733_adc.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/adc/a733_adc_native.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/gpu/gicv3_spi.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/thermal/a733_thermal.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/thermal/a733_thermal_native.c")
foreach(source IN ITEMS apps/monitor/main.c apps/monitor/native.c apps/monitor/run.c apps/monitor/service.c apps/monitor/parent.c apps/monitor/usb0.c apps/monitor/admission.c apps/monitor/snapshot.c)
 list(APPEND monitor_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-monitor EXCLUDE_FROM_ALL ${monitor_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(monitor_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(monitor_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(monitor_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(monitor_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER monitor_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-monitor PROPERTY COMPILE_OPTIONS "${monitor_options}")
target_include_directories(sunstone-a733-monitor BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/monitor")
target_include_directories(sunstone-a733-monitor PRIVATE ${monitor_includes})
target_compile_definitions(sunstone-a733-monitor PRIVATE ${monitor_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x415337334d4f4331ULL A733_SMP_RESULT_MAGIC=0x415337334d4f5231ULL)
if(monitor_links)
 target_link_libraries(sunstone-a733-monitor PRIVATE ${monitor_links})
endif()
file(GLOB monitor_headers "${SUNSTONE_SOURCE_ROOT}/apps/monitor/*.h")
set(monitor_inputs ${monitor_sources} ${monitor_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/include/a733_adc.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_adc_native.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_thermal.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_thermal_native.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/monitor/boot.patch" "${SUNSTONE_SOURCE_ROOT}/apps/monitor/platform.patch"
 "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES monitor_inputs)
set(monitor_identity "native-monitor1;supervised60;gpadc03-ths5;repeated;irq70-71;configured-disable;retained-cldo3;retained;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS monitor_inputs)
 file(SHA256 "${input}" monitor_hash)
 string(APPEND monitor_identity "${monitor_hash}")
endforeach()
string(SHA256 monitor_build_id "${monitor_identity}")
string(REGEX REPLACE "(..)" "0x\\1," monitor_id_bytes "${monitor_build_id}")
file(WRITE "${monitor_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${monitor_id_bytes}};\n")
file(WRITE "${monitor_build}/build-id.txt" "${monitor_build_id}\n")
target_sources(sunstone-a733-monitor PRIVATE "${monitor_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${monitor_inputs})
target_link_options(sunstone-a733-monitor PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${monitor_build}/image.map")
set_target_properties(sunstone-a733-monitor PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
set(monitor_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@monitor_build@/manifest.json" "{\"target\":\"sunstone-a733-monitor\",\"execution_profile\":\"monitor-supervised-v1\",\"build_id\":\"@monitor_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${monitor_manifest_script}" monitor_manifest_script @ONLY)
file(WRITE "${monitor_build}/finish-manifest.cmake" "${monitor_manifest_script}")
add_custom_command(TARGET sunstone-a733-monitor POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-monitor>" -P "${monitor_build}/finish-manifest.cmake" VERBATIM)
