# Cached diagnostic with a separately admitted USB result observer.
if(NOT DEFINED SUNSTONE_SOURCE_ROOT)
  set(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
set(a733_root "${SUNSTONE_SOURCE_ROOT}")
set(a733_port "${FREERTOS_DIR}/portable/GCC/ARM_CR82")
set(a733_build "${CMAKE_CURRENT_BINARY_DIR}/freertos-a733-smp-observe")
file(MAKE_DIRECTORY "${a733_build}")
apply_build_patch("${a733_build}/port-base.c" "${a733_port}/port.c"
  "${a733_root}/patches/freertos-smp-c.patch")
apply_build_patch("${a733_build}/port.c" "${a733_build}/port-base.c"
  "${a733_root}/patches/freertos-a733-smp.patch")
apply_build_patch("${a733_build}/portASM.S" "${a733_port}/portASM.S"
  "${a733_root}/patches/freertos-smp-asm.patch")
apply_build_patch("${a733_build}/portmacro.h" "${a733_port}/portmacro.h"
  "${a733_root}/patches/freertos-smp-macro.patch")
set(a733_sources
  platform/a733-smp-cached/start.S platform/a733-smp-cached/admission.c
  platform/a733-smp-cached/platform.c platform/a733-smp-observe/diagnostic.c
  platform/a733/cpu_start.c platform/a733/cpu_entry.c platform/a733/cpu_entry.S
  platform/a733/cpu_entry_native.c platform/a733/cpu_coherence.c platform/a733/cpu_coherence.S
  platform/a733-smp-cached/memory.c
  platform/a733-smp-cached/continue.c
  platform/a733-smp-observe/usb.c platform/a733-smp-observe/usb_service.c
  platform/a733-smp-observe/result.c platform/a733-smp-observe/service.S
  runtime/memory.c runtime/memmove.c checks/fp-context.S)
list(TRANSFORM a733_sources PREPEND "${a733_root}/")
apply_build_patch("${a733_build}/continue-core.c" "${a733_root}/platform/a733/cpu_continue.c"
  "${a733_root}/platform/a733-smp-cached/continuation-core.patch")
list(APPEND a733_sources "${a733_build}/continue-core.c")
# Private patches preserve the original cached image and its evidence.
file(SHA256 "${a733_root}/platform/a733-smp-cached/boot.c" observe_boot_hash)
file(SHA256 "${a733_root}/platform/a733-smp-cached/continue.S" observe_vector_hash)
if(NOT observe_boot_hash STREQUAL "2e7401667a2d320a268734f489722aedbb620bf8b2d96a8c791d2635136dcbe0" OR
   NOT observe_vector_hash STREQUAL "224e653514899239aa574f0ed153d823a2e256faaea6e0d9fbc18695b56bb75b")
  message(FATAL_ERROR "Observer hooks require review against changed cached boot/vector inputs")
endif()
foreach(pair "boot.c|boot.patch" "continue.S|continue.patch")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 src)
  list(GET parts 1 patch_name)
  apply_build_patch("${a733_build}/${src}" "${a733_root}/platform/a733-smp-cached/${src}"
    "${a733_root}/platform/a733-smp-observe/${patch_name}")
  list(APPEND a733_sources "${a733_build}/${src}")
endforeach()
file(MAKE_DIRECTORY "${a733_build}/tinyusb")
foreach(pair "dcd_sunxi_musb.c|tinyusb-a733-dcd.patch" "musb_def.h|tinyusb-a733-registers.patch")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 src)
  list(GET parts 1 patch_name)
  apply_build_patch("${a733_build}/tinyusb/${src}" "${TINYUSB_DIR}/src/portable/sunxi/${src}"
    "${a733_root}/patches/${patch_name}")
endforeach()
list(APPEND a733_sources "${TINYUSB_DIR}/src/tusb.c" "${TINYUSB_DIR}/src/common/tusb_fifo.c"
  "${TINYUSB_DIR}/src/device/usbd.c" "${TINYUSB_DIR}/src/class/vendor/vendor_device.c"
  "${a733_build}/tinyusb/dcd_sunxi_musb.c")
