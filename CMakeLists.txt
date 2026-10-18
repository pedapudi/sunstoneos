cmake_minimum_required(VERSION 3.24)
project(sunstoneos LANGUAGES C ASM)

set(FREERTOS_DIR "" CACHE PATH "Pinned FreeRTOS kernel source directory")
if(NOT EXISTS "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE/port.c")
  message(FATAL_ERROR "Set FREERTOS_DIR to the kernel fetched by tools/fetch-sources.py")
endif()

set(PORT "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
function(apply_build_patch output source patch_file)
  execute_process(COMMAND patch --silent --output "${output}.tmp" "${source}" "${patch_file}"
    RESULT_VARIABLE patch_status)
  if(NOT patch_status EQUAL 0)
    message(FATAL_ERROR "Could not apply ${patch_file}")
  endif()
  # Preserve the generated source timestamp when patch output is unchanged.
  configure_file("${output}.tmp" "${output}" COPYONLY)
  file(REMOVE "${output}.tmp")
endfunction()
apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c" "${PORT}/port.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-aarch64-clang.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${PORT}/port.c" patches/freertos-aarch64-clang.patch)
add_executable(kernel-check
  arch/aarch64/start.S
  arch/aarch64/vectors.S
  platform/qemu-virt/platform.c
  runtime/memory.c
  checks/kernel.c
  checks/fp-context.S
  "${FREERTOS_DIR}/tasks.c"
  "${FREERTOS_DIR}/queue.c"
  "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c"
  "${PORT}/portASM.S")
target_include_directories(kernel-check PRIVATE
  include runtime/include "${FREERTOS_DIR}/include" "${PORT}")
target_compile_definitions(kernel-check PRIVATE GUEST=1)
target_compile_options(kernel-check PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra)
target_link_options(kernel-check PRIVATE
  -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-virt/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/kernel-check.map")
set_target_properties(kernel-check PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-virt/link.ld")

# The physical platform and its secure QEMU model share the monitor and kernel.
option(SUNSTONE_EXPERIMENTAL_UFS "Build the unqualified UFS probe; hardware watchdog recovery is required before execution" OFF)
set(kernel_targets a733-kernel-check a733-kernel-model a733-monitor-model a733-storage-probe a733-power-probe)
if(SUNSTONE_EXPERIMENTAL_UFS)
  list(APPEND kernel_targets a733-ufs-probe)
endif()
foreach(target IN LISTS kernel_targets)
  set(check_source checks/kernel.c)
  if(target STREQUAL "a733-monitor-model")
    set(check_source checks/monitor-renew.c)
  elseif(target STREQUAL "a733-storage-probe")
    set(check_source checks/storage-probe.c)
  elseif(target STREQUAL "a733-power-probe")
    set(check_source checks/power-probe.c drivers/i2c/a733_i2c.c)
  elseif(target STREQUAL "a733-ufs-probe")
    set(check_source checks/ufs-probe.c drivers/i2c/a733_i2c.c)
  endif()
  add_executable(${target}
    platform/a733/monitor.S platform/a733/monitor.c platform/a733/platform.c
    arch/aarch64/cache.S arch/aarch64/start.S arch/aarch64/vectors.S
    runtime/memory.c "${check_source}" checks/fp-context.S
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c" "${PORT}/portASM.S")
  target_include_directories(${target} PRIVATE
    include runtime/include platform/a733 "${FREERTOS_DIR}/include" "${PORT}")
  target_compile_definitions(${target} PRIVATE GUEST=1 SUNSTONE_SECURE_MONITOR=1)
  target_compile_options(${target} PRIVATE
    -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
    -fno-unwind-tables -fno-asynchronous-unwind-tables
    -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra)
  target_link_options(${target} PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/a733/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
  set_target_properties(${target} PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/a733/link.ld")
endforeach()
set_source_files_properties(platform/a733/monitor.c PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
target_compile_definitions(a733-kernel-model PRIVATE MONITOR_QEMU=1)
target_compile_definitions(a733-monitor-model PRIVATE MONITOR_QEMU=1 SUNSTONE_USB_RUNTIME=1)

set(TINYUSB_DIR "" CACHE PATH "Pinned TinyUSB source directory")
if(TINYUSB_DIR)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tinyusb-a733")
  foreach(pair "dcd_sunxi_musb.c|tinyusb-a733-dcd.patch" "musb_def.h|tinyusb-a733-registers.patch")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 source)
    list(GET parts 1 patch_file)
    apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/tinyusb-a733/${source}"
      "${TINYUSB_DIR}/src/portable/sunxi/${source}"
      "${CMAKE_CURRENT_SOURCE_DIR}/patches/${patch_file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${TINYUSB_DIR}/src/portable/sunxi/${source}" "patches/${patch_file}")
  endforeach()
  add_executable(a733-usb-runtime
    platform/a733/monitor.S platform/a733/monitor.c platform/a733/platform.c
    arch/aarch64/cache.S arch/aarch64/start.S arch/aarch64/vectors.S runtime/memory.c
    drivers/usb/a733_usb.c drivers/usb/diagnostic.c
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${CMAKE_CURRENT_BINARY_DIR}/freertos-port.c" "${PORT}/portASM.S"
    "${TINYUSB_DIR}/src/tusb.c" "${TINYUSB_DIR}/src/common/tusb_fifo.c"
    "${TINYUSB_DIR}/src/device/usbd.c" "${TINYUSB_DIR}/src/class/vendor/vendor_device.c"
    "${CMAKE_CURRENT_BINARY_DIR}/tinyusb-a733/dcd_sunxi_musb.c")
  target_include_directories(a733-usb-runtime PRIVATE include runtime/include platform/a733
    drivers/usb "${FREERTOS_DIR}/include" "${PORT}" "${TINYUSB_DIR}/src")
  target_compile_definitions(a733-usb-runtime PRIVATE
    GUEST=1 SUNSTONE_SECURE_MONITOR=1 SUNSTONE_USB_RUNTIME=1 SUNSTONE_A733_USB=1)
  target_compile_options(a733-usb-runtime PRIVATE
    -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
    -fno-unwind-tables -fno-asynchronous-unwind-tables
    -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra)
  target_link_options(a733-usb-runtime PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/a733/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/a733-usb-runtime.map")
  set_target_properties(a733-usb-runtime PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/a733/link.ld")
endif()

include(cmake/cxx-runtime.cmake)
include(cmake/mimus.cmake)
include(cmake/smp.cmake)
include(cmake/smp-runtime.cmake)
include(cmake/a733-smp.cmake)
include(cmake/a733-smp-cached.cmake)
set(A733_NPU_HANDOFF_SOURCE "" CACHE FILEPATH "Explicit native NPU application handoff source")
option(A733_NPU_SUPERVISED "Build retained supervised native NPU application" OFF)
if(A733_NPU_SUPERVISED)
  include(cmake/a733-npu-supervised.cmake)
elseif(A733_NPU_HANDOFF_SOURCE)
  # Keep application source properties local and consume the cached boot inputs
  # before later profiles replace their directory variables.
  add_subdirectory(apps/npu-convolution native-npu EXCLUDE_FROM_ALL)
endif()
include(cmake/a733-smp-cached-devices.cmake)
include(cmake/a733-storage-maintenance.cmake)
include(cmake/a733-camera-application.cmake)
include(cmake/a733-smp-observe.cmake)
include(cmake/mimus-smp.cmake)
include(cmake/a733-mimus-native.cmake)
include(cmake/dma.cmake)
include(cmake/npu.cmake)
include(cmake/npu-startup.cmake)
include(cmake/npu-clock-handoff.cmake)
include(cmake/npu-convolution.cmake)
include(cmake/radio-security.cmake)
include(cmake/radio-network.cmake)
include(cmake/radio-dispatch.cmake)
include(cmake/radio-session.cmake)
if(SUNSTONE_TCP_DIR AND SUNSTONE_RSN_DIR AND SUNSTONE_NETWORK_C_HEADERS)
  include(cmake/a733-board.cmake)
endif()
include(cmake/dma-service.cmake)
include(cmake/dma-startup.cmake)
include(cmake/i2s-stream.cmake)
include(cmake/gpu.cmake)
include(cmake/gpu-freelist-service.cmake)
include(cmake/gpu-irq.cmake)
include(cmake/gpu-vm-memory.cmake)

include(cmake/a733-gpu-application.cmake)
include(cmake/a733-smp-supervised.cmake)
include(cmake/a733-hdmi-application.cmake)
set(SUNSTONE_I2S_RECORDED_WAV "" CACHE FILEPATH "Pinned existing recorded PCM fixture")
if(SUNSTONE_I2S_RECORDED_WAV)
  include(cmake/a733-i2s-application.cmake)
endif()
if(SUNSTONE_JPEG_FIXTURE)
  include(cmake/a733-jpeg.cmake)
endif()
option(SUNSTONE_BLUETOOTH_APPLICATION "Build the supervised Bluetooth application" OFF)
if(SUNSTONE_BLUETOOTH_APPLICATION)
  include(cmake/a733-bluetooth.cmake)
endif()

option(SUNSTONE_A733_CRYPTO "Build supervised native CE SHA/AES diagnostic" OFF)
if(SUNSTONE_A733_CRYPTO)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-crypto.cmake")
endif()
