# Integrated OS image (docs/os-image.md): the mimus board composition (USB0
# service, USB2 host, keyboard, USB storage, native mimus) plus a continuous
# thermal service, one fixed SPI table and one fixed start list. Supervised
# builds use the FEL-launched 60 s contract; resident builds use the stage-2
# loader's contract and renew the watchdog from service progress. The audio
# option adds I2S0 microphone receive through the DMA service and feeds each
# capture to the recognizer; it extends the session to 992 bytes, which the
# stage-2 loader also admits, and works in either lifetime. The radio option
# admits board workload 1, in which the board adds its Wi-Fi station, DHCP,
# TCP exchange and crypto engine entropy, in either lifetime. The headset
# option feeds Bluetooth headset segments to the recognizer instead, in either
# lifetime; with the radio, the board's radio owner owns the AIC8800 module
# and serves Wi-Fi and the Bluetooth client from one firmware load. The mSBC
# option adds wideband headset speech through the pinned libsbc. The GPU
# linear option places mimus linear layers on the CPU or the GPU from a
# generated table (docs/recognition-dispatch.md); it needs the resident
# lifetime and two build constants that other owners provide.
option(SUNSTONE_A733_OS "Build the integrated OS image" OFF)
option(SUNSTONE_A733_OS_RESIDENT "Integrated OS with the resident lifetime" OFF)
option(SUNSTONE_A733_OS_AUDIO "Integrated OS with I2S microphone recognition" OFF)
option(SUNSTONE_A733_OS_RADIO "Integrated OS with the board's radio station" OFF)
option(SUNSTONE_A733_OS_HEADSET "Integrated OS with Bluetooth headset recognition" OFF)
option(SUNSTONE_A733_OS_MSBC "Integrated OS headset with wideband mSBC speech" OFF)
option(SUNSTONE_A733_OS_GPU_LINEAR "Integrated OS with mimus linear layers on the GPU" OFF)
if(NOT SUNSTONE_A733_OS)
  return()
endif()
if(SUNSTONE_A733_OS_HEADSET AND SUNSTONE_A733_OS_AUDIO)
  message(FATAL_ERROR "The recognizer takes one capture source: the I2S microphone or the headset")
endif()
if(SUNSTONE_A733_OS_MSBC AND NOT SUNSTONE_A733_OS_HEADSET)
  message(FATAL_ERROR "Wideband mSBC speech is a headset option")
endif()
if(SUNSTONE_A733_OS_GPU_LINEAR)
  # The recognizer waits for GPU replies without a timer of its own; only the
  # resident lifetime's mimus service window and watchdog bound those waits.
  if(NOT SUNSTONE_A733_OS_RESIDENT)
    message(FATAL_ERROR "GPU linear layers need the resident lifetime, whose watchdog bounds every GPU wait")
  endif()
  # The page-table owner sets this once 0x46C00000-0x47000000 is mapped
  # read-only at EL0 as two 2 MiB blocks, so no CPU store can dirty a line of
  # the GPU's output region.
  if(NOT A733_GPU_OUTPUT_EL0_READ_ONLY)
    message(FATAL_ERROR "GPU linear layers need the GPU output region mapped read-only at EL0 (A733_GPU_OUTPUT_EL0_READ_ONLY)")
  endif()
  # The GPU kernel owner sets this once its completion uses DC CIVAC on the
  # output region: DC IVAC is checked as a write and faults on a read-only page.
  if(NOT A733_GPU_OUTPUT_CLEAN_INVALIDATE)
    message(FATAL_ERROR "GPU linear layers need clean-and-invalidate completion on the GPU output region (A733_GPU_OUTPUT_CLEAN_INVALIDATE)")
  endif()
endif()
if(NOT TARGET sunstone-a733-mimus-board)
  include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-mimus-board.cmake")
endif()
set(os_root "${SUNSTONE_SOURCE_ROOT}")
set(os_build "${CMAKE_CURRENT_BINARY_DIR}/os-image")
file(MAKE_DIRECTORY "${os_build}")
get_target_property(os_sources sunstone-a733-mimus-board SOURCES)
# The OS admission replaces the board's, and the OS boot replaces the
# supervised board boot; the mimus board build identity is not reused.
list(REMOVE_ITEM os_sources "$<TARGET_OBJECTS:a733-board-usb2-host>" "${board_build}/boot-supervised.c"
  "${os_root}/apps/board/admission.c" "${mb_build}/build-id.c")
