# One concrete trusted pci-epf-test DMA command, retaining the config-only target.
if(NOT SUNSTONE_BOARD_SUPERVISED OR SUNSTONE_BOARD_USB2_IRQ)
 message(FATAL_ERROR "PCIe test requires the supervised polling board parent")
endif()
if(NOT TARGET sunstone-a733-pcie)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-pcie.cmake")
endif()
set(pt_root "${SUNSTONE_SOURCE_ROOT}")
set(pt_build "${CMAKE_CURRENT_BINARY_DIR}/pcie-test-application")
file(MAKE_DIRECTORY "${pt_build}")
get_target_property(pt_sources sunstone-a733-pcie SOURCES)
list(REMOVE_ITEM pt_sources "$<TARGET_OBJECTS:a733-board-usb2-host>" "${pcie_build}/boot.c"
 "${pcie_build}/build-id.c" "${storage_build}/platform.c")
foreach(source IN ITEMS main.c native.c run.c usb0.c admission.c snapshot.c)
 list(REMOVE_ITEM pt_sources "${pt_root}/apps/pcie/${source}")
 list(APPEND pt_sources "${pt_root}/apps/pcie-test/${source}")
endforeach()
apply_build_patch("${pt_build}/boot.c" "${board_build}/boot-supervised.c" "${pt_root}/apps/pcie-test/boot.patch")
apply_build_patch("${pt_build}/platform.c" "${storage_build}/platform.c" "${pt_root}/apps/pcie-test/platform.patch")
list(APPEND pt_sources "${pt_build}/boot.c" "${pt_build}/platform.c"
 "${pt_root}/apps/pcie-test/irq.c" "${pt_root}/drivers/gpu/gicv3_spi.c"
 "${pt_root}/drivers/pci/a733_pcie_epf_test.c")
add_executable(sunstone-a733-pcie-test EXCLUDE_FROM_ALL ${pt_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
foreach(property COMPILE_OPTIONS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS LINK_LIBRARIES LINK_DEPENDS)
 get_target_property(value sunstone-a733-pcie ${property})
 if(NOT value STREQUAL "value-NOTFOUND")
  if(property STREQUAL "COMPILE_DEFINITIONS")
   list(FILTER value EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
  elseif(property STREQUAL "LINK_OPTIONS")
   list(FILTER value EXCLUDE REGEX "Map,")
  endif()
  set_property(TARGET sunstone-a733-pcie-test PROPERTY ${property} "${value}")
 endif()
endforeach()
target_include_directories(sunstone-a733-pcie-test BEFORE PRIVATE "${pt_root}/apps/pcie-test")
target_compile_definitions(sunstone-a733-pcie-test PRIVATE SUNSTONE_PCIE_EPF_TEST=1
 A733_SMP_CONTRACT_MAGIC=0x4153373350544331ULL A733_SMP_RESULT_MAGIC=0x4153373350545231ULL)
target_link_options(sunstone-a733-pcie-test PRIVATE "-Wl,-Map,${pt_build}/image.map")
file(GLOB pt_headers "${pt_root}/apps/pcie-test/*.h")
set(pt_inputs ${pcie_inputs} ${pt_sources} ${pt_headers}
 "${pt_root}/include/a733_pcie_epf_test.h" "${pt_root}/apps/pcie-test/source.json" "${pt_root}/apps/pcie-test/boot.patch"
 "${pt_root}/apps/pcie-test/platform.patch" "${pt_root}/sources.lock.json" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES pt_inputs)
set_target_properties(sunstone-a733-pcie-test PROPERTIES SUFFIX ".elf")
sunstone_build_identity(sunstone-a733-pcie-test BUILD_DIR "${pt_build}" INPUTS ${pt_inputs}
 TAG "native-pcie-epf-test1;one-copy4096;msi-one-shot;guards;retained"
 TARGETS a733-board-usb2-host PROFILE pcie-test-supervised-v1 RESULT pt_build_id)
