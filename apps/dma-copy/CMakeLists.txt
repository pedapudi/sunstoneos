cmake_minimum_required(VERSION 3.24)
project(a733_dma_copy LANGUAGES C ASM)
get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(FREERTOS_DIR "" CACHE PATH "Pinned FreeRTOS kernel source")
set(TINYUSB_DIR "" CACHE PATH "Pinned TinyUSB source")
set(A733_DMA_HANDOFF_SOURCE "" CACHE FILEPATH "Immutable board-qualified DMA handoff source")
if(NOT EXISTS "${A733_DMA_HANDOFF_SOURCE}" OR IS_DIRECTORY "${A733_DMA_HANDOFF_SOURCE}")
  message(FATAL_ERROR "Supply A733_DMA_HANDOFF_SOURCE defining a733_dma_copy_handoff; no default admission is provided")
endif()
get_filename_component(A733_DMA_HANDOFF_SOURCE "${A733_DMA_HANDOFF_SOURCE}" ABSOLUTE)
function(apply_build_patch output source patch_file)
  execute_process(COMMAND patch --silent --output "${output}.tmp" "${source}" "${patch_file}"
    RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "Could not apply ${patch_file}")
  endif()
  configure_file("${output}.tmp" "${output}" COPYONLY)
  file(REMOVE "${output}.tmp")
endfunction()

# Reuse the native boot/translation/kernel inputs, replacing only its application,
# task-privilege hook and vectors. The diagnostic target remains excluded.
include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-smp-cached.cmake")
get_target_property(copy_sources a733-smp-cached-diagnostic SOURCES)
list(REMOVE_ITEM copy_sources
  "${a733_root}/platform/a733-smp-cached/diagnostic.c"
  "${a733_root}/platform/a733-smp-cached/continue.S"
  "${a733_root}/platform/a733-smp-cached/boot.c"
  "${a733_build}/port.c" "${a733_build}/build-id.c")
set(copy_build "${CMAKE_CURRENT_BINARY_DIR}/dma-copy")
file(MAKE_DIRECTORY "${copy_build}")
apply_build_patch("${copy_build}/port.c" "${a733_build}/port.c"
  "${a733_root}/patches/freertos-smp-privileged.patch")
apply_build_patch("${copy_build}/continue.S" "${a733_root}/platform/a733-smp-cached/continue.S"
  "${a733_root}/apps/dma-copy/continue.patch")
apply_build_patch("${copy_build}/boot.c" "${a733_root}/platform/a733-smp-cached/boot.c"
  "${a733_root}/apps/dma-copy/boot.patch")
apply_build_patch("${copy_build}/usb_service.c" "${a733_root}/platform/a733-smp-observe/usb_service.c"
  "${a733_root}/apps/dma-copy/usb-service.patch")
file(MAKE_DIRECTORY "${copy_build}/tinyusb")
foreach(pair "dcd_sunxi_musb.c|tinyusb-a733-dcd.patch" "musb_def.h|tinyusb-a733-registers.patch")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 src)
  list(GET parts 1 patch_name)
  apply_build_patch("${copy_build}/tinyusb/${src}" "${TINYUSB_DIR}/src/portable/sunxi/${src}"
    "${a733_root}/patches/${patch_name}")
endforeach()
set_source_files_properties("${copy_build}/port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=a733_dma_pool_task_privileged")
set(copy_units
  apps/dma-copy/main.c apps/dma-copy/result.c platform/a733/dma_pool_session.c
  platform/a733-smp-observe/usb.c platform/a733-smp-observe/result.c platform/a733-smp-observe/service.S
  platform/a733/memory_reservations.c runtime/device_memory_pool.c
  arch/aarch64/device_memory.c platform/a733/dma_native.c drivers/dma/dma_memory.c
  platform/a733/counter_clock.c platform/a733/shared_locks.c platform/a733/dma_startup.c
  drivers/clock/a733_clock_leases.c drivers/dma/a733_dma_platform.c
  drivers/dma/a733_dma_platform_native.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
  drivers/dma/dma_service.c drivers/dma/dma_service_i2s.c drivers/audio/a733_i2s.c
  drivers/dma/dma_service_i2s_stream.c drivers/audio/a733_i2s_stream.c)