# The OS publication hook and workload rule replace the mimus board's.
list(REMOVE_ITEM os_sources "${os_root}/apps/mimus-board/identity.c" "${os_root}/apps/mimus-board/application.c")
apply_build_patch("${os_build}/application.c" "${os_root}/apps/mimus-board/application.c"
  "${os_root}/apps/os/application.patch")
list(APPEND os_sources "${os_build}/application.c")
if(SUNSTONE_A733_OS_RESIDENT)
  # The armed board boot reads the watchdog, redirect and FEL registers back
  # against the contract; the resident contract arms the first and zeroes the rest.
  set(os_boot_base "${board_build}/boot.c")
  set(os_contract_magic 0x415337334f524331ULL)
  set(os_result_magic 0x415337334f525231ULL)
else()
  set(os_boot_base "${board_build}/boot-supervised.c")
  set(os_contract_magic 0x415337334f534331ULL)
  set(os_result_magic 0x415337334f535231ULL)
endif()
apply_build_patch("${os_build}/boot.c" "${os_boot_base}" "${os_root}/apps/os/boot.patch")
set(os_added apps/os/admission.c apps/os/identity.c apps/os/irq.c apps/os/services.c apps/os/start.c
  apps/os/thermal.c apps/os/watchdog.c platform/a733-smp-cached/irq_table.c drivers/thermal/a733_thermal.c
  drivers/thermal/a733_thermal_native.c drivers/gpu/gicv3_spi.c)
set(os_patches "${os_root}/apps/os/boot.patch" "${os_root}/apps/os/application.patch")
set(os_generated "${os_build}/boot.c" "${os_build}/application.c")
if(SUNSTONE_A733_OS_RADIO)
  # The board's own radio composition serves workload 1; its owner and client
  # report resident progress through the patched main.c.
  list(APPEND os_added apps/os/radio.c)
  apply_build_patch("${os_build}/main.c" "${os_root}/apps/board/main.c" "${os_root}/apps/os/main.patch")
  list(REMOVE_ITEM os_sources "${os_root}/apps/board/main.c")
  list(APPEND os_sources "${os_build}/main.c")
  list(APPEND os_patches "${os_root}/apps/os/main.patch")
  list(APPEND os_generated "${os_build}/main.c")
  if(SUNSTONE_A733_OS_RESIDENT)
    list(REMOVE_ITEM os_sources "${os_root}/apps/board/radio_profile.c")
    list(APPEND os_added apps/os/radio_profile.c)
  endif()
endif()
if(SUNSTONE_A733_OS_AUDIO)
  # The I2S image's receive composition (cmake/a733-i2s-application.cmake).
  list(APPEND os_added apps/os/audio.c platform/a733/dma_pool_session.c platform/a733/dma_startup.c
    platform/a733/dma_native.c drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
    drivers/dma/a733_dma_platform.c drivers/dma/a733_dma_platform_native.c drivers/dma/dma_service.c
    drivers/dma/dma_service_i2s.c drivers/dma/dma_service_i2s_stream.c drivers/audio/a733_i2s.c
    drivers/audio/a733_i2s_stream.c drivers/audio/a733_i2s_platform.c drivers/audio/a733_i2s_platform_native.c
    drivers/audio/a733_audio_leases.c drivers/audio/dma_service_i2s_irq.c)
