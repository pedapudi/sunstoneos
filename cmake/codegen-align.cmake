include_guard(GLOBAL)
# Which OS units are compiled for strict alignment, and the code generation every OS unit shares.
#
# The kernel flags (cmake/kernel-flags.cmake) build for Armv8.0 with -mstrict-align, because the first code of a boot runs
# with the MMU off and Device memory faults on an unaligned access. After the MMU maps DRAM as Normal write-back and
# SCTLR_EL1.A is clear, a Cortex-A55 or A76 handles an unaligned access in hardware, and strict alignment only makes the compiler
# assemble a halfword, word or doubleword from bytes (about a quarter of the instructions of the NPU graph inspection of admission).
#
# Two explicit lists decide each unit of an OS image. A unit matched by SUNSTONE_RELAXED_ALIGN keeps the compiler free to use
# unaligned accesses: it runs at EL1 or EL0 under Normal write-back memory, reads or writes only Normal memory, and is not
# called before the MMU is on. A unit matched by SUNSTONE_STRICT_ALIGN must stay strict. A unit that no list names stays strict
# (the kernel flags), and checks/codegen-alignment.py reports it. A relaxed unit must not match a strict pattern: the configure
# step refuses that. The lists are written to alignment-classes.json in the build directory for that check.
#
# Entries are regular expressions over the absolute path of the source, in the syntax that CMake and Python share.
# Strict, and why:
set(SUNSTONE_STRICT_ALIGN
  # Before the MMU, at EL3, and the code that enables the translation: data accesses with the MMU off are Device accesses.
  "/platform/"
  "/arch/"
  "/os-image/(boot|continue|continue-sealed|platform|return-runtime|return-terminal)\\.[cS]$"
  "/freertos-a733-smp-cached/continue-core\\.c$"
  "/apps/os/(admission|lifetime|boot|cpu-return[^/]*|sealed|identity)\\.[cS]$"
  # The session decoder and the digest of it run at EL3 from the boot (a733_smp_boot -> a733_smp_contract_valid and
  # board_session_prepare), before the MMU is on; checks/codegen-alignment.py finds such units by the calls from the EL3 roots.
  "/apps/os/(session|grants|capture|npu-declaration)\\.c$"
  "/drivers/npu/npu_declaration\\.c$"
  "/drivers/storage/storage_digest[a-z_]*\\.[cS]$"
  # Registers, and the controllers' FIFOs and descriptors in Device or non-cacheable memory.
  "/drivers/[^/]+/[^/]*_native[^/]*\\.[cS]$"
  "/drivers/(board|clock|crypto|dram|gpu|i2c|pinctrl|power|thermal|usb)/"
  "/drivers/storage/(sunxi_mmc|sunxi_sdio|a733_mmc_platform_native)\\.c$"
  "/drivers/npu/(a733_npu_controller|a733_npu_memory|a733_npu_mmu|a733_npu_platform|a733_npu_sequence|a733_npu_startup|npu_runtime_queue)\\.c$"
  "/apps/os/(large_sd|watchdog|perf|cpu|thermal|cpu-return-spi|wifi_credential)\\.c$"
  # The memory routines that pre-MMU and Device callers link; the relaxed units link runtime/memory_unaligned.c.
  "/runtime/(memory|memmove)\\.c$"
  "/tinyusb-[^/]+/"
  "/storage-maintenance/")
# Strict by default: these trees hold registers and device queues, and a unit in them is relaxed only when the relaxed list below
# names it. They are not checked against that list.
set(SUNSTONE_STRICT_ALIGN_DEFAULT
  "/drivers/"
  "/apps/"
  "/os-image/"
  "/freertos-a733-smp-cached/"
  "/bluetooth-msbc/"
  "/runtime/")
# Relaxed, and why: the unit works on Normal memory only.
set(SUNSTONE_RELAXED_ALIGN
  # The scheduler, the unified memory ledger and the application region tables.
  "/kernel/(appmem|scheduler|umem)/[^/]*\\.c$"
  "/freertos-a733-smp-cached/(event_groups|queue|tasks)\\.c$"
  "/FreeRTOS-Kernel-[^/]+/(list\\.c|portable/MemMang/heap_4\\.c)$"
  # The runtime library around the application and the services.
  "/runtime/(counter_clock|cpu_topology_app|cxx_terminate|deadline-a733|deadline|device_memory_pool|new_delete|newlib|newlib_smp|newlib_syscalls_smp|npu_app|npu_app_queue|memory_unaligned)\\.cc?$"
  "/network/allocator\\.c$"
  # The OS tasks and services.
  "/apps/os/(application|client|irq|large|large_load|npu|npu-client|npu-graph-index|npu-irq|npu-observe|npu-queue|services|start|usb-service|usb0)\\.c$"
  "/apps/board/(audio|radio)\\.c$"
  # The NPU graph decoder and assembler, the runtime that places the graphs and the service ring.
  "/drivers/npu/(nbg_assembly|nbg_checksum|nbg_model|nbg_relocation|a733_npu_runtime|a733_npu_runtime_interim|npu_idle|npu_pool|npu_queue|npu_service)\\.c$"
  # The radio protocol and the file system, which parse frames and directory entries from buffers.
  "/drivers/net/(aic8800_[a-z_]*|sunstone_net|a733_radio_result|a733_radio_session|a733_radio_platform|a733_radio_entropy)\\.c$"
  "/drivers/storage/(fat32_ro|sdio_card)\\.c$"
  # The network stack and the supplicant.
  "/FreeRTOS-Plus-TCP-[^/]+/"
  "/FreeRTOS_TCP_Transmission_IPv4\\.c$"
  "/wpa_supplicant-[^/]+/")

