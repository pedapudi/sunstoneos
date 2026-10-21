# Fixed native board composition. Reuse the maintained cached boot and actual
# shared providers without linking storage authority or maintenance workers.
if(NOT TARGET sunstone-a733-storage-maintenance)
  include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-storage-maintenance.cmake")
endif()
set(SUNSTONE_TCP_DIR "" CACHE PATH "Pinned FreeRTOS+TCP source")
set(SUNSTONE_RSN_DIR "" CACHE PATH "Pinned wpa_supplicant source")
set(SUNSTONE_NETWORK_C_HEADERS "" CACHE PATH "Freestanding toolchain C headers")
if(NOT SUNSTONE_TCP_DIR OR NOT SUNSTONE_RSN_DIR OR NOT SUNSTONE_NETWORK_C_HEADERS)
  message(FATAL_ERROR "Native board requires pinned TCP/RSN sources and C headers")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${SUNSTONE_SOURCE_ROOT}/checks/radio-network.py"
  --verify-source "${SUNSTONE_TCP_DIR}" RESULT_VARIABLE board_tcp_verify)
if(NOT board_tcp_verify EQUAL 0)
  message(FATAL_ERROR "Native board TCP source authentication failed")
endif()
include("${SUNSTONE_SOURCE_ROOT}/cmake/radio-security.cmake")
option(SUNSTONE_BOARD_SUPERVISED "Retained RAM-only board session with manual power-removal recovery" OFF)
option(SUNSTONE_BOARD_USB2_IRQ "CPU0 interrupt completion for the supervised board USB2 owner" OFF)
set(board_root "${SUNSTONE_SOURCE_ROOT}")
set(board_build "${CMAKE_CURRENT_BINARY_DIR}/board-application")
file(MAKE_DIRECTORY "${board_build}")
get_target_property(board_sources sunstone-a733-storage-maintenance SOURCES)
foreach(source IN ITEMS
  platform/a733-storage-maintenance/main.c platform/a733-storage-maintenance/session.c
  platform/a733-storage-maintenance/admission.c platform/a733-storage-update/bootstrap.c
  platform/a733-storage-update/worker.c platform/a733-storage-update/usb.c
  drivers/storage/a733_storage_update.c drivers/storage/storage_update_usb.c
  drivers/storage/a733_mmc_platform.c drivers/storage/a733_mmc_platform_native.c
  drivers/storage/sunxi_mmc.c drivers/watchdog/a733_maintenance_watchdog.c)
  list(REMOVE_ITEM board_sources "${board_root}/${source}")
endforeach()
list(REMOVE_ITEM board_sources "${storage_build}/boot.c" "${storage_build}/build-id.c")
apply_build_patch("${board_build}/boot.c" "${board_root}/platform/a733-smp-cached/boot.c"
  "${board_root}/apps/board/boot.patch")
if(SUNSTONE_BOARD_SUPERVISED)
  apply_build_patch("${board_build}/boot-supervised.c" "${board_build}/boot.c"
    "${board_root}/platform/a733-smp-supervised/boot.patch")
  list(APPEND board_sources "${board_build}/boot-supervised.c")
  if(SUNSTONE_BOARD_USB2_IRQ)
    apply_build_patch("${board_build}/boot-host-irq.c" "${board_build}/boot-supervised.c"
      "${board_root}/apps/board/host_irq_boot.patch")
    list(REMOVE_ITEM board_sources "${board_build}/boot-supervised.c" "${storage_build}/platform.c")
    apply_build_patch("${board_build}/platform-host-irq.c" "${storage_build}/platform.c"
      "${board_root}/apps/board/host_irq_platform.patch")
    list(APPEND board_sources "${board_build}/boot-host-irq.c" "${board_build}/platform-host-irq.c")
  endif()
else()
  list(APPEND board_sources "${board_build}/boot.c")
