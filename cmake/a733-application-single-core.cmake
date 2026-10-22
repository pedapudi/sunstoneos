# Boot-CPU-only images of the supervised board and of the applications
# derived from it.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/patch.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/a733-board-application.cmake")

# Build sunstone-a733-<name>-single-core from the SMP image sunstone-a733-<name>.
#
# The image keeps every source of the SMP image except its cached SMP startup,
# secondary-CPU start, generated boot and platform, and build ID. In their
# place it links the boot-CPU entry, monitor and runtime of
# apps/storage-read-single-core, the A733 platform patched by that directory's
# platform.patch, and the CPU coherence and cached-memory sources that the
# monitor uses before it hands over to FreeRTOS.
#
# Sources compile with the SMP image's options, include directories and
# definitions, plus GUEST, SUNSTONE_CACHED_SINGLE_CORE, the platform log symbol
# and DEFINITIONS, which must name the profile macro that selects the
# application's branches in the shared monitor, runtime and platform.
# apps/board-single-core supplies the boot.h, cached.h and FreeRTOSConfig.h of
# every such image.
#
# PATCHES lists pairs of a unit name and a source relative to the source root.
# Each source is replaced by the output of apps/<name>-single-core/<unit>.patch
# in the build directory. PATCHED_INCLUDES lists pairs in the same form whose
# patched output is not compiled itself but included by another source; the
# build directory then joins the include path.
#
# The linker script is the board image's apps/board-single-core/link.ld with
# "board" and "BOARD" replaced by the application name in the same case, so
# that each image
# keeps its state in .<name>_runtime_retained between __<name>_retained_start
# and __<name>_retained_end. POOLS adds the 2 MiB uncached and device pools.
# The image must define <name>_single_core_geometry. Its execution profile is
# <name>-single-core-supervised-v1 and WORKLOAD names its manifest workload.
function(sunstone_single_core_application name)
  cmake_parse_arguments(PARSE_ARGV 1 sc "POOLS" "WORKLOAD" "PATCHES;PATCHED_INCLUDES;DEFINITIONS")
  if(SUNSTONE_BOARD_USB2_IRQ)
    message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
  endif()
  get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  set(base sunstone-a733-${name})
  set(target sunstone-a733-${name}-single-core)
  set(app "${root}/apps/${name}-single-core")
  set(headers_dir "${root}/apps/board-single-core")
  set(shared "${root}/apps/storage-read-single-core")
  set(build "${CMAKE_CURRENT_BINARY_DIR}/${name}-single-core")
  set(port "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
  set(profile "${name}-single-core-supervised-v1")
  file(MAKE_DIRECTORY "${build}")

  get_target_property(sources ${base} SOURCES)
  list(FILTER sources EXCLUDE REGEX "(/${name}-application/(boot|boot-supervised|platform|build-id)\\.c$|/storage-maintenance/(platform|port|continue)\\.(c|S)$|/platform/a733-smp-cached/|/platform/a733/cpu_|/freertos-a733-smp-cached/|/a733-smp-observe/(usb\\.c|service\\.S))")
  # The SMP base links its patched tasks.c from the port directory, which the
  # filter removes; a boot-CPU-only image links the kernel's own tasks.c.
  list(APPEND sources "${FREERTOS_DIR}/tasks.c")
  list(APPEND sources "${root}/platform/a733/cpu_coherence.c"
    "${root}/platform/a733/cpu_coherence.S" "${root}/platform/a733/cpu_start.c"
    "${root}/platform/a733-smp-cached/memory.c")
  set(patches)
  foreach(list PATCHES PATCHED_INCLUDES)
    set(pairs ${sc_${list}})
    while(pairs)
      list(POP_FRONT pairs unit path)
      apply_build_patch("${build}/${unit}.c" "${root}/${path}" "${app}/${unit}.patch")
      list(APPEND patches "${app}/${unit}.patch")
      if(list STREQUAL "PATCHES")
        list(REMOVE_ITEM sources "${root}/${path}")
        list(APPEND sources "${build}/${unit}.c")
      endif()
    endwhile()
  endforeach()
  apply_build_patch("${build}/port.c" "${port}/port.c" "${root}/patches/freertos-aarch64-clang.patch")
  apply_build_patch("${build}/platform.c" "${root}/platform/a733/platform.c" "${shared}/platform.patch")
  list(APPEND sources "${shared}/start.S" "${shared}/monitor.c" "${shared}/runtime.c"
    "${build}/platform.c" "${build}/port.c" "${port}/portASM.S"
    "${root}/drivers/usb/a733_usb_platform.c" "${root}/drivers/usb/a733_usb_platform_native.c"
    "${root}/arch/aarch64/start.S" "${root}/arch/aarch64/vectors.S" "${root}/arch/aarch64/cache.S")
  list(REMOVE_DUPLICATES sources)
  set_source_files_properties("${root}/drivers/gpu/a733_gpu_resources_native.c"
    "${root}/drivers/i2c/a733_i2c.c" PROPERTIES COMPILE_OPTIONS "-iquote;${root}/platform/a733")
  add_executable(${target} ${sources} $<TARGET_OBJECTS:a733-board-usb2-host>)

  get_target_property(options ${base} COMPILE_OPTIONS)
  get_target_property(includes ${base} INCLUDE_DIRECTORIES)
  get_target_property(definitions ${base} COMPILE_DEFINITIONS)
  list(FILTER definitions EXCLUDE REGEX "^SUNSTONE_A733_SMP_DIAGNOSTIC=")
  list(FILTER includes EXCLUDE REGEX "(a733-smp-cached|freertos-a733-smp-cached)")
  set_property(TARGET ${target} PROPERTY COMPILE_OPTIONS "${options}")
  if(sc_PATCHED_INCLUDES)
    target_include_directories(${target} BEFORE PRIVATE "${headers_dir}" "${build}" "${port}")
  else()
    target_include_directories(${target} BEFORE PRIVATE "${headers_dir}" "${port}")
  endif()
  target_include_directories(${target} PRIVATE ${includes} "${root}/apps/${name}")
  sunstone_enable_radio_security(${target} "${SUNSTONE_RSN_DIR}")
  target_compile_definitions(${target} PRIVATE ${definitions} GUEST=1 SUNSTONE_CACHED_SINGLE_CORE=1
    SUNSTONE_SINGLE_CORE_PLATFORM_LOG=__${name}_platform_log ${sc_DEFINITIONS})
  set_property(SOURCE "${shared}/monitor.c" "${shared}/runtime.c" "${build}/platform.c" APPEND PROPERTY COMPILE_OPTIONS
    "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,${target}>:-include>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,${target}>:${headers_dir}/boot.h>")

  file(READ "${headers_dir}/link.ld" script)
  string(REPLACE "board" "${name}" script "${script}")
  string(TOUPPER "${name}" label)
  string(REPLACE "BOARD" "${label}" script "${script}")
  file(WRITE "${build}/link.ld.tmp" "${script}")
  configure_file("${build}/link.ld.tmp" "${build}/link.ld" COPYONLY)
  file(REMOVE "${build}/link.ld.tmp")
  set(pools)
  if(sc_POOLS)
    set(pools -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152)
  endif()
  target_link_options(${target} PRIVATE -nostdlib -static -Wl,--gc-sections
    -Wl,--undefined=${name}_single_core_geometry ${pools}
    "-Wl,-T,${build}/link.ld" "-Wl,-Map,${build}/image.map")
  set_target_properties(${target} PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${build}/link.ld")

  # Bind every declared producer input; the frozen closure also records compiler depfiles.
  file(GLOB headers "${headers_dir}/*.h" "${root}/apps/${name}/*.h" "${root}/include/*.h")
  list(FILTER sources EXCLUDE REGEX "^\\$<TARGET_OBJECTS:")
  set(inputs ${sources} ${headers} ${patches} "${headers_dir}/link.ld"
    "${shared}/platform.patch" "${shared}/boot.h" "${shared}/cached.h" "${shared}/FreeRTOSConfig.h"
    "${port}/portmacro.h" "${port}/port.c" "${root}/patches/freertos-aarch64-clang.patch"
    "${CMAKE_CURRENT_LIST_FILE}" "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
  if(EXISTS "${app}/CMakeLists.txt")
    list(APPEND inputs "${app}/CMakeLists.txt")
  endif()
  sunstone_build_identity(${target} TAG "${profile};ROM-USB-mode2;SRE;60s" BUILD_DIR "${build}"
    INPUTS ${inputs} TARGETS a733-board-usb2-host PROFILE "${profile}" WORKLOAD "${sc_WORKLOAD}")
endfunction()
