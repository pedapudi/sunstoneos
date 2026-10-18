# Separate supervised HCI identification image; the board workload ABI stays
# unchanged. Actual common USB0/PMIC startup invokes the three application hooks.
if(NOT SUNSTONE_BOARD_SUPERVISED OR NOT TARGET sunstone-a733-board)
  message(FATAL_ERROR "Bluetooth application requires the actual supervised board parent and pinned dependencies")
endif()
  set(bluetooth_root "${SUNSTONE_SOURCE_ROOT}")
  set(bluetooth_build "${CMAKE_CURRENT_BINARY_DIR}/bluetooth-application")
  file(MAKE_DIRECTORY "${bluetooth_build}")
  file(READ "${bluetooth_root}/apps/board/usb0.c" bluetooth_usb_base)
  set(bluetooth_stop "static _Noreturn void stop(void){for(;;)vTaskDelay(1);}")
  string(FIND "${bluetooth_usb_base}" "${bluetooth_stop}" bluetooth_stop_at)
  if(bluetooth_stop_at LESS 0)
    message(FATAL_ERROR "Shared USB terminal path changed; review Bluetooth revocation adapter")
  endif()
  string(REPLACE "${bluetooth_stop}" "static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskDelay(1);}"
    bluetooth_usb_base "${bluetooth_usb_base}")
  file(WRITE "${bluetooth_build}/bluetooth-usb-base.c" "${bluetooth_usb_base}")
  get_target_property(bluetooth_sources sunstone-a733-board SOURCES)
  list(REMOVE_ITEM bluetooth_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
    "${bluetooth_root}/apps/board/usb0.c" "${bluetooth_root}/apps/board/identity.c"
    "${board_build}/build-id.c")
  foreach(source IN ITEMS apps/bluetooth/application.c apps/bluetooth/bluetooth.c apps/bluetooth/result.c
      apps/bluetooth/identity.c apps/bluetooth/usb0.c drivers/uart/a733_uart.c
      drivers/uart/a733_uart_platform.c drivers/uart/a733_uart_platform_native.c
      drivers/bluetooth/bt_h4.c drivers/bluetooth/bt_hci.c)
    list(APPEND bluetooth_sources "${bluetooth_root}/${source}")
  endforeach()
  add_executable(sunstone-a733-bluetooth EXCLUDE_FROM_ALL ${bluetooth_sources}
    $<TARGET_OBJECTS:a733-board-usb2-host>)
  foreach(property COMPILE_OPTIONS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS LINK_DEPENDS)
    get_target_property(value sunstone-a733-board ${property})
    if(NOT value STREQUAL "value-NOTFOUND")
      set_property(TARGET sunstone-a733-bluetooth PROPERTY ${property} "${value}")
    endif()
  endforeach()
  foreach(source IN LISTS bluetooth_sources)
    get_source_file_property(options "${source}" TARGET_DIRECTORY sunstone-a733-board COMPILE_OPTIONS)
    if(NOT options STREQUAL "NOTFOUND")
      set_source_files_properties("${source}" TARGET_DIRECTORY sunstone-a733-bluetooth PROPERTIES COMPILE_OPTIONS "${options}")
    endif()
  endforeach()
  target_compile_definitions(sunstone-a733-bluetooth PRIVATE SUNSTONE_BOARD_APPLICATION=1)
  target_include_directories(sunstone-a733-bluetooth BEFORE PRIVATE "${bluetooth_root}/apps/bluetooth" "${bluetooth_build}")
  get_target_property(bluetooth_link sunstone-a733-bluetooth LINK_OPTIONS)
  list(FILTER bluetooth_link EXCLUDE REGEX "Map,")
  set_property(TARGET sunstone-a733-bluetooth PROPERTY LINK_OPTIONS "${bluetooth_link}")
  target_link_options(sunstone-a733-bluetooth PRIVATE "-Wl,-Map,${bluetooth_build}/image.map")
  set(bluetooth_inputs ${board_inputs} ${bluetooth_sources} "${CMAKE_CURRENT_LIST_FILE}" "${bluetooth_build}/bluetooth-usb-base.c")
  file(GLOB bluetooth_headers "${bluetooth_root}/apps/bluetooth/*.h")
  list(APPEND bluetooth_inputs ${bluetooth_headers} "${bluetooth_root}/apps/bluetooth/manifest.py")
  list(REMOVE_DUPLICATES bluetooth_inputs)
  set(bluetooth_identity "native-bluetooth-identification-1;manual-power-removal;${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION}")
  foreach(input IN LISTS bluetooth_inputs)
    file(SHA256 "${input}" identity)
    string(APPEND bluetooth_identity "${identity}")
  endforeach()
  string(SHA256 bluetooth_build_id "${bluetooth_identity}")
  string(REGEX REPLACE "(..)" "0x\\1," bluetooth_bytes "${bluetooth_build_id}")
  file(WRITE "${bluetooth_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${bluetooth_bytes}};\n")
  file(WRITE "${bluetooth_build}/build-id.txt" "${bluetooth_build_id}\n")
  target_sources(sunstone-a733-bluetooth PRIVATE "${bluetooth_build}/build-id.c")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${bluetooth_inputs})
  set_target_properties(sunstone-a733-bluetooth PROPERTIES SUFFIX ".elf")
  add_custom_command(TARGET sunstone-a733-bluetooth POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${bluetooth_root}/apps/bluetooth/manifest.py"
      --image "$<TARGET_FILE:sunstone-a733-bluetooth>" --build-id "${bluetooth_build}/build-id.txt"
      --output "${bluetooth_build}/image-manifest.json"
    VERBATIM)