endif()
foreach(source IN ITEMS
  apps/board/main.c apps/board/session.c apps/board/radio_profile.c apps/board/admission.c apps/board/client.c apps/board/usb0.c apps/board/identity.c
  drivers/usb/a733_typec.c drivers/usb/a733_typec_pd.c platform/a733/typec_native.c drivers/i2c/a733_twi.c drivers/i2c/a733_twi_native.c
  drivers/usb/a733_usb2_platform.c drivers/usb/a733_usb2_native.c drivers/usb/a733_xhci.c drivers/usb/a733_usb3_phy.c
  platform/a733/memory_uncached.c platform/a733/memory_reservations.c runtime/device_memory_pool.c
  apps/board/radio.c drivers/net/a733_radio_session.c drivers/net/a733_radio_platform.c
  drivers/net/a733_radio_platform_native.c drivers/net/a733_radio_entropy.c
  drivers/net/aic8800_network.c drivers/net/aic8800_sdio_transport.c drivers/net/aic8800_sdio_codec.c
  drivers/net/aic8800_runtime.c drivers/net/aic8800_data.c drivers/net/aic8800_dispatch.c
  drivers/net/aic8800_firmware.c drivers/net/aic8800_wifi.c drivers/net/sunstone_net.c
  drivers/storage/sdio_card.c drivers/storage/sunxi_sdio.c
  drivers/crypto/a733_ce.c drivers/crypto/a733_ce_native.c drivers/crypto/a733_ce_entropy.c
  drivers/crypto/a733_ce_session.c drivers/crypto/a733_ce_hash.c drivers/crypto/a733_ce_cipher.c
  network/allocator.c)
  list(APPEND board_sources "${board_root}/${source}")
endforeach()
# Each role keeps the component's own configuration and logical port0. Namespace
# the common TinyUSB exports in the host objects; device and host tasks never
# touch one another's stack state or owner-dependent time implementation.
apply_build_patch("${board_build}/hub-native.c" "${TINYUSB_DIR}/src/host/hub.c"
  "${board_root}/drivers/usb/host/hub.patch")
# fetch-sources.py stores the Cadence PHY source at its lock archive path. Older
# caches hold the same file in an extracted board-kernel tree; cadence-source.py
# checks the pinned digest of whichever copy is used.
file(READ "${board_root}/sources.lock.json" board_lock)
string(JSON board_cadence_archive GET "${board_lock}" pcie-sunxi-cadence-combophy-c archive)
set(board_cadence_source "${SUNSTONE_SOURCE_CACHE}/${board_cadence_archive}")
if(NOT EXISTS "${board_cadence_source}")
  set(board_cadence_source "${SUNSTONE_SOURCE_CACHE}/pcie/kernel/bsp/drivers/phy/sunxi-cadence-combophy.c")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${board_root}/drivers/usb/host/cadence-source.py"
  --source "${board_cadence_source}" --output "${board_build}/ss-cadence.inc" RESULT_VARIABLE ss_source_status)
if(NOT ss_source_status EQUAL 0)
  message(FATAL_ERROR "Pinned Cadence USB source preparation failed; "
    "run tools/fetch-sources.py pcie-sunxi-cadence-combophy-c")
endif()
set(board_host_tiny "${board_build}/tinyusb-ss")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${board_root}/drivers/usb/host/superspeed.py"
  --source "${TINYUSB_DIR}/src" --output "${board_host_tiny}" RESULT_VARIABLE ss_stack_status)
if(NOT ss_stack_status EQUAL 0)
  message(FATAL_ERROR "Pinned TinyUSB SuperSpeed preparation failed")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${board_root}/drivers/usb/host/superspeed.py"
  "${board_root}/drivers/usb/host/cadence-source.py" "${board_cadence_source}" "${board_root}/sources.lock.json")
set(board_host_sources "${board_root}/drivers/usb/a733_xhci_tinyusb.c"
  "${board_build}/hub-native.c"
  "${board_host_tiny}/tusb.c" "${board_host_tiny}/host/usbh.c"
  "${board_host_tiny}/class/msc/msc_host.c"
  "${board_host_tiny}/class/hid/hid_host.c")
