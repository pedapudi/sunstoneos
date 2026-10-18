set(SUNSTONE_TCP_DIR "" CACHE PATH "External authenticated FreeRTOS+TCP source")
set(SUNSTONE_RSN_DIR "" CACHE PATH "External authenticated wpa_supplicant source")
set(SUNSTONE_NETWORK_C_HEADERS "" CACHE PATH "Freestanding toolchain C headers")
if(SUNSTONE_TCP_DIR AND SUNSTONE_RSN_DIR)
  get_filename_component(network_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${network_root}/checks/radio-network.py"
    --verify-source "${SUNSTONE_TCP_DIR}" RESULT_VARIABLE network_verify)
  if(NOT network_verify EQUAL 0)
    message(FATAL_ERROR "FreeRTOS+TCP source authentication failed")
  endif()
  if(NOT COMMAND sunstone_enable_radio_security)
    include("${network_root}/cmake/radio-security.cmake")
  endif()
  get_target_property(network_base_sources kernel-check SOURCES)
  list(REMOVE_ITEM network_base_sources checks/kernel.c checks/fp-context.S)
  set(network_sources)
  foreach(source IN LISTS network_base_sources)
    if(NOT IS_ABSOLUTE "${source}")
      set(source "${network_root}/${source}")
    endif()
    list(APPEND network_sources "${source}")
  endforeach()
  file(GLOB tcp_sources "${SUNSTONE_TCP_DIR}/source/*.c")
# DNS runs in the IP task; use its actual nonblocking socket internals.
set_property(SOURCE "${SUNSTONE_TCP_DIR}/source/FreeRTOS_DNS.c" APPEND PROPERTY
  COMPILE_DEFINITIONS DNS_BindSocket=sunstone_net_dns_bind DNS_CloseSocket=sunstone_net_dns_close)
  add_executable(radio-network-model EXCLUDE_FROM_ALL ${network_sources} ${tcp_sources}
    "${FREERTOS_DIR}/event_groups.c" "${FREERTOS_DIR}/portable/MemMang/heap_4.c"
    "${SUNSTONE_TCP_DIR}/source/portable/BufferManagement/BufferAllocation_1.c"
    "${network_root}/network/allocator.c" "${network_root}/runtime/memmove.c" "${network_root}/checks/radio-network.c"
    "${network_root}/drivers/net/aic8800_network.c" "${network_root}/drivers/net/sunstone_net.c"
    "${network_root}/drivers/net/aic8800_data.c" "${network_root}/drivers/net/aic8800_wifi.c"
    "${network_root}/drivers/net/aic8800_runtime.c" "${network_root}/drivers/net/aic8800_firmware.c"
    "${network_root}/drivers/net/aic8800_sdio_transport.c" "${network_root}/drivers/net/aic8800_sdio_codec.c"
    "${network_root}/drivers/storage/sdio_card.c" "${network_root}/drivers/storage/sunxi_sdio.c")
  foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS COMPILE_DEFINITIONS)
    get_target_property(value kernel-check ${property})
    set_property(TARGET radio-network-model PROPERTY ${property} "${value}")
  endforeach()
  target_include_directories(radio-network-model BEFORE PRIVATE "${network_root}/network/include" "${SUNSTONE_NETWORK_C_HEADERS}")
  target_include_directories(radio-network-model PRIVATE "${SUNSTONE_TCP_DIR}/source/include" "${SUNSTONE_TCP_DIR}/source/portable/Compiler/GCC")
  sunstone_enable_radio_security(radio-network-model "${SUNSTONE_RSN_DIR}")
  set_source_files_properties(${tcp_sources} PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter")
  get_target_property(network_link_options radio-network-model LINK_OPTIONS)
  list(FILTER network_link_options EXCLUDE REGEX "Map,")
  set_property(TARGET radio-network-model PROPERTY LINK_OPTIONS "${network_link_options}")
  target_link_options(radio-network-model PRIVATE "-Wl,--wrap=FreeRTOS_sendto" "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/radio-network-model.map")
  set_target_properties(radio-network-model PROPERTIES SUFFIX ".elf")
endif()
