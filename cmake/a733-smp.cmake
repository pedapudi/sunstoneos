# Standalone A733 diagnostic: no existing QEMU target or generated port changes.
if(NOT DEFINED SUNSTONE_SOURCE_ROOT)
  set(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
set(a733_root "${SUNSTONE_SOURCE_ROOT}")
set(a733_port "${FREERTOS_DIR}/portable/GCC/ARM_CR82")
set(a733_build "${CMAKE_CURRENT_BINARY_DIR}/freertos-a733-smp")
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
  platform/a733-smp/start.S platform/a733-smp/boot.c platform/a733-smp/admission.c
  platform/a733-smp/platform.c platform/a733-smp/diagnostic.c
  platform/a733/cpu_start.c platform/a733/cpu_entry.c platform/a733/cpu_entry.S
  platform/a733/cpu_entry_native.c platform/a733/cpu_continue.c platform/a733/cpu_continue.S
  runtime/memory.c runtime/memmove.c checks/fp-context.S)
list(TRANSFORM a733_sources PREPEND "${a733_root}/")
# This is a build-input identity, not the final ELF digest. The host records both.
set(a733_identity_inputs ${a733_sources}
  "${a733_root}/include/a733_cpu_start.h" "${a733_root}/include/a733_cpu_entry.h"
  "${a733_root}/include/a733_cpu_continue.h" "${a733_root}/include/FreeRTOSConfig.h"
  "${a733_root}/platform/a733-smp/FreeRTOSConfig.h" "${a733_root}/platform/a733-smp/boot.h"
  "${a733_root}/platform/a733-smp/link.ld" "${a733_root}/cmake/a733-smp.cmake"
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${a733_build}/port.c" "${a733_build}/portASM.S" "${a733_build}/portmacro.h")
file(GLOB a733_kernel_headers "${FREERTOS_DIR}/include/*.h")
list(APPEND a733_identity_inputs ${a733_kernel_headers}
  "${a733_root}/include/platform.h" "${a733_root}/runtime/include/string.h")
set(a733_identity "${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};armv8-a;O2;NC;8")
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
add_executable(a733-smp-diagnostic EXCLUDE_FROM_ALL ${a733_sources}
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${a733_build}/port.c" "${a733_build}/portASM.S" "${a733_build}/build-id.c")
target_include_directories(a733-smp-diagnostic PRIVATE "${a733_root}/platform/a733-smp"
  "${a733_root}/include" "${a733_root}/runtime/include" "${FREERTOS_DIR}/include" "${a733_build}")
target_compile_definitions(a733-smp-diagnostic PRIVATE SUNSTONE_A733_SMP_DIAGNOSTIC=1 SUNSTONE_SECURE_MONITOR=1)
target_compile_options(a733-smp-diagnostic PRIVATE -march=armv8-a -mstrict-align
  $<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>
  -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
  -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra)
set_source_files_properties(
  "${a733_root}/platform/a733-smp/boot.c" "${a733_root}/platform/a733-smp/platform.c"
  "${a733_root}/platform/a733-smp/admission.c" "${a733_root}/platform/a733/cpu_continue.c"
  "${a733_root}/platform/a733/cpu_entry.c" "${a733_root}/platform/a733/cpu_entry_native.c"
  "${a733_root}/platform/a733/cpu_start.c" PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
target_link_options(a733-smp-diagnostic PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${a733_root}/platform/a733-smp/link.ld" "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/a733-smp-diagnostic.map")
set_target_properties(a733-smp-diagnostic PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${a733_root}/platform/a733-smp/link.ld")