list(TRANSFORM copy_units PREPEND "${a733_root}/")
list(APPEND copy_sources ${copy_units} "${A733_DMA_HANDOFF_SOURCE}"
  "${copy_build}/port.c" "${copy_build}/continue.S" "${copy_build}/boot.c" "${copy_build}/usb_service.c"
  "${TINYUSB_DIR}/src/tusb.c" "${TINYUSB_DIR}/src/common/tusb_fifo.c"
  "${TINYUSB_DIR}/src/device/usbd.c" "${TINYUSB_DIR}/src/class/vendor/vendor_device.c"
  "${copy_build}/tinyusb/dcd_sunxi_musb.c")
set(copy_inputs ${a733_identity_inputs} ${copy_sources}
  "${CMAKE_CURRENT_LIST_FILE}" "${a733_root}/apps/dma-copy/continue.patch"
  "${a733_root}/patches/freertos-smp-privileged.patch")
file(GLOB_RECURSE copy_usb_headers "${TINYUSB_DIR}/src/*.h")
list(APPEND copy_inputs ${copy_usb_headers}
  "${a733_root}/apps/dma-copy/boot.patch" "${a733_root}/apps/dma-copy/usb-service.patch"
  "${a733_root}/platform/a733-smp-observe/usb_service.c"
  "${a733_root}/platform/a733-smp-observe/result.h" "${a733_root}/platform/a733-smp-observe/a733_usb.h"
  "${a733_root}/platform/a733-smp-observe/tusb_config.h" "${copy_build}/tinyusb/musb_def.h"
  "${TINYUSB_DIR}/src/portable/sunxi/dcd_sunxi_musb.c" "${TINYUSB_DIR}/src/portable/sunxi/musb_def.h"
  "${a733_root}/patches/tinyusb-a733-dcd.patch" "${a733_root}/patches/tinyusb-a733-registers.patch")
file(GLOB copy_headers "${a733_root}/include/*.h" "${a733_root}/runtime/include/*.h")
list(APPEND copy_inputs ${copy_headers})
list(APPEND copy_inputs
  "${a733_port}/port.c" "${a733_port}/portASM.S" "${a733_port}/portmacro.h"
  "${a733_root}/patches/freertos-smp-c.patch" "${a733_root}/patches/freertos-smp-asm.patch"
  "${a733_root}/patches/freertos-smp-macro.patch" "${a733_root}/patches/freertos-a733-smp.patch")
list(REMOVE_DUPLICATES copy_inputs)
string(REPLACE ";" "\n" copy_input_lines "${copy_inputs}")
file(WRITE "${copy_build}/identity-inputs.txt" "${copy_input_lines}\n")
# The final ELF hash is recorded separately; this binds the boot contract to the
# selected application, admission source, mappings and private kernel inputs.
set(copy_identity "dma-copy;${a733_identity};pool=2097152;privileged-bootstrap-worker")
foreach(input IN LISTS copy_inputs)
  file(SHA256 "${input}" digest)
  string(APPEND copy_identity "${digest}")
endforeach()
string(SHA256 copy_build_id "${copy_identity}")
string(REGEX REPLACE "(..)" "0x\\1," copy_id_bytes "${copy_build_id}")
file(WRITE "${copy_build}/build-id.c.tmp"
  "const unsigned char a733_smp_build_id[32]={${copy_id_bytes}};\n")
configure_file("${copy_build}/build-id.c.tmp" "${copy_build}/build-id.c" COPYONLY)
file(WRITE "${copy_build}/build-id.txt" "${copy_build_id}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${copy_inputs})
add_executable(a733-dma-copy ${copy_sources} "${copy_build}/build-id.c")
foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS)
  get_target_property(value a733-smp-cached-diagnostic ${property})
  set_property(TARGET a733-dma-copy PROPERTY ${property} "${value}")
endforeach()
target_include_directories(a733-dma-copy PRIVATE "${a733_root}/platform/a733-smp-observe"
  "${a733_root}/drivers/usb" "${TINYUSB_DIR}/src")
target_compile_definitions(a733-dma-copy PRIVATE SUNSTONE_SECURE_MONITOR=1 SUNSTONE_A733_SMP_DIAGNOSTIC=1
  SUNSTONE_A733_USB=1 A733_SMP_CONTRACT_MAGIC=0x415337334f424331ULL A733_SMP_RESULT_MAGIC=0x41533733444d5231ULL
  INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1
  INCLUDE_xSemaphoreGetMutexHolder=1)
target_link_options(a733-dma-copy PRIVATE "-Wl,--defsym=__device_pool_size=2097152"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/a733-dma-copy.map")
set_target_properties(a733-dma-copy PROPERTIES SUFFIX ".elf")
