# Dedicated staged CPU profile. All adaptation is private to this target.
if(SUNSTONE_A733_OS_RESIDENT OR SUNSTONE_A733_OS_AUDIO OR SUNSTONE_A733_OS_RADIO OR
   SUNSTONE_A733_OS_HEADSET OR SUNSTONE_A733_OS_NPU OR SUNSTONE_A733_OS_GPU_LINEAR OR
   SUNSTONE_A733_OS_PERF OR NOT A733_A76_ADMITTED OR NOT SUNSTONE_A733_OS_CPU_CLOCK OR
   NOT SUNSTONE_MIMUS_WORKERS STREQUAL "8")
  message(FATAL_ERROR "CPU return requires supervised staged eight-worker A76 CPU-clock recognition without optional services")
endif()
set(os_return "${os_root}/apps/os/cpu-return")
set(os_qualification "${os_root}/apps/cpu-runtime-qualification")
configure_file("${os_qualification}/runtime.h" "${os_build}/cpu-return-runtime.h" COPYONLY)
foreach(pair "runtime.c|runtime|${os_qualification}/runtime.c"
             "terminal.S|terminal|${os_qualification}/terminal.S"
             "continue.c|continue|${os_root}/platform/a733-smp-cached/continue.c"
             "continue.S|vectors|${storage_build}/continue.S"
             "main.c|main|${os_root}/apps/board/main.c"
             "usb0.c|usb0|${os_root}/apps/mimus-board/usb0.c"
             "cpu.c|cpu|${os_root}/apps/os/cpu.c"
             "thermal.c|thermal|${os_root}/apps/os/thermal.c"
             "start.c|start|${os_root}/apps/os/start.c"
             "admission.c|admission|${os_root}/apps/os/admission.c"
             "boot.c|boot|${os_build}/boot.c"
             "recognition.cc|recognition|${os_recognition}")
  string(REPLACE "|" ";" parts "${pair}")
  list(GET parts 0 unit)
  list(GET parts 1 patch_name)
  list(GET parts 2 original)
  set(patch_file "${os_return}/${patch_name}.patch")
  set(patched "${os_build}/return-${unit}")
  apply_build_patch("${patched}" "${original}" "${patch_file}")
  if(patch_name STREQUAL "main")
    apply_build_patch("${os_build}/return-main-pinned.c" "${patched}" "${os_return}/main-pin.patch")
    list(APPEND os_generated "${patched}")
    set(patched "${os_build}/return-main-pinned.c")
  endif()
  list(REMOVE_ITEM os_sources "${original}")
  list(REMOVE_ITEM os_added "apps/os/${unit}")
  if(patch_name STREQUAL "boot")
    set(os_boot_final "${patched}")
  else()
    list(APPEND os_sources "${patched}")
  endif()
  list(APPEND os_generated "${patched}")
  list(APPEND os_patches "${patch_file}")
  list(APPEND os_external_inputs "${original}")
endforeach()
list(APPEND os_added apps/os/cpu-return.c apps/os/cpu-return-spi.c apps/os/cpu-return-entry.c)
list(APPEND os_include_dirs "${os_build}")
list(APPEND os_external_inputs "${os_qualification}/runtime.h" "${os_build}/cpu-return-runtime.h"
  "${os_return}/main-pin.patch" "${CMAKE_CURRENT_LIST_FILE}")