add_library(a733-board-usb2-host OBJECT ${board_host_sources})
target_include_directories(a733-board-usb2-host BEFORE PRIVATE "${board_host_tiny}")
get_target_property(board_options sunstone-a733-storage-maintenance COMPILE_OPTIONS)
set_property(TARGET a733-board-usb2-host PROPERTY COMPILE_OPTIONS "${board_options}")
target_compile_definitions(a733-board-usb2-host PRIVATE SUNSTONE_SECURE_MONITOR=1 SUNSTONE_A733_SMP_DIAGNOSTIC=1)
target_compile_options(a733-board-usb2-host PRIVATE -include "${board_root}/apps/board/host-symbols.h")
target_include_directories(a733-board-usb2-host PRIVATE "${board_root}/drivers/usb/host"
  "${board_root}/apps/board" "${board_root}/platform/a733-smp-cached"
  "${board_root}/include" "${board_root}/runtime/include" "${FREERTOS_DIR}/include"
  "${storage_base}" "${TINYUSB_DIR}/src" "${TINYUSB_DIR}/src/host")
file(GLOB board_tcp_sources "${SUNSTONE_TCP_DIR}/source/*.c")
set(board_tcp_return "${board_build}/FreeRTOS_TCP_Transmission_IPv4.c")
apply_build_patch("${board_tcp_return}" "${SUNSTONE_TCP_DIR}/source/FreeRTOS_TCP_Transmission_IPv4.c"
  "${board_root}/patches/freertos-tcp-ipv4-return-checksum.patch")
list(REMOVE_ITEM board_tcp_sources "${SUNSTONE_TCP_DIR}/source/FreeRTOS_TCP_Transmission_IPv4.c")
list(APPEND board_tcp_sources "${board_tcp_return}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${SUNSTONE_TCP_DIR}/source/FreeRTOS_TCP_Transmission_IPv4.c"
  "${board_root}/patches/freertos-tcp-ipv4-return-checksum.patch")
# DNS runs in the IP task; use its actual nonblocking socket internals.
set_property(SOURCE "${SUNSTONE_TCP_DIR}/source/FreeRTOS_DNS.c" APPEND PROPERTY
  COMPILE_DEFINITIONS DNS_BindSocket=sunstone_net_dns_bind DNS_CloseSocket=sunstone_net_dns_close)
list(APPEND board_sources ${board_tcp_sources} "${FREERTOS_DIR}/event_groups.c"
  "${FREERTOS_DIR}/portable/MemMang/heap_4.c"
  "${SUNSTONE_TCP_DIR}/source/portable/BufferManagement/BufferAllocation_1.c")
