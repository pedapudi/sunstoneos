# Integrated OS image (docs/os-image.md): the mimus board composition (USB0
# service, USB2 host, keyboard, USB storage, native mimus) plus a continuous
# thermal service, one fixed SPI table and one fixed start list. Supervised
# builds use the FEL-launched 60 s contract; resident builds use the stage-2
# loader's contract and renew the watchdog from service progress. The audio
# option adds I2S0 microphone receive through the DMA service and feeds each
# capture to the recognizer; it extends the session past the 808 bytes that
# the stage-2 loader admits, so it is supervised only.
option(SUNSTONE_A733_OS "Build the integrated OS image" OFF)
option(SUNSTONE_A733_OS_RESIDENT "Integrated OS with the resident lifetime" OFF)
option(SUNSTONE_A733_OS_AUDIO "Integrated OS with I2S microphone recognition" OFF)
if(NOT SUNSTONE_A733_OS)
  return()
endif()
if(SUNSTONE_A733_OS_AUDIO AND SUNSTONE_A733_OS_RESIDENT)
  message(FATAL_ERROR "The stage-2 loader admits only the 808-byte OS session; audio is supervised only")
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
set(os_added apps/os/admission.c apps/os/irq.c apps/os/services.c apps/os/start.c apps/os/thermal.c
  apps/os/watchdog.c platform/a733-smp-cached/irq_table.c drivers/thermal/a733_thermal.c
  drivers/thermal/a733_thermal_native.c drivers/gpu/gicv3_spi.c)
set(os_patches "${os_root}/apps/os/boot.patch")
if(SUNSTONE_A733_OS_AUDIO)
  # The I2S image's receive composition (cmake/a733-i2s-application.cmake).
  list(APPEND os_added apps/os/audio.c platform/a733/dma_pool_session.c platform/a733/dma_startup.c
    platform/a733/dma_native.c drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
    drivers/dma/a733_dma_platform.c drivers/dma/a733_dma_platform_native.c drivers/dma/dma_service.c
    drivers/dma/dma_service_i2s.c drivers/dma/dma_service_i2s_stream.c drivers/audio/a733_i2s.c
    drivers/audio/a733_i2s_stream.c drivers/audio/a733_i2s_platform.c drivers/audio/a733_i2s_platform_native.c
    drivers/audio/a733_audio_leases.c drivers/audio/dma_service_i2s_irq.c)
  # The recognizer takes each generation's WAV from the microphone capture.
  apply_build_patch("${os_build}/recognition.cc" "${os_root}/apps/mimus-board/recognition.cc"
    "${os_root}/apps/os/recognition.patch")
  list(REMOVE_ITEM os_sources "${os_root}/apps/mimus-board/recognition.cc")
  list(APPEND os_sources "${os_build}/recognition.cc")
  list(APPEND os_patches "${os_root}/apps/os/recognition.patch")
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
  "$<$<BOOL:${SUNSTONE_A733_OS_AUDIO}>:SUNSTONE_A733_OS_AUDIO=1>")
target_include_directories(sunstone-a733-os BEFORE PRIVATE "${os_root}/apps/os")
get_target_property(os_link sunstone-a733-os LINK_OPTIONS)
list(FILTER os_link EXCLUDE REGEX "Map,")
set_property(TARGET sunstone-a733-os PROPERTY LINK_OPTIONS "${os_link}")
target_link_options(sunstone-a733-os PRIVATE "-Wl,-Map,${os_build}/image.map")
file(GLOB os_headers "${os_root}/apps/os/*.h")
set(os_inputs ${mb_inputs} ${os_added} ${os_headers} "${os_build}/boot.c" ${os_patches}
  "${os_root}/platform/a733-smp-cached/irq_table.h" "${os_root}/include/a733_thermal.h"
  "${os_root}/include/a733_thermal_native.h" "${os_root}/include/gicv3_spi.h" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES os_inputs)
if(SUNSTONE_A733_OS_AUDIO)
  list(APPEND os_inputs "${os_build}/recognition.cc")
endif()
list(REMOVE_ITEM os_inputs "${mb_build}/build-id.c")
set(os_identity "native-os-1;resident=${SUNSTONE_A733_OS_RESIDENT};audio=${SUNSTONE_A733_OS_AUDIO};${MIMUS_AARCH64_NEON};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION}")
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
    --audio "$<BOOL:${SUNSTONE_A733_OS_AUDIO}>"
    --output "${os_build}/link-check.json"
  VERBATIM)
