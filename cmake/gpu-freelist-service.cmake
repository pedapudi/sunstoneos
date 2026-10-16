# Include after gpu.cmake. This fixture reuses its firmware and privileged port.
if(SUNSTONE_GPU_FIRMWARE)
  get_target_property(gpu_freelist_sources gpu-service-model SOURCES)
  list(REMOVE_ITEM gpu_freelist_sources checks/pvr-gpu-service.c
    drivers/gpu/pvr_free_list.c drivers/gpu/pvr_gpu_events.c)
  list(REMOVE_DUPLICATES gpu_freelist_sources)
  add_executable(gpu-freelist-service-model EXCLUDE_FROM_ALL ${gpu_freelist_sources}
    checks/pvr-gpu-freelist-service.c drivers/gpu/pvr_free_list.c drivers/gpu/pvr_gpu_events.c)
  foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS COMPILE_DEFINITIONS)
    get_target_property(value gpu-service-model ${property})
    set_property(TARGET gpu-freelist-service-model PROPERTY ${property} "${value}")
  endforeach()
  get_target_property(gpu_freelist_link_options gpu-freelist-service-model LINK_OPTIONS)
  list(REMOVE_ITEM gpu_freelist_link_options "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/gpu-service-model.map")
  set_property(TARGET gpu-freelist-service-model PROPERTY LINK_OPTIONS "${gpu_freelist_link_options}")
  target_link_options(gpu-freelist-service-model PRIVATE
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/gpu-freelist-service-model.map")
  set_target_properties(gpu-freelist-service-model PROPERTIES SUFFIX ".elf")
endif()
