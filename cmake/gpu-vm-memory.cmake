if(TARGET gpu-memory-model)
  add_executable(gpu-vm-memory-model EXCLUDE_FROM_ALL
    "${CMAKE_CURRENT_BINARY_DIR}/npu-service-start.S"
    "${CMAKE_CURRENT_BINARY_DIR}/gpu-memory-platform.c"
    runtime/memory.c checks/a733-gpu-vm-memory.c
    drivers/gpu/pvr_gpu_vm.c platform/a733/gpu_vm_memory.c arch/aarch64/device_memory.c
    "${FREERTOS_DIR}/tasks.c" "${FREERTOS_DIR}/queue.c" "${FREERTOS_DIR}/list.c"
    "${CMAKE_CURRENT_BINARY_DIR}/gpu-service-port.c" "${smp_build}/portASM.S")
  foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS)
    get_target_property(value gpu-service-model ${property})
    set_property(TARGET gpu-vm-memory-model PROPERTY ${property} "${value}")
  endforeach()
  get_target_property(options gpu-vm-memory-model LINK_OPTIONS)
  list(REMOVE_ITEM options "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/gpu-service-model.map")
  set_property(TARGET gpu-vm-memory-model PROPERTY LINK_OPTIONS "${options}")
  target_link_options(gpu-vm-memory-model PRIVATE
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/gpu-vm-memory-model.map")
  target_compile_definitions(gpu-vm-memory-model PRIVATE
    INCLUDE_xTaskGetSchedulerState=1 INCLUDE_xTaskGetCurrentTaskHandle=1)
  set_target_properties(gpu-vm-memory-model PROPERTIES SUFFIX ".elf")
endif()
