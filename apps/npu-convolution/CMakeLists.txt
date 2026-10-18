cmake_minimum_required(VERSION 3.24)
project(a733_npu_convolution LANGUAGES C ASM)
get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(FREERTOS_DIR "" CACHE PATH "Pinned FreeRTOS kernel source")
set(TINYUSB_DIR "" CACHE PATH "Pinned TinyUSB source")
set(A733_NPU_HANDOFF_SOURCE "" CACHE FILEPATH "Immutable board-qualified NPU/PMIC/pin/USB handoff source")
if(NOT EXISTS "${A733_NPU_HANDOFF_SOURCE}" OR IS_DIRECTORY "${A733_NPU_HANDOFF_SOURCE}")
  message(FATAL_ERROR "Supply A733_NPU_HANDOFF_SOURCE defining a733_npu_application_boot; no default admission is provided")
endif()
get_filename_component(A733_NPU_HANDOFF_SOURCE "${A733_NPU_HANDOFF_SOURCE}" ABSOLUTE)
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
if(NOT TARGET a733-smp-cached-diagnostic)
  include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-smp-cached.cmake")
endif()
get_target_property(npu_sources a733-smp-cached-diagnostic SOURCES)
list(REMOVE_ITEM npu_sources
  "${a733_root}/platform/a733-smp-cached/diagnostic.c"
  "${a733_root}/platform/a733-smp-cached/continue.S"
  "${a733_root}/platform/a733-smp-cached/boot.c"
  "${a733_build}/port.c" "${a733_build}/build-id.c")
set(npu_build "${CMAKE_CURRENT_BINARY_DIR}/npu-convolution")
file(MAKE_DIRECTORY "${npu_build}")
apply_build_patch("${npu_build}/port.c" "${a733_build}/port.c"
  "${a733_root}/patches/freertos-smp-privileged.patch")
apply_build_patch("${npu_build}/continue.S" "${a733_root}/platform/a733-smp-cached/continue.S"
  "${a733_root}/apps/npu-convolution/continue.patch")
apply_build_patch("${npu_build}/boot.c" "${a733_root}/platform/a733-smp-cached/boot.c"
  "${a733_root}/apps/dma-copy/boot.patch")
apply_build_patch("${npu_build}/usb_service.c" "${a733_root}/platform/a733-smp-observe/usb_service.c"
  "${a733_root}/apps/npu-convolution/usb-service.patch")
file(MAKE_DIRECTORY "${npu_build}/tinyusb")
foreach(pair "dcd_sunxi_musb.c|tinyusb-a733-dcd.patch" "musb_def.h|tinyusb-a733-registers.patch")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 src)
  list(GET parts 1 patch_name)
  apply_build_patch("${npu_build}/tinyusb/${src}" "${TINYUSB_DIR}/src/portable/sunxi/${src}"
    "${a733_root}/patches/${patch_name}")
endforeach()
set_source_files_properties("${npu_build}/port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=a733_npu_application_task_privileged")
set(npu_units
  apps/npu-convolution/main.c apps/npu-convolution/native.c apps/npu-convolution/result.c
  platform/a733-smp-observe/usb.c platform/a733-smp-observe/result.c platform/a733-smp-observe/service.S
  platform/a733/memory_reservations.c runtime/device_memory_pool.c platform/a733/npu_reserved.c
  arch/aarch64/device_memory.c platform/a733/npu_native.c runtime/counter_clock.c
  platform/a733/counter_clock.c platform/a733/shared_locks.c
  drivers/power/a733_pmic_native.c drivers/pinctrl/a733_pins_native.c
  drivers/npu/npu_convolution.c drivers/npu/a733_npu_startup.c drivers/npu/a733_npu_pll.c
  drivers/npu/a733_npu_platform.c drivers/npu/a733_npu_platform_native.c drivers/i2c/a733_i2c.c
  drivers/npu/npu_service.c drivers/npu/a733_npu_controller.c
  drivers/npu/a733_npu_memory.c drivers/npu/a733_npu_mmu.c drivers/npu/nbg_model.c
  drivers/npu/nbg_assembly.c drivers/npu/nbg_checksum.c drivers/npu/nbg_relocation.c
  drivers/power/a733_pmic_consumer.c drivers/power/a733_pmic.c drivers/power/a733_rail_leases.c
  drivers/pinctrl/a733_pins.c drivers/pinctrl/a733_pin_tables.c
  drivers/gpu/a733_gpu_resources.c drivers/gpu/a733_gpu_resources_native.c
  drivers/clock/a733_clock_leases.c)
set_source_files_properties("${a733_root}/drivers/npu/npu_convolution.c" PROPERTIES
  COMPILE_DEFINITIONS "a733_npu_native_operations=a733_npu_application_operations")
set_source_files_properties("${a733_root}/drivers/gpu/a733_gpu_resources_native.c"
  "${a733_root}/drivers/i2c/a733_i2c.c" PROPERTIES
  COMPILE_OPTIONS "-iquote;${a733_root}/platform/a733")
list(TRANSFORM npu_units PREPEND "${a733_root}/")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
foreach(input MODEL INPUT REFERENCE)
  set(A733_NPU_${input} "" CACHE FILEPATH "Pinned fixed convolution ${input} bytes")
  if(NOT EXISTS "${A733_NPU_${input}}")
    message(FATAL_ERROR "Supply the pinned A733_NPU_${input} artifact")
  endif()
