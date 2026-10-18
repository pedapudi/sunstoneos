# Same supervised board provider composition with one fixed native CRYPTO task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(crypto_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST crypto_parent_definitions)
 message(FATAL_ERROR "CRYPTO cannot inherit an armed board target or generated boot")
endif()
set(crypto_build "${CMAKE_CURRENT_BINARY_DIR}/crypto-application")
file(MAKE_DIRECTORY "${crypto_build}")
get_target_property(crypto_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM crypto_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${crypto_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/crypto/boot.patch")
list(APPEND crypto_sources "${crypto_build}/boot.c")
foreach(source IN ITEMS apps/crypto/main.c apps/crypto/native.c apps/crypto/run.c apps/crypto/vectors.c apps/crypto/usb0.c apps/crypto/admission.c apps/crypto/snapshot.c)
 list(APPEND crypto_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-crypto EXCLUDE_FROM_ALL ${crypto_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(crypto_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(crypto_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(crypto_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(crypto_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER crypto_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-crypto PROPERTY COMPILE_OPTIONS "${crypto_options}")
target_include_directories(sunstone-a733-crypto BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/crypto")
target_include_directories(sunstone-a733-crypto PRIVATE ${crypto_includes})
target_compile_definitions(sunstone-a733-crypto PRIVATE ${crypto_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x4153373343524331ULL A733_SMP_RESULT_MAGIC=0x4153373343525231ULL)
if(crypto_links)
 target_link_libraries(sunstone-a733-crypto PRIVATE ${crypto_links})
endif()
file(GLOB crypto_headers "${SUNSTONE_SOURCE_ROOT}/apps/crypto/*.h")
set(crypto_inputs ${crypto_sources} ${crypto_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/apps/crypto/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/crypto/inputs.inc" "${SUNSTONE_SOURCE_ROOT}/apps/crypto/reference.inc" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES crypto_inputs)
set(crypto_identity "fixed-native-crypto1;supervised60;SHA256-AES128-ECB;exact-private-copy;retained;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS crypto_inputs)
 file(SHA256 "${input}" crypto_hash)
 string(APPEND crypto_identity "${crypto_hash}")
endforeach()
string(SHA256 crypto_build_id "${crypto_identity}")
string(REGEX REPLACE "(..)" "0x\\1," crypto_id_bytes "${crypto_build_id}")
file(WRITE "${crypto_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${crypto_id_bytes}};\n")
file(WRITE "${crypto_build}/build-id.txt" "${crypto_build_id}\n")
target_sources(sunstone-a733-crypto PRIVATE "${crypto_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${crypto_inputs})
target_link_options(sunstone-a733-crypto PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${crypto_build}/image.map")
set_target_properties(sunstone-a733-crypto PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
