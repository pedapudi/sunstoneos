# Dedicated compact cached image for the immutable storage maintenance session.
if(NOT DEFINED SUNSTONE_SOURCE_ROOT)
  set(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
if(NOT TARGET a733-smp-cached-diagnostic)
  include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-smp-cached.cmake")
endif()
set(storage_root "${SUNSTONE_SOURCE_ROOT}")
set(storage_build "${CMAKE_CURRENT_BINARY_DIR}/storage-maintenance")
set(storage_base "${CMAKE_CURRENT_BINARY_DIR}/freertos-a733-smp-cached")
set(storage_target sunstone-a733-storage-maintenance)
file(MAKE_DIRECTORY "${storage_build}" "${storage_build}/tinyusb")
get_target_property(storage_sources a733-smp-cached-diagnostic SOURCES)
foreach(name boot.c admission.c platform.c diagnostic.c continue.S)
  list(REMOVE_ITEM storage_sources "${storage_root}/platform/a733-smp-cached/${name}")
endforeach()
list(REMOVE_ITEM storage_sources "${storage_base}/port.c" "${storage_base}/build-id.c"
  "${storage_root}/checks/fp-context.S")
file(SHA256 "${storage_root}/platform/a733-smp-cached/boot.c" storage_source_hash)
if(NOT storage_source_hash STREQUAL "2e7401667a2d320a268734f489722aedbb620bf8b2d96a8c791d2635136dcbe0")
  message(FATAL_ERROR "Review the storage boot patch against changed cached source")
endif()
apply_build_patch("${storage_build}/boot.c" "${storage_root}/platform/a733-smp-cached/boot.c"
  "${storage_root}/platform/a733-storage-maintenance/boot.patch")
file(SHA256 "${storage_root}/platform/a733-smp-cached/platform.c" storage_source_hash)
if(NOT storage_source_hash STREQUAL "6bf8b9e73c04cd3254d5e7ce4e225263d67f3e97e6727df3ff8aad7d58f48b2e")
  message(FATAL_ERROR "Review the storage platform patch against changed cached source")
endif()
apply_build_patch("${storage_build}/platform.c" "${storage_root}/platform/a733-smp-cached/platform.c"
  "${storage_root}/platform/a733-storage-maintenance/platform.patch")
file(SHA256 "${storage_root}/platform/a733-smp-cached/continue.S" storage_source_hash)
if(NOT storage_source_hash STREQUAL "4a079fa75a44b37ffdea90ecd06752634707a513ec9a14e8220319b6de5c3b1a")
  message(FATAL_ERROR "Review the storage continue patch against changed cached source")
endif()
apply_build_patch("${storage_build}/continue.S" "${storage_root}/platform/a733-smp-cached/continue.S"
  "${storage_root}/platform/a733-storage-maintenance/continue.patch")
file(SHA256 "${storage_root}/platform/a733-smp-cached/link.ld" storage_source_hash)
if(NOT storage_source_hash STREQUAL "dacd51ebf781273c67bbd80eeebe6eff72d133f1c38ffb62a9e19b204179cd65")
  message(FATAL_ERROR "Review the storage link patch against changed cached source")
endif()
apply_build_patch("${storage_build}/link.ld" "${storage_root}/platform/a733-smp-cached/link.ld"
  "${storage_root}/platform/a733-storage-maintenance/link.patch")
apply_build_patch("${storage_build}/port.c" "${storage_base}/port.c"
  "${storage_root}/patches/freertos-smp-privileged.patch")
set_source_files_properties("${storage_root}/drivers/gpu/a733_gpu_resources_native.c"
  "${storage_root}/drivers/i2c/a733_i2c.c" PROPERTIES
  COMPILE_OPTIONS "-iquote${storage_root}/platform/a733")
set_source_files_properties("${storage_build}/port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=a733_storage_native_task_privileged")
foreach(pair "dcd_sunxi_musb.c|tinyusb-a733-dcd.patch" "musb_def.h|tinyusb-a733-registers.patch")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 src)
  list(GET parts 1 patch_name)
  apply_build_patch("${storage_build}/tinyusb/${src}" "${TINYUSB_DIR}/src/portable/sunxi/${src}"
    "${storage_root}/patches/${patch_name}")
