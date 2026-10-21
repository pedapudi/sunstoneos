# The SMP port is kept in an emulator-only target until A733 recovery and
# secondary-core power sequencing are qualified.
set(smp_port "${FREERTOS_DIR}/portable/GCC/ARM_CR82")
set(smp_build "${CMAKE_CURRENT_BINARY_DIR}/freertos-smp")
file(MAKE_DIRECTORY "${smp_build}")
apply_build_patch("${smp_build}/port.c" "${smp_port}/port.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-c.patch")
apply_build_patch("${smp_build}/portASM.S" "${smp_port}/portASM.S"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-asm.patch")
apply_build_patch("${smp_build}/portmacro.h" "${smp_port}/portmacro.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-macro.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${smp_port}/port.c" "${smp_port}/portASM.S" "${smp_port}/portmacro.h"
  patches/freertos-smp-c.patch patches/freertos-smp-asm.patch patches/freertos-smp-macro.patch)
foreach(target IN ITEMS smp-kernel-model smp-context-model)
  if(target STREQUAL "smp-context-model")
    set(smp_check checks/smp-context.c checks/smp-context.S)
  else()
    set(smp_check checks/smp-kernel.c checks/fp-context.S)
  endif()
  add_executable(${target} EXCLUDE_FROM_ALL
    platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c
    ${smp_check}
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${smp_build}/port.c" "${smp_build}/portASM.S")
  target_include_directories(${target} PRIVATE platform/qemu-smp include runtime/include
    "${FREERTOS_DIR}/include" "${smp_build}")
  target_compile_options(${target} PRIVATE
    -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
    -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
    -O2 -g -Wall -Wextra)
  target_link_options(${target} PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
  set_target_properties(${target} PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
endforeach()

# Eight-core emulator soak over the same port and platform sources.
add_executable(smp8-soak-model EXCLUDE_FROM_ALL
  platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c
  checks/smp8-soak.c checks/fp-context.S
  "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${smp_build}/port.c" "${smp_build}/portASM.S")
target_include_directories(smp8-soak-model PRIVATE platform/qemu-smp include runtime/include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(smp8-soak-model PRIVATE SUNSTONE_SMP_CORES=8)
target_compile_options(smp8-soak-model PRIVATE
  -march=armv8-a -mstrict-align -ffreestanding -fno-builtin -fno-stack-protector
  -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections
  -O2 -g -Wall -Wextra)
target_link_options(smp8-soak-model PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--defsym=SUNSTONE_SMP_STACKS=8
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/smp8-soak-model.map")
set_target_properties(smp8-soak-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
