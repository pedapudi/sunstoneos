if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Same supervised board provider composition with one fixed native UART task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(uart_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST uart_parent_definitions)
 message(FATAL_ERROR "UART cannot inherit an armed board target or generated boot")
endif()
set(uart_build "${CMAKE_CURRENT_BINARY_DIR}/uart-application")
file(MAKE_DIRECTORY "${uart_build}")
get_target_property(uart_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM uart_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${uart_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/uart/boot.patch")
list(REMOVE_ITEM uart_sources "${storage_build}/platform.c")
apply_build_patch("${uart_build}/platform.c" "${storage_build}/platform.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/uart/platform.patch")
list(APPEND uart_sources "${uart_build}/boot.c" "${uart_build}/platform.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/gpu/gicv3_spi.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/uart/a733_uart.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/uart/a733_uart_platform.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/uart/a733_uart_platform_native.c")
foreach(source IN ITEMS apps/uart/main.c apps/uart/native.c apps/uart/irq.c apps/uart/run.c apps/uart/usb0.c apps/uart/admission.c apps/uart/snapshot.c)
 list(APPEND uart_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-uart EXCLUDE_FROM_ALL ${uart_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(uart_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(uart_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(uart_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(uart_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER uart_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-uart PROPERTY COMPILE_OPTIONS "${uart_options}")
target_include_directories(sunstone-a733-uart BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/uart")
target_include_directories(sunstone-a733-uart PRIVATE ${uart_includes})
target_compile_definitions(sunstone-a733-uart PRIVATE ${uart_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x4153373355414331ULL A733_SMP_RESULT_MAGIC=0x4153373355415231ULL)
if(uart_links)
 target_link_libraries(sunstone-a733-uart PRIVATE ${uart_links})
endif()
file(GLOB uart_headers "${SUNSTONE_SOURCE_ROOT}/apps/uart/*.h")
set(uart_inputs ${uart_sources} ${uart_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/include/gicv3_spi.h"
 "${SUNSTONE_SOURCE_ROOT}/include/a733_uart.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_uart_platform.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_uart_platform_native.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/uart/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/uart/platform.patch"
 "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES uart_inputs)
set(uart_identity "native-uart2-deferred-irq;supervised60;pb0-pb1-passive-loopback;8n1-115200;IRQ36;closed;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS uart_inputs)
 file(SHA256 "${input}" uart_hash)
 string(APPEND uart_identity "${uart_hash}")
endforeach()
string(SHA256 uart_build_id "${uart_identity}")
string(REGEX REPLACE "(..)" "0x\\1," uart_id_bytes "${uart_build_id}")
file(WRITE "${uart_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${uart_id_bytes}};\n")
file(WRITE "${uart_build}/build-id.txt" "${uart_build_id}\n")
target_sources(sunstone-a733-uart PRIVATE "${uart_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${uart_inputs})
target_link_options(sunstone-a733-uart PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${uart_build}/image.map")
set_target_properties(sunstone-a733-uart PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
set(uart_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@uart_build@/manifest.json" "{\"target\":\"sunstone-a733-uart\",\"execution_profile\":\"uart-supervised-v1\",\"build_id\":\"@uart_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${uart_manifest_script}" uart_manifest_script @ONLY)
file(WRITE "${uart_build}/finish-manifest.cmake" "${uart_manifest_script}")
add_custom_command(TARGET sunstone-a733-uart POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-uart>" -P "${uart_build}/finish-manifest.cmake" VERBATIM)