endforeach()
set(storage_extra
  platform/a733-storage-maintenance/main.c platform/a733-storage-maintenance/admission.c
  platform/a733-storage-maintenance/session.c
  platform/a733-storage-update/bootstrap.c platform/a733-storage-update/worker.c platform/a733-storage-update/usb.c
  drivers/storage/a733_storage_update.c drivers/storage/storage_digest.c drivers/storage/storage_update_usb.c
  drivers/storage/a733_mmc_platform.c drivers/storage/a733_mmc_platform_native.c drivers/storage/sunxi_mmc.c
  drivers/clock/a733_clock_leases.c drivers/power/a733_rail_leases.c
  drivers/power/a733_pmic.c drivers/power/a733_pmic_native.c drivers/power/a733_pmic_consumer.c drivers/i2c/a733_i2c.c
  drivers/pinctrl/a733_pins.c drivers/pinctrl/a733_pin_tables.c drivers/pinctrl/a733_pins_native.c
  drivers/gpu/a733_gpu_resources.c drivers/gpu/a733_gpu_resources_native.c
  drivers/usb/a733_usb_platform.c drivers/usb/a733_usb_platform_native.c
  platform/a733/counter_clock.c platform/a733/shared_locks.c platform/a733/native_usb.c
  drivers/watchdog/a733_maintenance_watchdog.c platform/a733/maintenance_watchdog.c
  arch/aarch64/device_memory.c)
list(TRANSFORM storage_extra PREPEND "${storage_root}/")
list(APPEND storage_sources ${storage_extra} "${storage_build}/boot.c" "${storage_build}/platform.c"
  "${storage_build}/continue.S" "${storage_build}/port.c" "${storage_build}/tinyusb/dcd_sunxi_musb.c"
  "${TINYUSB_DIR}/src/tusb.c" "${TINYUSB_DIR}/src/common/tusb_fifo.c"
  "${TINYUSB_DIR}/src/device/usbd.c" "${TINYUSB_DIR}/src/class/vendor/vendor_device.c")
file(GLOB storage_headers "${storage_root}/include/*.h" "${storage_root}/runtime/include/*.h")
file(GLOB_RECURSE storage_usb_headers "${TINYUSB_DIR}/src/*.h")
set(storage_inputs ${storage_sources} ${storage_headers} ${storage_usb_headers}
  "${CMAKE_CURRENT_LIST_FILE}" "${storage_build}/link.ld"
  "${storage_root}/platform/a733-storage-maintenance/session.h"
  "${storage_root}/platform/a733-storage-maintenance/FreeRTOSConfig.h"
  "${storage_root}/platform/a733-storage-maintenance/CMakeLists.txt"
  "${storage_root}/platform/a733-storage-update/tusb_config.h"
  "${storage_root}/platform/a733-smp-cached/boot.h" "${storage_root}/platform/a733-smp-cached/cached.h"
  "${storage_root}/platform/a733/boot.h"
  "${storage_root}/platform/a733-smp-cached/FreeRTOSConfig.h"
  "${storage_base}/portmacro.h" "${storage_build}/tinyusb/musb_def.h")
file(GLOB storage_kernel_headers "${FREERTOS_DIR}/include/*.h")
list(APPEND storage_inputs ${storage_kernel_headers})
list(REMOVE_DUPLICATES storage_inputs)
set(storage_identity "storage-maintenance-1;${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};armv8-a;O2;WBWA;8")
foreach(input IN LISTS storage_inputs)
  file(SHA256 "${input}" storage_digest)
  string(APPEND storage_identity "${storage_digest}")
endforeach()
string(SHA256 storage_build_id "${storage_identity}")
string(REGEX REPLACE "(..)" "0x\\1," storage_id_bytes "${storage_build_id}")
file(WRITE "${storage_build}/build-id.c.tmp" "const unsigned char a733_smp_build_id[32]={${storage_id_bytes}};\n")
configure_file("${storage_build}/build-id.c.tmp" "${storage_build}/build-id.c" COPYONLY)
file(WRITE "${storage_build}/build-id.txt" "${storage_build_id}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${storage_inputs})
add_executable(${storage_target} EXCLUDE_FROM_ALL ${storage_sources} "${storage_build}/build-id.c")
get_target_property(storage_compile_options a733-smp-cached-diagnostic COMPILE_OPTIONS)
set_property(TARGET ${storage_target} PROPERTY COMPILE_OPTIONS "${storage_compile_options}")
target_include_directories(${storage_target} PRIVATE
  "${storage_root}/platform/a733-storage-maintenance" "${storage_root}/platform/a733-storage-update"
  "${storage_root}/platform/a733-smp-cached" "${storage_root}/drivers/usb" "${TINYUSB_DIR}/src"
  "${storage_root}/include" "${storage_root}/runtime/include" "${FREERTOS_DIR}/include" "${storage_base}")
target_compile_definitions(${storage_target} PRIVATE SUNSTONE_SECURE_MONITOR=1 SUNSTONE_A733_SMP_DIAGNOSTIC=1
  SUNSTONE_A733_USB=1 A733_SMP_CONTRACT_MAGIC=0x4153373353544331ULL A733_SMP_RESULT_MAGIC=0x4153373353545231ULL)
target_link_options(${storage_target} PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${storage_build}/image.map")
set_target_properties(${storage_target} PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
