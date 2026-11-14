# The layout of pxCurrentTCBs belongs to the kernel that defines it
# (include/sunstone_current_tcb.h). FreeRTOS tasks.c patched with
# patches/freertos-smp-tasks.patch, and the control and partitioned schedulers
# built on it, give each core a 64-byte line and refuse to compile unless
# portCURRENT_TCB_SHIFT is 6. The unmodified tasks.c keeps 8-byte entries, the
# header's default. Every unit of an image must use the kernel's shift, so a
# target that compiles a generated tasks.c (any tasks.c outside FREERTOS_DIR)
# gets the definition for all its units. A target that mixes the two kinds
# fails to link (the guard symbols in the header). Every module that generates
# a patched tasks.c includes this file, so standalone projects get the rule too.
include_guard(GLOBAL)
function(sunstone_current_tcb_apply directory)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
      continue()
    endif()
    get_target_property(sources ${target} SOURCES)
    if(NOT sources)
      continue()
    endif()
    foreach(source IN LISTS sources)
      get_filename_component(name "${source}" NAME)
      if(name STREQUAL "tasks.c" AND NOT source STREQUAL "${FREERTOS_DIR}/tasks.c")
        target_compile_definitions(${target} PUBLIC portCURRENT_TCB_SHIFT=6)
        break()
      endif()
    endforeach()
  endforeach()
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    sunstone_current_tcb_apply("${child}")
  endforeach()
endfunction()
function(sunstone_current_tcb_apply_all)
  sunstone_current_tcb_apply("${CMAKE_SOURCE_DIR}")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL sunstone_current_tcb_apply_all)
