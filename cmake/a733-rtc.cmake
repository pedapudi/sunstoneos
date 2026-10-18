# Supervised fixed RTC adoption, initialization and owner-task operations.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(rtc_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST rtc_definitions)
 message(FATAL_ERROR "RTC requires the supervised board composition")
endif()
set(rtc_build "${CMAKE_CURRENT_BINARY_DIR}/rtc-application")
file(MAKE_DIRECTORY "${rtc_build}")
get_target_property(rtc_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM rtc_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c" "${storage_build}/platform.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
apply_build_patch("${rtc_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/rtc/boot.patch")
apply_build_patch("${rtc_build}/platform.c" "${storage_build}/platform.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/rtc/platform.patch")
list(APPEND rtc_sources "${rtc_build}/boot.c" "${rtc_build}/platform.c")
foreach(source IN ITEMS apps/rtc/main.c apps/rtc/initialize.c apps/rtc/runtime.c apps/rtc/service.c apps/rtc/service-port.c apps/rtc/native.c apps/rtc/irq.c apps/rtc/usb0.c apps/rtc/admission.c apps/rtc/snapshot.c
 drivers/rtc/a733_rtc.c drivers/rtc/a733_rtc_initialize.c drivers/rtc/a733_rtc_boot.c drivers/gpu/gicv3_spi.c)
 list(APPEND rtc_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
list(REMOVE_DUPLICATES rtc_sources)
add_executable(sunstone-a733-rtc EXCLUDE_FROM_ALL ${rtc_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
get_target_property(rtc_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(rtc_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(rtc_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER rtc_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-rtc PROPERTY COMPILE_OPTIONS "${rtc_options}")
target_include_directories(sunstone-a733-rtc BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/rtc")
target_include_directories(sunstone-a733-rtc PRIVATE ${rtc_includes})
target_compile_definitions(sunstone-a733-rtc PRIVATE ${rtc_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_BOARD_STARTUP_SECONDS=6 A733_SMP_CONTRACT_MAGIC=0x4153373352544331ULL A733_SMP_RESULT_MAGIC=0x4153373352545231ULL)
if(rtc_links)
 target_link_libraries(sunstone-a733-rtc PRIVATE ${rtc_links})
endif()
file(GLOB rtc_headers "${SUNSTONE_SOURCE_ROOT}/apps/rtc/*.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_rtc*.h")
set(rtc_inputs ${rtc_sources} ${rtc_headers} ${board_inputs} "${CMAKE_CURRENT_LIST_FILE}"
 "${SUNSTONE_SOURCE_ROOT}/apps/rtc/sources.json"
 "${SUNSTONE_SOURCE_ROOT}/apps/rtc/boot.patch" "${SUNSTONE_SOURCE_ROOT}/apps/rtc/platform.patch")
list(REMOVE_DUPLICATES rtc_inputs)
set(rtc_identity "native-rtc4;owner-service;startup6;supervised60;TypeA;external32768;alarm1..10;copied88;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS rtc_inputs)
 file(SHA256 "${input}" rtc_hash)
 string(APPEND rtc_identity "${rtc_hash}")
endforeach()
string(SHA256 rtc_build_id "${rtc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," rtc_id_bytes "${rtc_build_id}")
file(WRITE "${rtc_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${rtc_id_bytes}};\n")
file(WRITE "${rtc_build}/build-id.txt" "${rtc_build_id}\n")
target_sources(sunstone-a733-rtc PRIVATE "${rtc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${rtc_inputs})
target_link_options(sunstone-a733-rtc PRIVATE -nostdlib -static -Wl,--gc-sections
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${rtc_build}/image.map")
set_target_properties(sunstone-a733-rtc PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
