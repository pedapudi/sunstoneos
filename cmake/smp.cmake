include("${CMAKE_CURRENT_LIST_DIR}/scheduler-partitioned.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/kernel-flags.cmake")
# The SMP port is kept in an emulator-only target until A733 recovery and
# secondary-core power sequencing are qualified.
set(smp_port "${FREERTOS_DIR}/portable/GCC/ARM_CR82")
set(smp_build "${CMAKE_CURRENT_BINARY_DIR}/freertos-smp")
file(MAKE_DIRECTORY "${smp_build}")
apply_build_patch("${smp_build}/port.c" "${smp_port}/port.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-c.patch")
apply_build_patch("${smp_build}/portASM-base.S" "${smp_port}/portASM.S"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-asm.patch")
apply_build_patch("${smp_build}/portASM.S" "${smp_build}/portASM-base.S"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-gpu-call.patch")
apply_build_patch("${smp_build}/portmacro.h" "${smp_port}/portmacro.h"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-macro.patch")
# Exercise the scheduler used by the cached native SMP image, including
# affinity-aware time slicing and publication of the selected task register.
apply_build_patch("${smp_build}/tasks.c" "${FREERTOS_DIR}/tasks.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-tasks.patch")
if(SUNSTONE_PARTITIONED_SCHEDULER)
  sunstone_scheduler_partitioned_prepare("${smp_build}" "${smp_build}/tasks.c")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${smp_port}/port.c" "${smp_port}/portASM.S" "${smp_port}/portmacro.h"
  "${FREERTOS_DIR}/tasks.c" patches/freertos-smp-tasks.patch
  patches/freertos-smp-c.patch patches/freertos-smp-asm.patch patches/freertos-smp-macro.patch
  patches/freertos-gpu-call.patch)
foreach(target IN ITEMS smp-kernel-model smp-context-model)
  if(target STREQUAL "smp-context-model")
    set(smp_check checks/smp-context.c checks/smp-context.S)
  else()
    set(smp_check checks/smp-kernel.c checks/fp-context.S)
  endif()
  add_executable(${target} EXCLUDE_FROM_ALL
    platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c
    ${smp_check}
    "${smp_build}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${smp_build}/port.c" "${smp_build}/portASM.S")
  target_include_directories(${target} PRIVATE platform/qemu-smp include runtime/include
    "${FREERTOS_DIR}/include" "${smp_build}")
  target_compile_options(${target} PRIVATE
    ${SUNSTONE_KERNEL_FLAGS})
  target_link_options(${target} PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
  set_target_properties(${target} PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
  if(SUNSTONE_PARTITIONED_SCHEDULER)
    sunstone_scheduler_partitioned_bind(${target} "${smp_build}")
  endif()
endforeach()

# Eight-core emulator soak over the same port and platform sources.
add_executable(smp8-soak-model EXCLUDE_FROM_ALL
  platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c
  checks/smp8-soak.c checks/fp-context.S
  "${smp_build}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${smp_build}/port.c" "${smp_build}/portASM.S")
target_include_directories(smp8-soak-model PRIVATE platform/qemu-smp include runtime/include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(smp8-soak-model PRIVATE SUNSTONE_SMP_CORES=8)
target_compile_options(smp8-soak-model PRIVATE
  ${SUNSTONE_KERNEL_FLAGS})
target_link_options(smp8-soak-model PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--defsym=SUNSTONE_SMP_STACKS=8
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/smp8-soak-model.map")
set_target_properties(smp8-soak-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")

if(SUNSTONE_PARTITIONED_SCHEDULER)
  sunstone_scheduler_partitioned_bind(smp8-soak-model "${smp_build}")
endif()

if(SUNSTONE_PARTITIONED_SCHEDULER)
  add_executable(scheduler-partitioned-qemu EXCLUDE_FROM_ALL
    platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c
    checks/scheduler-partitioned-qemu.c checks/fp-context.S
    "${smp_build}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${smp_build}/port.c" "${smp_build}/portASM.S")
  target_include_directories(scheduler-partitioned-qemu PRIVATE
    checks/scheduler-partitioned-qemu-config platform/qemu-smp include runtime/include
    "${FREERTOS_DIR}/include" "${smp_build}")
  target_compile_definitions(scheduler-partitioned-qemu PRIVATE SUNSTONE_SMP_CORES=8 SS_KERNEL_TEST_ADMIN_ENTER=1 SUNSTONE_OWNER_COUNTERS=1)
  target_compile_options(scheduler-partitioned-qemu PRIVATE ${SUNSTONE_KERNEL_FLAGS})
  target_link_options(scheduler-partitioned-qemu PRIVATE -nostdlib -static -Wl,--gc-sections
    -Wl,--defsym=SUNSTONE_SMP_STACKS=8 -Wl,--wrap=vTaskSwitchContext -Wl,--wrap=vPortRecursiveLock -Wl,--wrap=xTaskGenericNotify
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/scheduler-partitioned-qemu.map")
  set_target_properties(scheduler-partitioned-qemu PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
  sunstone_scheduler_partitioned_bind(scheduler-partitioned-qemu "${smp_build}")
endif()

if(SUNSTONE_PARTITIONED_SCHEDULER)
  # The concurrent workload with an interrupt above the API mask firing on every core (SS_STORM).
  add_executable(scheduler-partitioned-storm-qemu EXCLUDE_FROM_ALL
    platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c
    checks/scheduler-partitioned-qemu.c checks/fp-context.S
    "${smp_build}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${smp_build}/port.c" "${smp_build}/portASM.S")
  target_include_directories(scheduler-partitioned-storm-qemu PRIVATE
    checks/scheduler-partitioned-qemu-config platform/qemu-smp include runtime/include
    "${FREERTOS_DIR}/include" "${smp_build}")
  target_compile_definitions(scheduler-partitioned-storm-qemu PRIVATE SUNSTONE_SMP_CORES=8 SS_KERNEL_TEST_ADMIN_ENTER=1 SS_STORM=1 SUNSTONE_OWNER_COUNTERS=1)
  target_compile_options(scheduler-partitioned-storm-qemu PRIVATE ${SUNSTONE_KERNEL_FLAGS})
  target_link_options(scheduler-partitioned-storm-qemu PRIVATE -nostdlib -static -Wl,--gc-sections
    -Wl,--defsym=SUNSTONE_SMP_STACKS=8 -Wl,--wrap=vTaskSwitchContext -Wl,--wrap=vPortRecursiveLock -Wl,--wrap=xTaskGenericNotify
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/scheduler-partitioned-storm-qemu.map")
  set_target_properties(scheduler-partitioned-storm-qemu PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
  sunstone_scheduler_partitioned_bind(scheduler-partitioned-storm-qemu "${smp_build}")
endif()

if(SUNSTONE_PARTITIONED_SCHEDULER)
  # Switch requests raised in an interrupt must not be lost to an interrupt above the API mask landing in the
  # interrupt-exit path (checks/scheduler-yield-qemu.c): the plain build and the build with that interrupt (SS_STORM).
  foreach(variant yield yield-storm)
    set(extra SS_WAKE=1)
    if(variant STREQUAL "yield-storm")
      list(APPEND extra SS_STORM=1)
    endif()
    add_executable(scheduler-${variant}-qemu EXCLUDE_FROM_ALL
      platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c
      checks/scheduler-yield-qemu.c "${smp_build}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
      "${smp_build}/port.c" "${smp_build}/portASM.S")
    target_include_directories(scheduler-${variant}-qemu PRIVATE
      checks/scheduler-partitioned-qemu-config platform/qemu-smp include runtime/include
      "${FREERTOS_DIR}/include" "${smp_build}")
    target_compile_definitions(scheduler-${variant}-qemu PRIVATE SUNSTONE_SMP_CORES=8 SUNSTONE_OWNER_COUNTERS=1 ${extra})
    target_compile_options(scheduler-${variant}-qemu PRIVATE ${SUNSTONE_KERNEL_FLAGS})
    target_link_options(scheduler-${variant}-qemu PRIVATE -nostdlib -static -Wl,--gc-sections
      -Wl,--defsym=SUNSTONE_SMP_STACKS=8
      "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
      "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/scheduler-${variant}-qemu.map")
    set_target_properties(scheduler-${variant}-qemu PROPERTIES SUFFIX ".elf"
      LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
    sunstone_scheduler_partitioned_bind(scheduler-${variant}-qemu "${smp_build}")
  endforeach()
endif()

if(SUNSTONE_PARTITIONED_SCHEDULER)
  # Object locks across eight cores: independent objects must not wait for each other, and shared objects keep their
  # contents (checks/scheduler-objects-qemu.c). The queue, event group and stream buffer sources are the patched copies.
  add_executable(scheduler-objects-qemu EXCLUDE_FROM_ALL
    platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c
    checks/scheduler-objects-qemu.c
    "${smp_build}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${FREERTOS_DIR}/event_groups.c" "${FREERTOS_DIR}/stream_buffer.c"
    "${smp_build}/port.c" "${smp_build}/portASM.S")
  target_include_directories(scheduler-objects-qemu PRIVATE
    checks/scheduler-partitioned-qemu-config platform/qemu-smp include runtime/include
    "${FREERTOS_DIR}/include" "${smp_build}")
  target_compile_definitions(scheduler-objects-qemu PRIVATE SUNSTONE_SMP_CORES=8 SS_KERNEL_TEST_ADMIN_ENTER=1 SS_WAKE=1 SS_OBJECT_HOOK=1)
  target_compile_options(scheduler-objects-qemu PRIVATE ${SUNSTONE_KERNEL_FLAGS})
  target_link_options(scheduler-objects-qemu PRIVATE -nostdlib -static -Wl,--gc-sections
    -Wl,--defsym=SUNSTONE_SMP_STACKS=8
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/scheduler-objects-qemu.map")
  set_target_properties(scheduler-objects-qemu PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
  sunstone_scheduler_partitioned_bind(scheduler-objects-qemu "${smp_build}")
endif()

if(SUNSTONE_PARTITIONED_SCHEDULER)
  add_executable(scheduler-partitioned-heap-qemu EXCLUDE_FROM_ALL
    platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c
    checks/scheduler-partitioned-qemu.c
    "${smp_build}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${FREERTOS_DIR}/portable/MemMang/heap_4.c"
    "${smp_build}/port.c" "${smp_build}/portASM.S")
  target_include_directories(scheduler-partitioned-heap-qemu PRIVATE
    checks/scheduler-partitioned-qemu-config platform/qemu-smp include runtime/include
    "${FREERTOS_DIR}/include" "${smp_build}")
  target_compile_definitions(scheduler-partitioned-heap-qemu PRIVATE
    SUNSTONE_SMP_CORES=2 SS_QEMU_DYNAMIC_LIFECYCLE=1)
  target_compile_options(scheduler-partitioned-heap-qemu PRIVATE ${SUNSTONE_KERNEL_FLAGS})
  target_link_options(scheduler-partitioned-heap-qemu PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/scheduler-partitioned-heap-qemu.map")
  set_target_properties(scheduler-partitioned-heap-qemu PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
  sunstone_scheduler_partitioned_bind(scheduler-partitioned-heap-qemu "${smp_build}")
endif()