endif()
set(os_include_dirs)
if(SUNSTONE_A733_OS_HEADSET)
  # The Bluetooth image's headset composition (cmake/a733-bluetooth.cmake),
  # without its USB0 output and its own recognizer: the OS recognizer takes
  # the headset segments. Version 3 profiles name the recognition models, which
  # are the mimus board's own (apps/bluetooth/recognition-models.json).
  set(SUNSTONE_BT_SECURITY_DIR "" CACHE PATH "Pinned Zephyr BR/EDR security source directory")
  if(NOT SUNSTONE_BT_SECURITY_DIR)
    message(FATAL_ERROR "The headset build requires SUNSTONE_BT_SECURITY_DIR")
  endif()
  set(os_bluetooth "${os_build}/bluetooth")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${os_root}/apps/bluetooth/security-source.py"
    --source "${SUNSTONE_BT_SECURITY_DIR}" --output "${os_bluetooth}" RESULT_VARIABLE os_security_status)
  if(NOT os_security_status EQUAL 0)
    message(FATAL_ERROR "Pinned Bluetooth security source verification failed")
  endif()
  file(READ "${os_root}/apps/bluetooth/security-source.json" os_security_record)
  string(JSON os_security_count LENGTH "${os_security_record}" sources)
  math(EXPR os_security_last "${os_security_count}-1")
  foreach(index RANGE 0 ${os_security_last})
    string(JSON name MEMBER "${os_security_record}" sources ${index})
    list(APPEND os_external_inputs "${SUNSTONE_BT_SECURITY_DIR}/${name}")
  endforeach()
  file(READ "${os_root}/apps/bluetooth/recognition-models.json" os_models)
  set(os_model_sizes "")
  set(os_model_hashes "")
  foreach(index RANGE 0 1)
    string(JSON bytes GET "${os_models}" models ${index} bytes)
    string(JSON hash GET "${os_models}" models ${index} sha256)
    string(APPEND os_model_sizes "${bytes},")
    string(REGEX REPLACE "(..)" "0x\\1," hash_bytes "${hash}")
    string(APPEND os_model_hashes "{${hash_bytes}},")
  endforeach()
  file(WRITE "${os_bluetooth}/recognition-models.h"
    "static const uint64_t bluetooth_model_bytes[2]={${os_model_sizes}};\nstatic const uint8_t bluetooth_model_sha256[2][32]={${os_model_hashes}};\n")
  list(APPEND os_added apps/os/headset.c apps/bluetooth/bluetooth.c apps/bluetooth/headset.c apps/bluetooth/pcm.c
    drivers/uart/a733_uart.c drivers/uart/a733_uart_platform.c drivers/uart/a733_uart_platform_native.c
    drivers/bluetooth/bt_h4.c drivers/bluetooth/bt_hci.c drivers/bluetooth/bt_security.c drivers/bluetooth/bt_acl.c
    drivers/bluetooth/bt_l2cap.c drivers/bluetooth/bt_sdp.c drivers/bluetooth/bt_sdp_server.c
    drivers/bluetooth/bt_rfcomm.c drivers/bluetooth/bt_hfp_ag.c drivers/bluetooth/bt_sco.c)
  list(APPEND os_sources "${os_bluetooth}/zephyr-ssp.c" "${os_bluetooth}/zephyr-keys_br.c"
    "${os_bluetooth}/zephyr-security-level.c")
  list(APPEND os_external_inputs "${os_root}/apps/bluetooth/security-source.json"
    "${os_root}/apps/bluetooth/security-source.py" "${os_root}/apps/bluetooth/recognition-models.json"
    "${os_bluetooth}/zephyr-security-wire.h" "${os_bluetooth}/recognition-models.h")
  file(GLOB os_bluetooth_headers "${os_root}/apps/bluetooth/*.h")
  list(APPEND os_external_inputs ${os_bluetooth_headers})
  list(APPEND os_include_dirs "${os_bluetooth}")
  if(SUNSTONE_A733_OS_MSBC)
    # The Bluetooth image's mSBC codec (cmake/a733-bluetooth.cmake) with its
    # pinned libsbc sources, checked against apps/bluetooth/msbc-source.json.
    set(SUNSTONE_BT_SBC_DIR "" CACHE PATH "Pinned BlueZ libsbc2.0 source directory")
    if(NOT SUNSTONE_BT_SBC_DIR)
      message(FATAL_ERROR "The mSBC headset build requires SUNSTONE_BT_SBC_DIR")
    endif()
    file(READ "${os_root}/apps/bluetooth/msbc-source.json" os_sbc_manifest)
    string(JSON os_sbc_count LENGTH "${os_sbc_manifest}" sources)
    math(EXPR os_sbc_last "${os_sbc_count}-1")
    foreach(index RANGE 0 ${os_sbc_last})
      string(JSON name GET "${os_sbc_manifest}" sources ${index} path)
      string(JSON expected GET "${os_sbc_manifest}" sources ${index} sha256)
      file(SHA256 "${SUNSTONE_BT_SBC_DIR}/${name}" actual)
      if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Pinned libsbc input differs: ${name}")
      endif()
      list(APPEND os_external_inputs "${SUNSTONE_BT_SBC_DIR}/${name}")
    endforeach()
    list(APPEND os_external_inputs "${os_root}/apps/bluetooth/msbc-source.json")
    list(APPEND os_added apps/bluetooth/msbc.c apps/bluetooth/playback.c)
    set(os_sbc_sources "${SUNSTONE_BT_SBC_DIR}/sbc/sbc.c" "${SUNSTONE_BT_SBC_DIR}/sbc/sbc_primitives.c")
    list(APPEND os_sources ${os_sbc_sources})
    list(APPEND os_include_dirs "${SUNSTONE_BT_SBC_DIR}")
  endif()