# Add -mno-strict-align to the relaxed units of the named targets, for those targets only: the same source file is also built by
# other targets (models and diagnostics) that keep the kernel flags.
function(sunstone_relax_alignment)
  set(cond)
  set(relaxed_sources)
  foreach(target IN LISTS ARGN)
    list(APPEND cond "$<STREQUAL:$<TARGET_PROPERTY:NAME>,${target}>")
    get_target_property(sources ${target} SOURCES)
    foreach(source IN LISTS sources)
      if(NOT source MATCHES "^[A-Za-z0-9_/.+-]+$" OR NOT source MATCHES "\\.(c|cc|cpp)$")
        continue()
      endif()
      get_filename_component(path "${source}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
      set(relaxed FALSE)
      foreach(pattern IN LISTS SUNSTONE_RELAXED_ALIGN)
        if(path MATCHES "${pattern}")
          set(relaxed TRUE)
        endif()
      endforeach()
      if(NOT relaxed)
        continue()
      endif()
      foreach(pattern IN LISTS SUNSTONE_STRICT_ALIGN)
        if(path MATCHES "${pattern}")
          message(FATAL_ERROR "${path} is on both the strict and the relaxed alignment lists (cmake/codegen-align.cmake)")
        endif()
      endforeach()
      list(APPEND relaxed_sources "${source}")
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES relaxed_sources)
  list(JOIN cond "," cond)
  foreach(source IN LISTS relaxed_sources)
    set_property(SOURCE "${source}" DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY COMPILE_OPTIONS
      "$<$<OR:${cond}>:-mno-strict-align;-mtune=cortex-a55>")
    sunstone_relaxed_memory_calls("${source}" "${cond}")
  endforeach()
endfunction()

# The C units of the relaxed lists call the unaligned memory routines (include/sunstone_relaxed_memory.h).
function(sunstone_relaxed_memory_calls source cond)
  if(source MATCHES "\\.c$" AND NOT source MATCHES "/runtime/memory_unaligned\\.c$")
    set_property(SOURCE "${source}" DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY COMPILE_OPTIONS
      "$<$<OR:${cond}>:--include=${SUNSTONE_SOURCE_ROOT}/include/sunstone_relaxed_memory.h>")
  endif()
endfunction()

# An application's objects run at EL0 under Normal write-back memory and take the same treatment as a relaxed unit.
function(sunstone_relax_application target)
  set_property(GLOBAL APPEND PROPERTY SUNSTONE_ALIGN_APPLICATIONS ${target})
endfunction()

# Runs once, after every target and source of the directory is known (the OS image adds sources in several files).
function(sunstone_apply_alignment)
  set(targets sunstone-a733-os-core sunstone-a733-os-sd)
  foreach(candidate IN ITEMS audio headset gpu npu radio)
    if(TARGET sunstone-a733-os-${candidate})
      list(APPEND targets sunstone-a733-os-${candidate})
    endif()
  endforeach()
  get_property(applications GLOBAL PROPERTY SUNSTONE_ALIGN_APPLICATIONS)
  sunstone_relax_alignment(${targets})
  foreach(target IN LISTS applications)
    get_target_property(sources ${target} SOURCES)
    set(cond "$<STREQUAL:$<TARGET_PROPERTY:NAME>,${target}>")
    foreach(source IN LISTS sources)
      if(source MATCHES "\\.(c|cc|cpp)$")
        set_property(SOURCE "${source}" DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" APPEND PROPERTY COMPILE_OPTIONS
          "$<${cond}:-mno-strict-align;-mtune=cortex-a55>")
        sunstone_relaxed_memory_calls("${source}" "${cond}")
      endif()
    endforeach()
  endforeach()
  sunstone_write_alignment_classes("${targets}" "${applications}")
endfunction()

# The record that checks/codegen-alignment.py reads next to compile_commands.json.
function(sunstone_write_alignment_classes targets applications)
  set(json "{\n  \"strict\": [")
  list(JOIN SUNSTONE_STRICT_ALIGN "\", \"" strict)
  string(REPLACE "\\" "\\\\" strict "${strict}")
  list(JOIN SUNSTONE_STRICT_ALIGN_DEFAULT "\", \"" strict_default)
  string(REPLACE "\\" "\\\\" strict_default "${strict_default}")
  list(JOIN SUNSTONE_RELAXED_ALIGN "\", \"" relaxed)
  string(REPLACE "\\" "\\\\" relaxed "${relaxed}")
  list(JOIN targets "\", \"" targets_text)
  list(JOIN applications "\", \"" applications_text)
  if(applications_text)
    set(applications_text "\"${applications_text}\"")
  endif()
  string(APPEND json "\"${strict}\"],\n  \"strict_default\": [\"${strict_default}\"],\n  \"relaxed\": [\"${relaxed}\"],\n")
  string(APPEND json "  \"targets\": [\"${targets_text}\"],\n  \"applications\": [${applications_text}],\n  \"tune\": \"cortex-a55\"\n}\n")
  file(WRITE "${CMAKE_BINARY_DIR}/alignment-classes.json" "${json}")
endfunction()
