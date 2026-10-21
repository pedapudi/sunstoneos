cmake_minimum_required(VERSION 3.24)
project(sunstoneos LANGUAGES C ASM)
# Documented code generation and the board images' compiler identities assume
# the Clang 21 driver selected by cmake/aarch64-clang.cmake.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR CMAKE_C_COMPILER_VERSION VERSION_LESS 21)
  message(FATAL_ERROR "Clang 21 or later is required; found ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
endif()
option(SUNSTONE_A733_PWM "Supervised native PWM0 fan application" OFF)
option(SUNSTONE_A733_STORAGE_READ "Supervised read-only SD0 application" OFF)
# These profiles derive their images from the supervised board target, which
# leaves the watchdog disabled. That mode must be requested explicitly, and
# before the board target is created; a profile that switched it on itself
# would also leave it on in the cache after the profile was disabled.
foreach(profile SUNSTONE_A733_PWM SUNSTONE_A733_STORAGE_READ SUNSTONE_A733_CRYPTO
    SUNSTONE_A733_THERMAL SUNSTONE_A733_RTC SUNSTONE_A733_PCIE SUNSTONE_A733_PCIE_TEST
    SUNSTONE_A733_SPI SUNSTONE_A733_ADC SUNSTONE_A733_MONITOR SUNSTONE_A733_DEINTERLACE
    SUNSTONE_JPEG_ENCODE_APPLICATION SUNSTONE_JPEG_FIXTURE SUNSTONE_BLUETOOTH_APPLICATION
    SUNSTONE_MIMUS_BOARD_APPLICATION)
  if(${profile} AND NOT SUNSTONE_BOARD_SUPERVISED)
    message(FATAL_ERROR "${profile} builds on the supervised board; configure with -DSUNSTONE_BOARD_SUPERVISED=ON")
  endif()
endforeach()

set(FREERTOS_DIR "" CACHE PATH "Pinned FreeRTOS kernel source directory")
if(NOT EXISTS "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE/port.c")
  message(FATAL_ERROR "Set FREERTOS_DIR to the kernel fetched by tools/fetch-sources.py")
endif()
# Storage-derived targets are configured unconditionally and patch TinyUSB.
set(TINYUSB_DIR "" CACHE PATH "Pinned TinyUSB source directory")
if(NOT EXISTS "${TINYUSB_DIR}/src/tusb.c")
  message(FATAL_ERROR "Set TINYUSB_DIR to the TinyUSB source fetched by tools/fetch-sources.py; "
    "the central build needs it even for kernel-only targets")
endif()
# Patches still apply to a different checkout with fuzz, so check the files the
# build compiles against the lock, and recheck when one of them changes.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(pinned "freertos|${FREERTOS_DIR}" "tinyusb|${TINYUSB_DIR}")
  string(REPLACE "|" ";" parts "${pinned}")
  list(GET parts 0 name)
  list(GET parts 1 directory)
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/fetch-sources.py"
    "${name}" --verify "${directory}" RESULT_VARIABLE verify_status OUTPUT_VARIABLE verified_files)
  if(NOT verify_status EQUAL 0)
    message(FATAL_ERROR "${directory} does not match the ${name} files pinned in sources.lock.json")
  endif()
  string(REPLACE "\n" ";" verified_files "${verified_files}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${verified_files})
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS sources.lock.json tools/fetch-sources.py)
# Map checkout, build and fetched-source paths to fixed names in every target.
# An interface library keeps the maps out of the COMPILE_OPTIONS that image
# identities hash, because those maps contain the very paths being removed.
sunstone_path_maps(path_maps)
add_library(sunstone-path-maps INTERFACE)
foreach(map IN LISTS path_maps)
  target_compile_options(sunstone-path-maps INTERFACE "-ffile-prefix-map=${map}")
endforeach()
link_libraries(sunstone-path-maps)

set(PORT "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
include(cmake/patch.cmake)
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
# The standalone NPU project defines its own apply_build_patch, which would
# replace the exact one for every later profile.
include(cmake/patch.cmake)
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
option(SUNSTONE_JPEG_ENCODE_APPLICATION "Build supervised native VGA JPEG encoder" OFF)
if(SUNSTONE_JPEG_ENCODE_APPLICATION)
  include(cmake/a733-jpeg-encode.cmake)
endif()
if(SUNSTONE_JPEG_FIXTURE)
  include(cmake/a733-jpeg.cmake)
endif()
option(SUNSTONE_BLUETOOTH_APPLICATION "Build the supervised Bluetooth application" OFF)
if(SUNSTONE_BLUETOOTH_APPLICATION)
  include(cmake/a733-bluetooth.cmake)
endif()

option(SUNSTONE_MIMUS_BOARD_APPLICATION "Build supervised recorded recognition with USB storage" OFF)
if(SUNSTONE_MIMUS_BOARD_APPLICATION)
  include(cmake/a733-mimus-board.cmake)
endif()

option(SUNSTONE_A733_CRYPTO "Build supervised native CE SHA/AES diagnostic" OFF)
if(SUNSTONE_A733_CRYPTO)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-crypto.cmake")
endif()

option(SUNSTONE_A733_THERMAL "Build supervised native thermal monitoring" OFF)
if(SUNSTONE_A733_THERMAL)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-thermal.cmake")
endif()

option(SUNSTONE_A733_RTC "Build supervised native RTC read/alarm diagnostic" OFF)
if(SUNSTONE_A733_RTC)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-rtc.cmake")
endif()

option(SUNSTONE_A733_PCIE "Build supervised native PCIe direct-endpoint observation" OFF)
if(SUNSTONE_A733_PCIE)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-pcie.cmake")
endif()

option(SUNSTONE_A733_PCIE_TEST "Build supervised PCIe endpoint DMA copy with MSI completion" OFF)
if(SUNSTONE_A733_PCIE_TEST)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-pcie-test.cmake")
endif()

if(SUNSTONE_A733_STORAGE_READ)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-storage-read.cmake")
endif()

option(SUNSTONE_A733_SPI "Build the supervised native SPI application" OFF)
if(SUNSTONE_A733_SPI)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-spi.cmake")
endif()

if(SUNSTONE_A733_PWM)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-pwm.cmake")
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-pwm-single-core.cmake")
endif()

option(SUNSTONE_A733_ADC "Build the supervised native GPADC board monitor" OFF)
if(SUNSTONE_A733_ADC)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-adc.cmake")
endif()

option(SUNSTONE_A733_MONITOR "Build supervised repeated thermal/GPADC monitoring" OFF)
if(SUNSTONE_A733_MONITOR)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-monitor.cmake")
endif()

option(SUNSTONE_A733_DEINTERLACE "Build supervised native DI301 offline weave" OFF)
if(SUNSTONE_A733_DEINTERLACE)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-deinterlace.cmake")
endif()

option(SUNSTONE_A733_STAGE2 "Build the cold-entry monitor with the stage-2 slot loader" OFF)
if(SUNSTONE_A733_STAGE2)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-standalone-boot.cmake")
endif()

include(cmake/a733-os.cmake)