set_source_files_properties(${board_tcp_sources} PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
add_executable(sunstone-a733-board EXCLUDE_FROM_ALL ${board_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
target_include_directories(sunstone-a733-board PRIVATE "${board_build}")
if(SUNSTONE_BOARD_USB2_IRQ)
  if(NOT SUNSTONE_BOARD_SUPERVISED)
    message(FATAL_ERROR "USB2 IRQ completion requires supervised board startup")
  endif()
  target_sources(sunstone-a733-board PRIVATE "${board_root}/apps/board/host_irq.c"
    "${board_root}/drivers/gpu/gicv3_spi.c")
  target_compile_definitions(sunstone-a733-board PRIVATE SUNSTONE_BOARD_USB2_IRQ=1)
endif()
sunstone_enable_radio_security(sunstone-a733-board "${SUNSTONE_RSN_DIR}")
get_target_property(board_all_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM board_all_sources "$<TARGET_OBJECTS:a733-board-usb2-host>")
file(GLOB_RECURSE board_external_headers "${SUNSTONE_TCP_DIR}/source/*.h" "${SUNSTONE_RSN_DIR}/src/*.h"
  "${SUNSTONE_NETWORK_C_HEADERS}/*.h")
set(board_inputs "${board_cadence_source}" "${board_root}/drivers/usb/host/cadence-source.py" "${board_root}/drivers/usb/host/superspeed.py" ${board_all_sources} ${board_external_headers} ${board_host_sources} ${storage_inputs}
  "${board_root}/apps/board/boot.patch" "${board_root}/drivers/usb/host/hub.patch"
  "${TINYUSB_DIR}/src/host/hub.c" "${CMAKE_CURRENT_LIST_FILE}")
# The board, its host objects and its network stack also include these private headers.
file(GLOB board_headers "${board_root}/apps/board/*.h" "${board_root}/network/include/*.h"
  "${board_root}/drivers/usb/*.h" "${board_root}/drivers/usb/host/*.h")
list(APPEND board_inputs ${board_headers} "${board_root}/cmake/radio-security.cmake")
list(REMOVE_DUPLICATES board_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${board_inputs})
set_property(TARGET sunstone-a733-board PROPERTY COMPILE_OPTIONS "${board_options}")
target_compile_options(sunstone-a733-board PRIVATE "$<$<COMPILE_LANGUAGE:C>:-include;stdint.h>")
target_include_directories(sunstone-a733-board BEFORE PRIVATE "${board_root}/apps/board"
  "${board_root}/platform/a733-storage-update" "${board_root}/platform/a733-smp-cached"
  "${board_root}/drivers/usb" "${TINYUSB_DIR}/src" "${board_root}/include"
  "${board_root}/runtime/include" "${FREERTOS_DIR}/include" "${storage_base}")
if(SUNSTONE_BOARD_SUPERVISED)
  target_compile_definitions(sunstone-a733-board PRIVATE SUNSTONE_BOARD_SUPERVISED=1)
  set(board_contract_magic 0x4153373342534332ULL)
  set(board_result_magic 0x4153373342535232ULL)
else()
  set(board_contract_magic 0x41533733424f4331ULL)
  set(board_result_magic 0x41533733424f5231ULL)
endif()
target_compile_definitions(sunstone-a733-board PRIVATE "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,sunstone-a733-board>:SUNSTONE_BOARD_USB2_DUAL=1;SUNSTONE_BOARD_USB3=1>" SUNSTONE_SECURE_MONITOR=1
  SUNSTONE_A733_SMP_DIAGNOSTIC=1 SUNSTONE_A733_USB=1
  A733_SMP_CONTRACT_MAGIC=${board_contract_magic} A733_SMP_RESULT_MAGIC=${board_result_magic})
target_link_options(sunstone-a733-board PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152 "-Wl,-T,${storage_build}/link.ld"
  "-Wl,-Map,${board_build}/image.map")
target_include_directories(sunstone-a733-board PRIVATE "${board_root}/network/include"
  "${SUNSTONE_NETWORK_C_HEADERS}" "${SUNSTONE_TCP_DIR}/source/include"
  "${SUNSTONE_TCP_DIR}/source/portable/Compiler/GCC")
set_target_properties(sunstone-a733-board PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
# Identify the image by its input contents and its effective options, which
# select code such as the USB2 IRQ path or extended device memory through
# CMAKE_C_FLAGS. Checkout and build paths are replaced by fixed names.
string(TOUPPER "${CMAKE_BUILD_TYPE}" board_config)
set(board_flags "${CMAKE_C_FLAGS};${CMAKE_C_FLAGS_${board_config}};${CMAKE_ASM_FLAGS};${CMAKE_ASM_FLAGS_${board_config}}")
foreach(target sunstone-a733-board a733-board-usb2-host)
  foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS)
    get_target_property(board_value ${target} ${property})
    string(APPEND board_flags ";${property}=${board_value}")
  endforeach()
endforeach()
sunstone_portable_text(board_flags "${board_flags}")
set(board_identity "native-board-1;${SUNSTONE_BOARD_SUPERVISED};USB2_IRQ=${SUNSTONE_BOARD_USB2_IRQ};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};8;USB0-device;USB2-host;NC;fixed-watchdog-window;${board_flags}")
foreach(input IN LISTS board_inputs)
  file(SHA256 "${input}" board_digest)
  string(APPEND board_identity "${board_digest}")
endforeach()
string(SHA256 board_build_id "${board_identity}")
string(REGEX REPLACE "(..)" "0x\\1," board_id_bytes "${board_build_id}")
file(WRITE "${board_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${board_id_bytes}};\n")
file(WRITE "${board_build}/build-id.txt" "${board_build_id}\n")
target_sources(sunstone-a733-board PRIVATE "${board_build}/build-id.c")