# This is a build-input identity, not the final ELF digest. The host records both.
set(a733_identity_inputs ${a733_sources}
  "${a733_root}/platform/a733/cpu_continue.c"
  "${a733_root}/platform/a733-smp-cached/continuation-core.patch"
  "${a733_root}/platform/a733-smp-observe/CMakeLists.txt"
  "${a733_root}/include/a733_cpu_start.h" "${a733_root}/include/a733_cpu_entry.h"
  "${a733_root}/include/a733_cpu_continue.h" "${a733_root}/include/a733_cpu_coherence.h"
  "${a733_root}/include/a733_cached_memory.h"
  "${a733_root}/platform/a733-smp-cached/cached.h" "${a733_root}/include/FreeRTOSConfig.h"
  "${a733_root}/platform/a733-smp-cached/FreeRTOSConfig.h" "${a733_root}/platform/a733-smp-cached/boot.h"
  "${a733_root}/platform/a733-smp-cached/link.ld" "${a733_root}/cmake/a733-smp-observe.cmake"
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${a733_build}/port.c" "${a733_build}/portASM.S" "${a733_build}/portmacro.h")
file(GLOB a733_kernel_headers "${FREERTOS_DIR}/include/*.h")
list(APPEND a733_identity_inputs ${a733_kernel_headers}
  "${a733_root}/include/platform.h" "${a733_root}/runtime/include/string.h")
file(GLOB_RECURSE observer_usb_headers "${TINYUSB_DIR}/src/*.h")
list(APPEND a733_identity_inputs ${observer_usb_headers}
  "${a733_root}/platform/a733-smp-observe/result.h" "${a733_root}/platform/a733-smp-observe/tusb_config.h"
  "${a733_root}/platform/a733-smp-observe/boot.patch" "${a733_root}/platform/a733-smp-observe/continue.patch"
  "${a733_root}/platform/a733-smp-cached/boot.c" "${a733_root}/platform/a733-smp-cached/continue.S"
  "${a733_root}/platform/a733-smp-observe/a733_usb.h" "${a733_build}/tinyusb/musb_def.h"
  "${a733_root}/patches/tinyusb-a733-dcd.patch" "${a733_root}/patches/tinyusb-a733-registers.patch"
  "${TINYUSB_DIR}/src/portable/sunxi/dcd_sunxi_musb.c" "${TINYUSB_DIR}/src/portable/sunxi/musb_def.h")
set(a733_identity "observer-OB-C1-OB-R1;${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};armv8-a;O2;WBWA;Device-monitor;8")
foreach(input IN LISTS a733_identity_inputs)
  file(SHA256 "${input}" digest)
  string(APPEND a733_identity "${digest}")
endforeach()
string(SHA256 a733_build_id "${a733_identity}")
string(REGEX REPLACE "(..)" "0x\\1," a733_id_bytes "${a733_build_id}")
file(WRITE "${a733_build}/build-id.c.tmp"
  "const unsigned char a733_smp_build_id[32]={${a733_id_bytes}};\n")
configure_file("${a733_build}/build-id.c.tmp" "${a733_build}/build-id.c" COPYONLY)
file(WRITE "${a733_build}/build-id.txt" "${a733_build_id}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${a733_identity_inputs}
  "${a733_port}/port.c" "${a733_port}/portASM.S" "${a733_port}/portmacro.h"
  "${a733_root}/patches/freertos-smp-c.patch" "${a733_root}/patches/freertos-smp-asm.patch"
  "${a733_root}/patches/freertos-smp-macro.patch" "${a733_root}/patches/freertos-a733-smp.patch")
add_executable(a733-smp-observe EXCLUDE_FROM_ALL ${a733_sources}
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${a733_build}/port.c" "${a733_build}/portASM.S" "${a733_build}/build-id.c")
target_include_directories(a733-smp-observe PRIVATE "${a733_root}/platform/a733-smp-observe"
  "${a733_root}/drivers/usb" "${TINYUSB_DIR}/src" "${a733_root}/platform/a733-smp-cached"
  "${a733_root}/include" "${a733_root}/runtime/include" "${FREERTOS_DIR}/include" "${a733_build}")
target_compile_definitions(a733-smp-observe PRIVATE SUNSTONE_A733_SMP_DIAGNOSTIC=1 SUNSTONE_SECURE_MONITOR=1 SUNSTONE_A733_USB=1
  A733_SMP_CONTRACT_MAGIC=0x415337334f424331ULL A733_SMP_RESULT_MAGIC=0x415337334f425231ULL)
target_compile_options(a733-smp-observe PRIVATE -march=armv8-a -mstrict-align
  $<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>
  -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
  -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra)
set_source_files_properties(
  "${a733_root}/platform/a733-smp-cached/boot.c" "${a733_root}/platform/a733-smp-cached/platform.c"
  "${a733_root}/platform/a733-smp-cached/admission.c" "${a733_root}/platform/a733-smp-cached/continue.c"
  "${a733_root}/platform/a733/cpu_entry.c" "${a733_root}/platform/a733/cpu_entry_native.c"
  "${a733_root}/platform/a733/cpu_start.c" PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
target_link_options(a733-smp-observe PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${a733_root}/platform/a733-smp-cached/link.ld" "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/a733-smp-observe.map")
set_target_properties(a733-smp-observe PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${a733_root}/platform/a733-smp-cached/link.ld")