endif()
set(os_recognition "${os_root}/apps/mimus-board/recognition.cc")
if(SUNSTONE_A733_OS_AUDIO OR SUNSTONE_A733_OS_HEADSET)
  # The recognizer takes each generation's WAV from the microphone or headset capture.
  apply_build_patch("${os_build}/recognition.cc" "${os_recognition}" "${os_root}/apps/os/recognition.patch")
  set(os_recognition "${os_build}/recognition.cc")
  list(APPEND os_patches "${os_root}/apps/os/recognition.patch")
endif()
if(SUNSTONE_A733_OS_GPU_LINEAR)
  # The placement table comes from the models the image embeds by hash, for
  # the recognition core pair: cores 6 and 7 once the A76 cores are admitted,
  # cores 2 and 3 until then.
  set(SUNSTONE_A733_OS_MODEL_DIR "" CACHE PATH "Directory with base.mimus and overlay.mimus for the GPU placement table")
  if(NOT SUNSTONE_A733_OS_MODEL_DIR)
    message(FATAL_ERROR "The GPU linear build requires SUNSTONE_A733_OS_MODEL_DIR")
  endif()
  if(A733_A76_ADMITTED)
    set(os_cores a76)
    set(os_recognition_core 6)
  else()
    set(os_cores a55)
    set(os_recognition_core 2)
  endif()
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${os_root}/apps/os/placement.py"
    --models "${SUNSTONE_A733_OS_MODEL_DIR}/base.mimus" "${SUNSTONE_A733_OS_MODEL_DIR}/overlay.mimus"
    --cores ${os_cores} --output "${os_build}/placement.h" RESULT_VARIABLE os_placement_status)
  if(NOT os_placement_status EQUAL 0)
    message(FATAL_ERROR "GPU placement table generation failed")
  endif()
  # The recognizer opens the GPU session after the model hashes and runs on
  # the recognition core; the hook ends the boot through its failure path.
  apply_build_patch("${os_build}/recognition-gpu.cc" "${os_recognition}" "${os_root}/apps/os/recognition-dispatch.patch")
  set(os_recognition "${os_build}/recognition-gpu.cc")
  list(APPEND os_patches "${os_root}/apps/os/recognition-dispatch.patch")
  list(APPEND os_added apps/os/dispatch.c)
  list(APPEND os_external_inputs "${os_root}/apps/os/placement.py" "${os_build}/placement.h"
    "${SUNSTONE_A733_OS_MODEL_DIR}/base.mimus" "${SUNSTONE_A733_OS_MODEL_DIR}/overlay.mimus")
  list(APPEND os_include_dirs "${os_build}")
endif()
if(NOT os_recognition STREQUAL "${os_root}/apps/mimus-board/recognition.cc")
  list(REMOVE_ITEM os_sources "${os_root}/apps/mimus-board/recognition.cc")
  list(APPEND os_sources "${os_recognition}")
