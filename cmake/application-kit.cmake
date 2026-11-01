include_guard(GLOBAL)

# Device-image applications use the same maintained SMP C++ runtime as OS images.
# Return every added source and archive so callers bind them into image identity.
function(sunstone_device_cxx_inputs output)
  if(NOT NEWLIB_ROOT OR NOT NEWLIB_SMP_ROOT OR NOT COMMAND sunstone_smp_compile)
    message(FATAL_ERROR "Device C++ composition requires the pinned SMP newlib runtime")
  endif()
  get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  set(inputs "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" "${root}/cmake/smp-runtime.cmake"
    "${root}/cmake/cxx-runtime.cmake")
  foreach(source newlib.c newlib_smp.c newlib_syscalls_smp.c new_delete.cc cxx_terminate.cc)
    list(APPEND inputs "${root}/runtime/${source}")
  endforeach()
  list(APPEND inputs "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
    "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a"
    "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
  set(${output} "${inputs}" PARENT_SCOPE)
endfunction()

# Replace the device image's minimal C headers with SMP newlib headers, add its
# runtime sources and preserve the existing reentrancy and compiler settings.
function(sunstone_device_cxx target)
  sunstone_device_cxx_inputs(inputs)
  get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  get_target_property(includes ${target} INCLUDE_DIRECTORIES)
  list(REMOVE_ITEM includes "${root}/runtime/include" "${SUNSTONE_NETWORK_C_HEADERS}")
  set_property(TARGET ${target} PROPERTY INCLUDE_DIRECTORIES "${includes}")
  sunstone_smp_compile(${target})
  foreach(input IN LISTS inputs)
    if(input MATCHES "\\.(c|cc)$")
      target_sources(${target} PRIVATE "${input}")
    elseif(input MATCHES "\\.a$")
      target_link_libraries(${target} PRIVATE "${input}")
    endif()
  endforeach()
  target_include_directories(${target} PRIVATE "${root}")
  target_link_options(${target} PRIVATE -Wl,--wrap=_impure_ptr)
endfunction()
