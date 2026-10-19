# Include after dma-service.cmake and dma-startup.cmake. The service ABI adds
# stream dispatch even for finite-only callers, so close those existing links.
set(i2s_stream_runtime drivers/audio/a733_i2s_stream.c drivers/dma/dma_service_i2s_stream.c)
foreach(target dma-service-model dma-startup-model)
  if(TARGET ${target})
    target_sources(${target} PRIVATE ${i2s_stream_runtime})
  endif()
endforeach()
apply_build_patch("${CMAKE_CURRENT_BINARY_DIR}/i2s-stream-port.c" "${smp_build}/port.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/freertos-smp-privileged.patch")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/i2s-stream-port.c" PROPERTIES
  COMPILE_DEFINITIONS "npu_service_task_privileged=i2s_stream_task_privileged")
add_executable(i2s-stream-model EXCLUDE_FROM_ALL
  platform/qemu-smp/start.S platform/qemu-smp/platform.c runtime/memory.c runtime/memmove.c
  arch/aarch64/device_memory.c platform/a733/dma_native.c platform/a733/shared_locks.c
  drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
  drivers/dma/dma_service.c drivers/dma/dma_service_i2s.c ${i2s_stream_runtime}
  drivers/audio/a733_i2s.c drivers/audio/a733_i2s_platform.c drivers/audio/a733_audio_leases.c
  drivers/clock/a733_clock_leases.c drivers/pinctrl/a733_pins.c drivers/pinctrl/a733_pin_tables.c
  checks/i2s-stream.c "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
  "${CMAKE_CURRENT_BINARY_DIR}/i2s-stream-port.c" "${smp_build}/portASM.S")
target_include_directories(i2s-stream-model PRIVATE platform/qemu-smp include runtime/include
  "${FREERTOS_DIR}/include" "${smp_build}")
target_compile_definitions(i2s-stream-model PRIVATE INCLUDE_xTaskGetSchedulerState=1
  INCLUDE_xTaskGetCurrentTaskHandle=1 INCLUDE_xSemaphoreGetMutexHolder=1)
target_compile_options(i2s-stream-model PRIVATE -march=armv8-a -mstrict-align -ffreestanding
  -fno-builtin -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections -O2 -g -Wall -Wextra -Werror -fstack-usage)
target_link_options(i2s-stream-model PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/i2s-stream-model.map")
set_target_properties(i2s-stream-model PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/platform/qemu-smp/link.ld")
# Same producer/provider composition with two disjoint, simultaneously live jobs.
get_target_property(i2s_duplex_sources i2s-stream-model SOURCES)
add_executable(i2s-duplex-model EXCLUDE_FROM_ALL ${i2s_duplex_sources})
foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS)
  get_target_property(i2s_duplex_property i2s-stream-model ${property})
  set_property(TARGET i2s-duplex-model PROPERTY ${property} "${i2s_duplex_property}")
endforeach()
get_target_property(i2s_duplex_definitions i2s-stream-model COMPILE_DEFINITIONS)
target_compile_definitions(i2s-duplex-model PRIVATE ${i2s_duplex_definitions} I2S_CHECK_DUPLEX=1)
set_target_properties(i2s-duplex-model PROPERTIES SUFFIX ".elf")

add_executable(i2s-duplex-retention-model EXCLUDE_FROM_ALL ${i2s_duplex_sources})
foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS)
  get_target_property(i2s_duplex_property i2s-stream-model ${property})
  set_property(TARGET i2s-duplex-retention-model PROPERTY ${property} "${i2s_duplex_property}")
endforeach()
target_compile_definitions(i2s-duplex-retention-model PRIVATE ${i2s_duplex_definitions} I2S_CHECK_DUPLEX=2)
set_target_properties(i2s-duplex-retention-model PROPERTIES SUFFIX ".elf")

get_target_property(i2s_cyclic_sources i2s-stream-model SOURCES)
list(REMOVE_ITEM i2s_cyclic_sources checks/i2s-stream.c)
add_executable(i2s-cyclic-model EXCLUDE_FROM_ALL ${i2s_cyclic_sources} checks/i2s-cyclic.c)
foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS COMPILE_DEFINITIONS)
 get_target_property(i2s_cyclic_property i2s-stream-model ${property})
 set_property(TARGET i2s-cyclic-model PROPERTY ${property} "${i2s_cyclic_property}")
endforeach()
set_target_properties(i2s-cyclic-model PROPERTIES SUFFIX ".elf")
add_executable(i2s-runtime-model EXCLUDE_FROM_ALL ${i2s_cyclic_sources}
 checks/i2s-runtime-provider.c apps/i2s/runtime.c)
foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS COMPILE_DEFINITIONS)
 get_target_property(i2s_runtime_property i2s-stream-model ${property})
 set_property(TARGET i2s-runtime-model PROPERTY ${property} "${i2s_runtime_property}")
endforeach()
target_include_directories(i2s-runtime-model PRIVATE apps/i2s platform/a733-smp-cached)
set_target_properties(i2s-runtime-model PROPERTIES SUFFIX ".elf")

add_executable(i2s-file-model EXCLUDE_FROM_ALL ${i2s_cyclic_sources}
 checks/i2s-runtime-provider.c apps/i2s/runtime.c)
foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS COMPILE_DEFINITIONS)
 get_target_property(i2s_file_property i2s-runtime-model ${property})
 set_property(TARGET i2s-file-model PROPERTY ${property} "${i2s_file_property}")
endforeach()
target_compile_definitions(i2s-file-model PRIVATE I2S_FILE_CHECK=1)
set_target_properties(i2s-file-model PROPERTIES SUFFIX ".elf")
