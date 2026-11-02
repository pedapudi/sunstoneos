cmake_minimum_required(VERSION 3.24)
project(latency_cost_model LANGUAGES C ASM)
get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${SUNSTONE_SOURCE_ROOT}/cmake/kernel-flags.cmake")
include("${SUNSTONE_SOURCE_ROOT}/cmake/patch.cmake")
include("${SUNSTONE_SOURCE_ROOT}/cmake/latency-instrumentation.cmake")
set(smp_port "${FREERTOS_DIR}/portable/GCC/ARM_CR82")
set(smp_build "${CMAKE_CURRENT_BINARY_DIR}/port")
file(MAKE_DIRECTORY "${smp_build}")
foreach(pair "port.c|c" "portASM.S|asm" "portmacro.h|macro")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 unit)
  list(GET parts 1 kind)
  apply_build_patch("${smp_build}/${unit}" "${smp_port}/${unit}"
    "${SUNSTONE_SOURCE_ROOT}/patches/freertos-smp-${kind}.patch")
endforeach()
apply_build_patch("${smp_build}/tasks.c" "${FREERTOS_DIR}/tasks.c"
  "${SUNSTONE_SOURCE_ROOT}/patches/freertos-smp-tasks.patch")
add_executable(latency-cost-model
  native.c model.c switch.c irq.c
  "${SUNSTONE_SOURCE_ROOT}/platform/qemu-smp/start.S"
  "${SUNSTONE_SOURCE_ROOT}/platform/qemu-smp/platform.c"
  "${SUNSTONE_SOURCE_ROOT}/runtime/memory.c"
  "${SUNSTONE_SOURCE_ROOT}/runtime/memmove.c"
  "${FREERTOS_DIR}/list.c" "${FREERTOS_DIR}/queue.c"
  "${smp_build}/tasks.c" "${smp_build}/port.c" "${smp_build}/portASM.S")
target_include_directories(latency-cost-model PRIVATE
  "${SUNSTONE_SOURCE_ROOT}/platform/qemu-smp" "${SUNSTONE_SOURCE_ROOT}/include"
  "${SUNSTONE_SOURCE_ROOT}/runtime/include" "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(latency-cost-model PRIVATE SUNSTONE_SMP_CORES=8 configIDLE_AFFINITY=1 LATENCY_COST_MODEL=1)
target_compile_options(latency-cost-model PRIVATE ${SUNSTONE_KERNEL_FLAGS} -Werror
  "$<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>")
target_link_options(latency-cost-model PRIVATE -nostdlib -static -Wl,--gc-sections
  -Wl,--defsym=SUNSTONE_SMP_STACKS=8 -Wl,--wrap=vTaskSwitchContext -Wl,--wrap=vApplicationIRQHandler
  "-Wl,-T,${SUNSTONE_SOURCE_ROOT}/platform/qemu-smp/link.ld")
set_target_properties(latency-cost-model PROPERTIES SUFFIX ".elf")
if(SUNSTONE_LATENCY)
  set_property(SOURCE "${SUNSTONE_SOURCE_ROOT}/runtime/latency_irq.c" APPEND PROPERTY
    COMPILE_DEFINITIONS __wrap_vApplicationIRQHandler=latency_cost_inner_irq)
  sunstone_enable_latency(latency-cost-model)
  sunstone_enable_latency_port(latency-cost-model)
endif()