endforeach()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${a733_root}/apps/npu-convolution/artifacts.py"
  --model "${A733_NPU_MODEL}" --input "${A733_NPU_INPUT}" --reference "${A733_NPU_REFERENCE}"
  --output "${npu_build}/artifacts.c" RESULT_VARIABLE artifact_status)
if(NOT artifact_status EQUAL 0)
  message(FATAL_ERROR "Fixed NPU artifacts failed identity admission")
endif()
list(APPEND npu_sources "${npu_build}/artifacts.c")
list(APPEND npu_sources ${npu_units} "${A733_NPU_HANDOFF_SOURCE}"
  "${npu_build}/port.c" "${npu_build}/continue.S" "${npu_build}/boot.c" "${npu_build}/usb_service.c"
  "${TINYUSB_DIR}/src/tusb.c" "${TINYUSB_DIR}/src/common/tusb_fifo.c"
  "${TINYUSB_DIR}/src/device/usbd.c" "${TINYUSB_DIR}/src/class/vendor/vendor_device.c"
  "${npu_build}/tinyusb/dcd_sunxi_musb.c")
set(npu_inputs ${a733_identity_inputs} ${npu_sources}
  "${a733_root}/apps/npu-convolution/artifacts.py" "${A733_NPU_MODEL}" "${A733_NPU_INPUT}" "${A733_NPU_REFERENCE}"
  "${CMAKE_CURRENT_LIST_FILE}" "${a733_root}/apps/npu-convolution/continue.patch"
  "${a733_root}/patches/freertos-smp-privileged.patch")
file(GLOB_RECURSE npu_usb_headers "${TINYUSB_DIR}/src/*.h")
list(APPEND npu_inputs ${npu_usb_headers}
  "${a733_root}/apps/dma-copy/boot.patch" "${a733_root}/apps/npu-convolution/usb-service.patch"
  "${a733_root}/platform/a733-smp-observe/usb_service.c"
  "${a733_root}/platform/a733-smp-observe/result.h" "${a733_root}/platform/a733-smp-observe/a733_usb.h"
  "${a733_root}/platform/a733-smp-observe/tusb_config.h" "${npu_build}/tinyusb/musb_def.h"
  "${TINYUSB_DIR}/src/portable/sunxi/dcd_sunxi_musb.c" "${TINYUSB_DIR}/src/portable/sunxi/musb_def.h"
  "${a733_root}/patches/tinyusb-a733-dcd.patch" "${a733_root}/patches/tinyusb-a733-registers.patch")
file(GLOB npu_headers "${a733_root}/include/*.h" "${a733_root}/runtime/include/*.h")
list(APPEND npu_inputs ${npu_headers})
list(APPEND npu_inputs
  "${a733_port}/port.c" "${a733_port}/portASM.S" "${a733_port}/portmacro.h"
  "${a733_root}/patches/freertos-smp-c.patch" "${a733_root}/patches/freertos-smp-asm.patch"
  "${a733_root}/patches/freertos-smp-macro.patch" "${a733_root}/patches/freertos-a733-smp.patch")
list(REMOVE_DUPLICATES npu_inputs)
string(REPLACE ";" "\n" npu_input_lines "${npu_inputs}")
file(WRITE "${npu_build}/identity-inputs.txt" "${npu_input_lines}\n")
# The final ELF hash is recorded separately; this binds the boot contract to the
# selected application, admission source, mappings and private kernel inputs.
set(npu_identity "npu-convolution;${a733_identity};pool=2097152;privileged-bootstrap-worker")
foreach(input IN LISTS npu_inputs)
  file(SHA256 "${input}" digest)
  string(APPEND npu_identity "${digest}")
endforeach()
string(SHA256 npu_build_id "${npu_identity}")
string(REGEX REPLACE "(..)" "0x\\1," npu_id_bytes "${npu_build_id}")
file(WRITE "${npu_build}/build-id.c.tmp"
  "const unsigned char a733_smp_build_id[32]={${npu_id_bytes}};\n")
configure_file("${npu_build}/build-id.c.tmp" "${npu_build}/build-id.c" COPYONLY)
file(WRITE "${npu_build}/build-id.txt" "${npu_build_id}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${npu_inputs})
add_executable(a733-npu-convolution ${npu_sources} "${npu_build}/build-id.c")
foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS)
  get_target_property(value a733-smp-cached-diagnostic ${property})
  set_property(TARGET a733-npu-convolution PROPERTY ${property} "${value}")
endforeach()
target_include_directories(a733-npu-convolution PRIVATE "${a733_root}/platform/a733-smp-observe"
  "${a733_root}/drivers/usb" "${TINYUSB_DIR}/src")
target_compile_definitions(a733-npu-convolution PRIVATE SUNSTONE_SECURE_MONITOR=1 SUNSTONE_A733_SMP_DIAGNOSTIC=1
  SUNSTONE_A733_USB=1 A733_SMP_CONTRACT_MAGIC=0x415337334f424331ULL A733_SMP_RESULT_MAGIC=0x415337334e505231ULL
  INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1
  INCLUDE_xSemaphoreGetMutexHolder=1)
target_link_options(a733-npu-convolution PRIVATE "-Wl,--defsym=__device_pool_size=2097152"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/a733-npu-convolution.map")
set_target_properties(a733-npu-convolution PROPERTIES SUFFIX ".elf")