endif()
list(TRANSFORM os_added PREPEND "${os_root}/")
list(APPEND os_sources "${os_build}/boot.c" ${os_added})
list(REMOVE_DUPLICATES os_sources)
add_executable(sunstone-a733-os EXCLUDE_FROM_ALL ${os_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
foreach(property COMPILE_OPTIONS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_FEATURES LINK_OPTIONS LINK_LIBRARIES)
  get_target_property(value sunstone-a733-mimus-board ${property})
  if(NOT value STREQUAL "value-NOTFOUND")
    set_property(TARGET sunstone-a733-os PROPERTY ${property} "${value}")
  endif()
endforeach()
get_target_property(os_definitions sunstone-a733-os COMPILE_DEFINITIONS)
list(FILTER os_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-os PROPERTY COMPILE_DEFINITIONS "${os_definitions}")
target_compile_definitions(sunstone-a733-os PRIVATE SUNSTONE_A733_OS=1 SUNSTONE_A733_IRQ_TABLE=1
  A733_SMP_CONTRACT_MAGIC=${os_contract_magic} A733_SMP_RESULT_MAGIC=${os_result_magic}
  "$<$<BOOL:${SUNSTONE_A733_OS_RESIDENT}>:SUNSTONE_BOARD_RESIDENT=1>"
  "$<$<BOOL:${SUNSTONE_A733_OS_AUDIO}>:SUNSTONE_A733_OS_AUDIO=1>"
  "$<$<BOOL:${SUNSTONE_A733_OS_RADIO}>:SUNSTONE_A733_OS_RADIO=1>"
  "$<$<BOOL:${SUNSTONE_A733_OS_HEADSET}>:SUNSTONE_A733_OS_HEADSET=1>")
if(SUNSTONE_A733_OS_GPU_LINEAR)
  target_compile_definitions(sunstone-a733-os PRIVATE SUNSTONE_A733_OS_GPU_LINEAR=1
    OS_RECOGNITION_CORE=${os_recognition_core})
endif()
target_include_directories(sunstone-a733-os BEFORE PRIVATE "${os_root}/apps/os")
if(os_include_dirs)
  target_include_directories(sunstone-a733-os PRIVATE ${os_include_dirs})
endif()
if(SUNSTONE_A733_OS_HEADSET)
  target_compile_definitions(sunstone-a733-os PRIVATE SUNSTONE_BLUETOOTH_HEADSET=1 SUNSTONE_BLUETOOTH_RECOGNITION=1)
endif()
if(SUNSTONE_A733_OS_MSBC)
  target_compile_definitions(sunstone-a733-os PRIVATE SUNSTONE_A733_OS_MSBC=1 SUNSTONE_BLUETOOTH_MSBC=1)
  set_source_files_properties(${os_sbc_sources} TARGET_DIRECTORY sunstone-a733-os
    PROPERTIES COMPILE_DEFINITIONS SBC_HIGH_PRECISION=1)
endif()
get_target_property(os_link sunstone-a733-os LINK_OPTIONS)
list(FILTER os_link EXCLUDE REGEX "Map,")
set_property(TARGET sunstone-a733-os PROPERTY LINK_OPTIONS "${os_link}")
target_link_options(sunstone-a733-os PRIVATE "-Wl,-Map,${os_build}/image.map")
file(GLOB os_headers "${os_root}/apps/os/*.h")
set(os_inputs ${mb_inputs} ${os_added} ${os_headers} ${os_generated} ${os_patches} ${os_external_inputs}
  "${os_root}/platform/a733-smp-cached/irq_table.h" "${os_root}/include/a733_thermal.h"
  "${os_root}/include/a733_thermal_native.h" "${os_root}/include/gicv3_spi.h" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES os_inputs)
if(NOT os_recognition STREQUAL "${os_root}/apps/mimus-board/recognition.cc")
  list(APPEND os_inputs "${os_recognition}")
endif()
list(REMOVE_ITEM os_inputs "${mb_build}/build-id.c")
set(os_identity "native-os-1;resident=${SUNSTONE_A733_OS_RESIDENT};audio=${SUNSTONE_A733_OS_AUDIO};radio=${SUNSTONE_A733_OS_RADIO};headset=${SUNSTONE_A733_OS_HEADSET};msbc=${SUNSTONE_A733_OS_MSBC};gpu_linear=${SUNSTONE_A733_OS_GPU_LINEAR};${MIMUS_AARCH64_NEON};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION}")
foreach(input IN LISTS os_inputs)
  file(SHA256 "${input}" hash)
  string(APPEND os_identity "${hash}")
endforeach()
string(SHA256 os_build_id "${os_identity}")
string(REGEX REPLACE "(..)" "0x\\1," os_id_bytes "${os_build_id}")
file(WRITE "${os_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${os_id_bytes}};\n")
file(WRITE "${os_build}/build-id.txt" "${os_build_id}\n")
target_sources(sunstone-a733-os PRIVATE "${os_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${os_inputs})
set_target_properties(sunstone-a733-os PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${mb_build}/link.ld")
# The link check refuses duplicate or unlinked table and start-list entries
# and recognition regions that overlap the image or leave the loader window.
add_custom_command(TARGET sunstone-a733-os POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${os_root}/checks/os-image-link.py"
    --image "$<TARGET_FILE:sunstone-a733-os>" --build-id "${os_build}/build-id.txt"
    --assets "${mb_build}/assets.h" --recognizer "${os_root}/apps/mimus/recorded.h"
    --loader "${os_root}/boot/a733/stage2/loader.c"
    --lifetime "$<IF:$<BOOL:${SUNSTONE_A733_OS_RESIDENT}>,resident,supervised>"
    --audio "$<BOOL:${SUNSTONE_A733_OS_AUDIO}>" --radio "$<BOOL:${SUNSTONE_A733_OS_RADIO}>"
    --headset "$<BOOL:${SUNSTONE_A733_OS_HEADSET}>" --gpu-linear "$<BOOL:${SUNSTONE_A733_OS_GPU_LINEAR}>"
    --output "${os_build}/link-check.json"
  VERBATIM)
