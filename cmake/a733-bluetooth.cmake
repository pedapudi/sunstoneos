if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
endif()
# Separate supervised HCI identification image; the board workload ABI stays
# unchanged. Actual common USB0/PMIC startup invokes the three application hooks.
if(NOT SUNSTONE_BOARD_SUPERVISED OR NOT TARGET sunstone-a733-board)
  message(FATAL_ERROR "Bluetooth application requires the actual supervised board parent and pinned dependencies")
endif()
  set(bluetooth_root "${SUNSTONE_SOURCE_ROOT}")
  set(bluetooth_build "${CMAKE_CURRENT_BINARY_DIR}/bluetooth-application")
  file(MAKE_DIRECTORY "${bluetooth_build}")
  set(SUNSTONE_BT_SECURITY_DIR "" CACHE PATH "Pinned Zephyr BR/EDR security source directory")
  set(bluetooth_security_sources)
  set(bluetooth_security_inputs)
  set(bluetooth_manifest_security)
  if(SUNSTONE_BT_SECURITY_DIR)
    set(bluetooth_security_build "${bluetooth_build}/security")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${bluetooth_root}/apps/bluetooth/security-source.py"
      --source "${SUNSTONE_BT_SECURITY_DIR}" --output "${bluetooth_security_build}"
      RESULT_VARIABLE bluetooth_security_status)
    if(NOT bluetooth_security_status EQUAL 0)
      message(FATAL_ERROR "Pinned Bluetooth security source verification failed")
    endif()
    list(APPEND bluetooth_security_sources
      "${bluetooth_security_build}/zephyr-ssp.c" "${bluetooth_security_build}/zephyr-keys_br.c"
      "${bluetooth_security_build}/zephyr-security-level.c"
      "${bluetooth_root}/drivers/bluetooth/bt_security.c" "${bluetooth_root}/apps/bluetooth/headset.c"
      "${bluetooth_root}/drivers/bluetooth/bt_acl.c" "${bluetooth_root}/drivers/bluetooth/bt_l2cap.c"
      "${bluetooth_root}/drivers/bluetooth/bt_sdp.c" "${bluetooth_root}/drivers/bluetooth/bt_sdp_server.c"
      "${bluetooth_root}/drivers/bluetooth/bt_rfcomm.c" "${bluetooth_root}/drivers/bluetooth/bt_hfp_ag.c"
      "${bluetooth_root}/drivers/bluetooth/bt_sco.c")
    file(READ "${bluetooth_root}/apps/bluetooth/security-source.json" bluetooth_security_record)
    string(JSON bluetooth_security_count LENGTH "${bluetooth_security_record}" sources)
    math(EXPR bluetooth_security_last "${bluetooth_security_count}-1")
    foreach(index RANGE 0 ${bluetooth_security_last})
      string(JSON name MEMBER "${bluetooth_security_record}" sources ${index})
      list(APPEND bluetooth_security_inputs "${SUNSTONE_BT_SECURITY_DIR}/${name}")
    endforeach()
    list(APPEND bluetooth_security_inputs "${bluetooth_root}/apps/bluetooth/security-source.json"
      "${bluetooth_root}/apps/bluetooth/security-source.py" "${bluetooth_root}/include/bt_security.h"
      "${bluetooth_root}/include/bt_security_port.h" "${bluetooth_security_build}/zephyr-security-wire.h")
    list(APPEND bluetooth_manifest_security --security-inputs "${bluetooth_security_build}/security-inputs.json")
  endif()
  option(SUNSTONE_BLUETOOTH_MIMUS "Recognize a finite authenticated headset capture with unchanged Mimus" OFF)
  option(SUNSTONE_BLUETOOTH_MSBC "Enable opt-in native mSBC capture with pinned libsbc" OFF)
  if(SUNSTONE_BLUETOOTH_MSBC AND NOT SUNSTONE_BLUETOOTH_MIMUS)
    message(FATAL_ERROR "Wideband capture requires the existing recognition application")
  endif()
  set(bluetooth_manifest_recognition)
  set(bluetooth_recognition_sources)
  set(bluetooth_recognition_inputs)
  if(SUNSTONE_BLUETOOTH_MIMUS)
    if(NOT SUNSTONE_BT_SECURITY_DIR OR NOT TARGET mimus_asr_smp)
      message(FATAL_ERROR "Headset recognition requires pinned security and existing SMP Mimus/newlib targets")
    endif()
    if(SUNSTONE_BLUETOOTH_MSBC)
    set(SUNSTONE_BT_SBC_DIR "" CACHE PATH "Pinned BlueZ libsbc2.0 source directory")
    if(NOT SUNSTONE_BT_SBC_DIR)
      message(FATAL_ERROR "Recognition image requires pinned libsbc source for opt-in wideband capture")
    endif()
    file(READ "${bluetooth_root}/apps/bluetooth/msbc-source.json" bluetooth_sbc_manifest)
    string(JSON bluetooth_sbc_count LENGTH "${bluetooth_sbc_manifest}" sources)
    math(EXPR bluetooth_sbc_last "${bluetooth_sbc_count}-1")
    foreach(index RANGE 0 ${bluetooth_sbc_last})
      string(JSON name GET "${bluetooth_sbc_manifest}" sources ${index} path)
      string(JSON expected GET "${bluetooth_sbc_manifest}" sources ${index} sha256)
      file(SHA256 "${SUNSTONE_BT_SBC_DIR}/${name}" actual)
      if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Pinned libsbc input differs:${name}")
      endif()
      list(APPEND bluetooth_sbc_inputs "${SUNSTONE_BT_SBC_DIR}/${name}")
    endforeach()
    list(APPEND bluetooth_sbc_inputs "${bluetooth_root}/apps/bluetooth/msbc-source.json")
    set(bluetooth_sbc_sources "${bluetooth_root}/apps/bluetooth/msbc.c" "${SUNSTONE_BT_SBC_DIR}/sbc/sbc.c" "${SUNSTONE_BT_SBC_DIR}/sbc/sbc_primitives.c")
    list(APPEND bluetooth_manifest_recognition --msbc-inputs "${bluetooth_root}/apps/bluetooth/msbc-source.json")
    endif()
    set(bluetooth_recognition_sources ${bluetooth_sbc_sources}
      "${bluetooth_root}/apps/bluetooth/recognition.cc" "${bluetooth_root}/apps/bluetooth/pcm.c"
      "${bluetooth_root}/apps/mimus/recorded.cc" "${bluetooth_root}/apps/mimus/platform.cc"
      "${bluetooth_root}/runtime/newlib.c" "${bluetooth_root}/runtime/newlib_smp.c"
      "${bluetooth_root}/runtime/newlib_syscalls_smp.c" "${bluetooth_root}/runtime/new_delete.cc"
      "${bluetooth_root}/runtime/cxx_terminate.cc"
      "${MIMUS_DIR}/firmware/components/pipeline/src/formatter.cc")
    list(APPEND bluetooth_manifest_recognition --recognition-inputs "${bluetooth_root}/apps/bluetooth/recognition-models.json")
    file(READ "${bluetooth_root}/apps/bluetooth/recognition-models.json" bluetooth_models)
    set(bluetooth_model_sizes "")
    set(bluetooth_model_hashes "")
    foreach(index RANGE 0 1)
      string(JSON bytes GET "${bluetooth_models}" models ${index} bytes)
      string(JSON hash GET "${bluetooth_models}" models ${index} sha256)
      string(APPEND bluetooth_model_sizes "${bytes},")
      string(REGEX REPLACE "(..)" "0x\\1," hash_bytes "${hash}")
      string(APPEND bluetooth_model_hashes "{${hash_bytes}},")
    endforeach()
    file(WRITE "${bluetooth_build}/recognition-models.h"
      "static const uint64_t bluetooth_model_bytes[2]={${bluetooth_model_sizes}};\nstatic const uint8_t bluetooth_model_sha256[2][32]={${bluetooth_model_hashes}};\n")
    apply_build_patch("${bluetooth_build}/recognition-link.ld" "${storage_build}/link.ld" "${bluetooth_root}/apps/mimus/link.patch")
    get_target_property(bluetooth_asr_inputs mimus_asr_smp SOURCES)
    file(GLOB_RECURSE bluetooth_asr_headers "${MIMUS_DIR}/firmware/components/asr/include/*.h"
      "${MIMUS_DIR}/firmware/components/pipeline/include/*.h")
    set(bluetooth_recognition_inputs ${bluetooth_sbc_inputs} ${bluetooth_recognition_sources} ${bluetooth_asr_inputs} ${bluetooth_asr_headers}
      "${bluetooth_root}/apps/bluetooth/recognition-models.json" "${bluetooth_root}/apps/mimus/recorded.h"
      "${bluetooth_root}/apps/mimus/source.json" "${bluetooth_root}/apps/mimus/link.patch"
      "${bluetooth_build}/recognition-models.h" "${bluetooth_build}/recognition-link.ld"
      "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
      "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a" "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
  endif()
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
  if(SUNSTONE_BLUETOOTH_MIMUS)
    list(REMOVE_ITEM bluetooth_sources "${bluetooth_root}/runtime/memory.c" "${bluetooth_root}/runtime/memmove.c")
  endif()
  list(APPEND bluetooth_sources ${bluetooth_security_sources} ${bluetooth_recognition_sources})
  add_executable(sunstone-a733-bluetooth EXCLUDE_FROM_ALL ${bluetooth_sources}
    $<TARGET_OBJECTS:a733-board-usb2-host>)
  if(SUNSTONE_BLUETOOTH_MSBC)
    set_source_files_properties("${SUNSTONE_BT_SBC_DIR}/sbc/sbc.c" "${SUNSTONE_BT_SBC_DIR}/sbc/sbc_primitives.c"
      PROPERTIES COMPILE_DEFINITIONS SBC_HIGH_PRECISION=1)
  endif()
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
  if(SUNSTONE_BT_SECURITY_DIR)
    target_compile_definitions(sunstone-a733-bluetooth PRIVATE SUNSTONE_BLUETOOTH_HEADSET=1)
    target_include_directories(sunstone-a733-bluetooth PRIVATE "${bluetooth_security_build}")
  endif()
  target_include_directories(sunstone-a733-bluetooth BEFORE PRIVATE "${bluetooth_root}/apps/bluetooth" "${bluetooth_build}")
  if(SUNSTONE_BLUETOOTH_MIMUS)
    get_target_property(bluetooth_includes sunstone-a733-bluetooth INCLUDE_DIRECTORIES)
    list(REMOVE_ITEM bluetooth_includes "${bluetooth_root}/runtime/include" "${SUNSTONE_NETWORK_C_HEADERS}")
    set_property(TARGET sunstone-a733-bluetooth PROPERTY INCLUDE_DIRECTORIES "${bluetooth_includes}")
    sunstone_smp_compile(sunstone-a733-bluetooth)
    if(SUNSTONE_BLUETOOTH_MSBC)
      target_include_directories(sunstone-a733-bluetooth PRIVATE "${SUNSTONE_BT_SBC_DIR}")
      target_compile_definitions(sunstone-a733-bluetooth PRIVATE SUNSTONE_BLUETOOTH_MSBC=1)
    endif()
    target_include_directories(sunstone-a733-bluetooth PRIVATE "${MIMUS_DIR}/firmware/components/pipeline/include")
    target_compile_definitions(sunstone-a733-bluetooth PRIVATE SUNSTONE_BLUETOOTH_RECOGNITION=1 "$<$<COMPILE_LANGUAGE:CXX>:SUNSTONE_MIMUS_NATIVE=1>")
    target_link_libraries(sunstone-a733-bluetooth PRIVATE mimus_asr_smp
      "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
      "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a" "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
  endif()
  get_target_property(bluetooth_link sunstone-a733-bluetooth LINK_OPTIONS)
  list(FILTER bluetooth_link EXCLUDE REGEX "Map,")
  set_property(TARGET sunstone-a733-bluetooth PROPERTY LINK_OPTIONS "${bluetooth_link}")
  if(SUNSTONE_BLUETOOTH_MIMUS)
    list(FILTER bluetooth_link EXCLUDE REGEX "-T,")
    set_property(TARGET sunstone-a733-bluetooth PROPERTY LINK_OPTIONS "${bluetooth_link}")
    target_link_options(sunstone-a733-bluetooth PRIVATE "-Wl,-T,${bluetooth_build}/recognition-link.ld"
      -Wl,--wrap=_impure_ptr -Wl,--defsym=__runtime_heap_size=4194304)
  endif()
  target_link_options(sunstone-a733-bluetooth PRIVATE "-Wl,-Map,${bluetooth_build}/image.map")
  set(bluetooth_inputs ${board_inputs} ${bluetooth_sources} ${bluetooth_security_inputs} ${bluetooth_recognition_inputs} "${CMAKE_CURRENT_LIST_FILE}" "${bluetooth_build}/bluetooth-usb-base.c")
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
      --output "${bluetooth_build}/image-manifest.json" ${bluetooth_manifest_security} ${bluetooth_manifest_recognition}
    COMMAND_EXPAND_LISTS VERBATIM)
