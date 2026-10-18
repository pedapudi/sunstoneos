# Same supervised board provider composition with one fixed native PCIE task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(pcie_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST pcie_parent_definitions)
 message(FATAL_ERROR "PCIE cannot inherit an armed board target or generated boot")
endif()
set(pcie_build "${CMAKE_CURRENT_BINARY_DIR}/pcie-application")
file(MAKE_DIRECTORY "${pcie_build}")
get_target_property(pcie_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM pcie_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${pcie_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/pcie/boot.patch")
list(APPEND pcie_sources "${pcie_build}/boot.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/pci/a733_pcie.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/pci/a733_pcie_native.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/pci/a733_pcie_phy.c")
foreach(source IN ITEMS apps/pcie/main.c apps/pcie/native.c apps/pcie/run.c apps/pcie/usb0.c apps/pcie/admission.c apps/pcie/snapshot.c)
 list(APPEND pcie_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-pcie EXCLUDE_FROM_ALL ${pcie_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(pcie_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(pcie_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(pcie_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(pcie_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER pcie_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-pcie PROPERTY COMPILE_OPTIONS "${pcie_options}")
target_include_directories(sunstone-a733-pcie BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/pcie")
target_include_directories(sunstone-a733-pcie PRIVATE ${pcie_includes})
target_compile_definitions(sunstone-a733-pcie PRIVATE ${pcie_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x4153373350434331ULL A733_SMP_RESULT_MAGIC=0x4153373350435231ULL)
if(pcie_links)
 target_link_libraries(sunstone-a733-pcie PRIVATE ${pcie_links})
endif()
file(GLOB pcie_headers "${SUNSTONE_SOURCE_ROOT}/apps/pcie/*.h")
set(pcie_inputs ${pcie_sources} ${pcie_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/include/a733_pcie.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_pcie_native.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/pcie/boot.patch"
 "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES pcie_inputs)
set(pcie_identity "native-pcie1;supervised60;x1gen1-direct-config;final-capability-audit;retained;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS pcie_inputs)
 file(SHA256 "${input}" pcie_hash)
 string(APPEND pcie_identity "${pcie_hash}")
endforeach()
string(SHA256 pcie_build_id "${pcie_identity}")
string(REGEX REPLACE "(..)" "0x\\1," pcie_id_bytes "${pcie_build_id}")
file(WRITE "${pcie_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${pcie_id_bytes}};\n")
file(WRITE "${pcie_build}/build-id.txt" "${pcie_build_id}\n")
target_sources(sunstone-a733-pcie PRIVATE "${pcie_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${pcie_inputs})
target_link_options(sunstone-a733-pcie PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${pcie_build}/image.map")
set_target_properties(sunstone-a733-pcie PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
set(pcie_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@pcie_build@/manifest.json" "{\"target\":\"sunstone-a733-pcie\",\"execution_profile\":\"pcie-supervised-v1\",\"build_id\":\"@pcie_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${pcie_manifest_script}" pcie_manifest_script @ONLY)
file(WRITE "${pcie_build}/finish-manifest.cmake" "${pcie_manifest_script}")
add_custom_command(TARGET sunstone-a733-pcie POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-pcie>" -P "${pcie_build}/finish-manifest.cmake" VERBATIM)
