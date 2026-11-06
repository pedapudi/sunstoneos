# OS implementation and device owners, independent of application sources.
include("${CMAKE_CURRENT_LIST_DIR}/latency-instrumentation.cmake")
option(SUNSTONE_A733_OS "Build the integrated OS image" OFF)
option(SUNSTONE_A733_OS_SD_WRITE "OS image whose SD0 owner task writes the card under runner-admitted bounds (write-capable: the runner needs a coordinator approval record)" OFF)
option(SUNSTONE_A733_OS_RESIDENT "Integrated OS with the resident lifetime" OFF)
option(SUNSTONE_A733_OS_USB_CLASSES "OS input, serial and camera host services" OFF)
option(SUNSTONE_A733_OS_USB_JPEG "OS camera MJPEG hardware decoder" OFF)
option(SUNSTONE_A733_OS_DMA "Adopted DMA0 RAM-copy service with normal retirement" OFF)
option(SUNSTONE_A733_OS_RADIO_SCAN "Radio station that scans and returns: board workload 3, no USB host, scan-only profile, normal return" OFF)
option(SUNSTONE_A733_OS_GPU "OS GPU compute queue" OFF)
option(SUNSTONE_GPU_TIMING "Capture optional GPU job timing records" OFF)
option(SUNSTONE_GPU_NSI_PMU "Capture experimental GPU master NSI traffic counters" OFF)
if(SUNSTONE_GPU_NSI_PMU AND NOT SUNSTONE_GPU_TIMING)
  message(FATAL_ERROR "SUNSTONE_GPU_NSI_PMU requires SUNSTONE_GPU_TIMING")
endif()
if(SUNSTONE_GPU_TIMING AND NOT SUNSTONE_A733_OS_GPU)
  message(FATAL_ERROR "SUNSTONE_GPU_TIMING requires SUNSTONE_A733_OS_GPU")
endif()
option(SUNSTONE_A733_OS_GPU_RECOVERY_TEST
  "Withhold one completion until the ordinary job deadline in a qualification image" OFF)
set(SUNSTONE_A733_OS_GPU_BISECT "" CACHE STRING
  "GPU diagnostic limit: constructor, firmware, jobs:N or layer:K")
option(SUNSTONE_A733_OS_CPU_RETURN "Application result with success-only ROM return" OFF)
if(SUNSTONE_A733_OS_RADIO_SCAN AND NOT SUNSTONE_A733_OS_CPU_RETURN)
  message(FATAL_ERROR "The scanning radio station needs SUNSTONE_A733_OS_CPU_RETURN=ON")
endif()
option(SUNSTONE_GPU_FAST_TIMING "Capture optional GPU IRQ-entry counter timestamps" OFF)
option(SUNSTONE_A733_OS_CORE "Build the OS core without an application" OFF)
option(SUNSTONE_A733_CPU_TOPOLOGY "Expose read-only topology and per-core architectural observations" OFF)
option(SUNSTONE_A733_OS_HELLO "Build the hello-compute OS image" OFF)
option(SUNSTONE_A733_HW_FACTS "Record the hardware facts table on every core before the scheduler (docs/hardware/a733-facts-probe.md)" OFF)
if(NOT SUNSTONE_A733_OS AND NOT SUNSTONE_A733_OS_CORE AND NOT SUNSTONE_A733_OS_HELLO)
  return()
endif()
include_guard(GLOBAL)
if(SUNSTONE_A733_OS_DMA AND NOT SUNSTONE_A733_OS_CPU_RETURN)
  message(FATAL_ERROR "Adopted RAM DMA currently requires the returning CPU profile")
endif()
if(SUNSTONE_A733_OS_GPU_RECOVERY_TEST AND
   (NOT SUNSTONE_A733_OS_GPU OR NOT SUNSTONE_A733_OS_CPU_RETURN OR
    NOT SUNSTONE_APPLICATION STREQUAL "gpu-lifecycle"))
  message(FATAL_ERROR "GPU recovery qualification requires the returning GPU lifecycle image")
endif()
if(SUNSTONE_A733_OS_USB_CLASSES AND SUNSTONE_A733_OS_CPU_RETURN)
  message(FATAL_ERROR "Returning USB class drain is not qualified")
endif()
if(SUNSTONE_A733_OS_USB_CLASSES AND NOT SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "USB class services require SUNSTONE_BOARD_USB2_IRQ=ON")
endif()
if(SUNSTONE_A733_OS_USB_JPEG AND SUNSTONE_A733_OS_GPU_LINEAR)
  message(FATAL_ERROR "USB JPEG owns runtime NC allocation; other device owners need allocator serialization")
endif()
if(SUNSTONE_A733_OS_USB_JPEG AND NOT SUNSTONE_A733_OS_USB_CLASSES)
  message(FATAL_ERROR "USB JPEG requires USB class services")
endif()
if(NOT SUNSTONE_BOARD_SUPERVISED OR NOT TARGET sunstone-a733-board)
  message(FATAL_ERROR "The OS kit requires the supervised native board composition")
endif()
if(NOT NEWLIB_ROOT OR NOT NEWLIB_SMP_ROOT)
  message(FATAL_ERROR "The OS kit requires the pinned SMP C and C++ runtime")
endif()
set(os_gpu_bisect_definitions)
if(SUNSTONE_A733_OS_GPU_BISECT)
  if(NOT SUNSTONE_A733_OS_GPU OR NOT SUNSTONE_A733_OS_CPU_RETURN)
    message(FATAL_ERROR "GPU diagnostics require the queue and returning profile")
  endif()
  if(NOT SUNSTONE_A733_OS_GPU_BISECT MATCHES "^(constructor|firmware|jobs:([1-9][0-9]*)|layer:([0-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-4]))$")
    message(FATAL_ERROR "GPU diagnostic limit must be constructor, firmware, jobs:N or layer:K (K < 255)")
  endif()
  set(os_bisect_limit 0)
  if(CMAKE_MATCH_1 STREQUAL "constructor")
    set(os_bisect_stage 1)
  elseif(CMAKE_MATCH_1 STREQUAL "firmware")
    set(os_bisect_stage 2)
  elseif(CMAKE_MATCH_2)
    set(os_bisect_stage 3)
    set(os_bisect_limit ${CMAKE_MATCH_2})
  else()
    set(os_bisect_stage 4)
    set(os_bisect_limit ${CMAKE_MATCH_3})
  endif()
  set(os_gpu_bisect_definitions SUNSTONE_A733_OS_GPU_BISECT=1
    OS_GPU_BISECT_STAGE=${os_bisect_stage} OS_GPU_BISECT_LIMIT=${os_bisect_limit}u)
endif()
if(SUNSTONE_A733_OS_GPU)
  set(SUNSTONE_GPU_FIRMWARE "" CACHE FILEPATH "Pinned A733 GPU firmware")
  if(NOT SUNSTONE_GPU_FIRMWARE)
    message(FATAL_ERROR "The GPU linear build requires SUNSTONE_GPU_FIRMWARE")
  endif()
  file(SHA256 "${SUNSTONE_GPU_FIRMWARE}" os_gpu_firmware_hash)
  if(NOT os_gpu_firmware_hash STREQUAL "1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2")
    message(FATAL_ERROR "GPU firmware identity refused")
  endif()
endif()
set(os_root "${SUNSTONE_SOURCE_ROOT}")
set(os_build "${CMAKE_CURRENT_BINARY_DIR}/os-image")
file(MAKE_DIRECTORY "${os_build}")
get_target_property(os_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM os_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
  "${board_build}/boot-host-irq.c" "${board_build}/boot-supervised.c" "${board_build}/boot.c" "${board_build}/build-id.c"
  "${os_root}/apps/board/admission.c" "${os_root}/apps/board/identity.c"
  "${os_root}/apps/board/client.c" "${os_root}/apps/board/usb0.c")
if(SUNSTONE_A733_OS_RESIDENT)
  set(os_contract_magic 0x415337334f524331ULL)
  set(os_result_magic 0x415337334f525231ULL)
else()
  set(os_contract_magic 0x415337334f534331ULL)
  set(os_result_magic 0x415337334f535231ULL)
endif()
# One EL3 boot for both lifetimes: the patch branches on the lifetime that admission verified from the contract magic
# (a733_smp_lifetime in apps/os/admission.c) for the watchdog readback, the result magic and the cores' held startup.
apply_build_patch("${os_build}/boot.c" "${board_build}/boot-supervised.c" "${os_root}/apps/os/boot.patch")
# Retained per-core fault records and their previous-boot report
# (docs/fault-capture.md); SUNSTONE_A733_FAULT_PAGE enables them.
set(os_added apps/os/admission.c apps/os/identity.c apps/os/irq.c apps/os/services.c apps/os/start.c
  runtime/cpu_topology_app.c
  apps/os/usb-service.c apps/os/large.c apps/os/large_load.c drivers/storage/sunxi_mmc.c drivers/storage/a733_mmc_platform.c drivers/storage/a733_mmc_platform_native.c drivers/storage/fat32_ro.c drivers/storage/storage_digest.c drivers/storage/storage_digest_extensions.c drivers/storage/storage_digest_ce.S kernel/appmem/app_region.c kernel/appmem/large_admit.c kernel/umem/umem.c apps/os/application.c apps/os/session.c apps/os/grants.c apps/os/usb0.c apps/os/client.c apps/os/capture.c
  runtime/deadline.c runtime/deadline-a733.c
  runtime/newlib.c runtime/newlib_smp.c runtime/newlib_syscalls_smp.c runtime/new_delete.cc runtime/cxx_terminate.cc
  apps/os/thermal.c apps/os/watchdog.c platform/a733-smp-cached/irq_table.c drivers/thermal/a733_thermal.c
  drivers/thermal/a733_thermal_native.c drivers/gpu/gicv3_spi.c platform/a733/fault.S platform/a733/fault.c)
if(SUNSTONE_A733_OS_SD_WRITE)
  # One SD0 owner task serves the loader's reads and the FAT32 write service (apps/os/sd_owner.c); the SD core is built with its write path.
  list(APPEND os_added apps/os/sd_owner.c drivers/storage/a733_storage_bounds.c drivers/storage/a733_storage_update.c drivers/storage/storage_wait.c)
endif()
set(os_patches "${os_root}/apps/os/boot.patch")
if(SUNSTONE_A733_OS_USB_CLASSES)
  apply_build_patch("${os_build}/link-usb.ld" "${storage_build}/link.ld"
    "${os_root}/apps/os/usb-memory-link.patch")
  list(APPEND os_patches "${os_root}/apps/os/usb-memory-link.patch")
endif()
set(os_generated "${os_build}/boot.c")
# The radio station is in every image: the board's own radio composition serves the radio workloads, and the session
# grants it with the radio device (apps/os/session.c). Its owner and client report resident progress through the
# patched main.c.
list(APPEND os_added apps/os/radio.c apps/os/wifi_credential.c)
if(SUNSTONE_A733_OS_RADIO_SCAN)
  list(APPEND os_added apps/os/radio_scan.c apps/os/ce_probe.c)
endif()
if(NOT SUNSTONE_A733_OS_CPU_RETURN)
  # The returning lifetime patches main.c itself (cmake/a733-os-cpu-return.cmake); the resident
  # progress reports of this patch have no use in it.
  apply_build_patch("${os_build}/main.c" "${os_root}/apps/board/main.c" "${os_root}/apps/os/main.patch")
  list(REMOVE_ITEM os_sources "${os_root}/apps/board/main.c")
  list(APPEND os_sources "${os_build}/main.c")
  list(APPEND os_patches "${os_root}/apps/os/main.patch")
  list(APPEND os_generated "${os_build}/main.c")
endif()
if(SUNSTONE_A733_OS_RESIDENT)
  list(REMOVE_ITEM os_sources "${os_root}/apps/board/radio_profile.c")
  list(APPEND os_added apps/os/radio_profile.c)
endif()
if(SUNSTONE_A733_OS_DMA)
  list(APPEND os_added apps/os/dma.c platform/a733/dma_pool_session.c platform/a733/dma_startup.c
    platform/a733/dma_native.c drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
    drivers/dma/a733_dma_platform.c drivers/dma/a733_dma_platform_native.c drivers/dma/dma_service.c
    drivers/dma/dma_service_i2s.c drivers/dma/dma_service_i2s_stream.c drivers/audio/a733_i2s.c
    drivers/audio/a733_i2s_stream.c drivers/audio/dma_service_i2s_irq.c)
endif()
# The I2S capture service is in every image, and a session grants it (docs/designs/one-os-image.md). Its sources are
# the I2S image's receive composition (cmake/a733-i2s-application.cmake).
list(APPEND os_added apps/os/audio.c platform/a733/dma_pool_session.c platform/a733/dma_startup.c
    platform/a733/dma_native.c drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
    drivers/dma/a733_dma_platform.c drivers/dma/a733_dma_platform_native.c drivers/dma/dma_service.c
    drivers/dma/dma_service_i2s.c drivers/dma/dma_service_i2s_stream.c drivers/audio/a733_i2s.c
    drivers/audio/a733_i2s_stream.c drivers/audio/a733_i2s_platform.c drivers/audio/a733_i2s_platform_native.c
    drivers/audio/a733_audio_leases.c drivers/audio/dma_service_i2s_irq.c)
# The NPU service is in every image, and a session grants it (docs/designs/one-os-image.md).
list(APPEND os_added apps/os/npu.c apps/os/npu-declaration.c apps/os/npu-graph-index.c apps/os/npu-client.c apps/os/npu-irq.c
  platform/a733/npu_native.c runtime/counter_clock.c drivers/npu/a733_npu_runtime.c drivers/npu/a733_npu_runtime_interim.c drivers/npu/npu_pool.c drivers/npu/npu_declaration.c
  drivers/npu/npu_service.c drivers/npu/npu_queue.c drivers/npu/npu_runtime_queue.c drivers/npu/npu_idle.c drivers/npu/a733_npu_sequence.c
  drivers/npu/a733_npu_controller.c drivers/npu/a733_npu_memory.c
  drivers/npu/a733_npu_mmu.c drivers/npu/nbg_model.c drivers/npu/nbg_assembly.c drivers/npu/nbg_checksum.c
  drivers/npu/nbg_relocation.c drivers/npu/a733_npu_startup.c drivers/npu/a733_npu_platform.c
  drivers/npu/a733_npu_platform_native.c apps/os/npu-observe.c)
file(GLOB os_npu_headers "${os_root}/include/*npu*.h" "${os_root}/include/nbg*.h")
list(APPEND os_external_inputs ${os_npu_headers}
  "${os_root}/platform/a733/npu_irq.c" "${os_root}/include/a733_npu_irq.h"
  "${os_root}/drivers/npu/nbg_command.h")
set(os_include_dirs)
set(os_headset_include_dirs)
# The headset service: the Bluetooth stack, its pinned Zephyr security sources and the mSBC codec with its pinned libsbc
# sources. They form the headset object library below, which an image links only when its application declares the
# audio-headset device (cmake/sunstone-image.cmake); without both source directories the library does not exist and
# such an image is refused at configure.
set(SUNSTONE_BT_SECURITY_DIR "" CACHE PATH "Pinned Zephyr BR/EDR security source directory")
set(SUNSTONE_BT_SBC_DIR "" CACHE PATH "Pinned BlueZ libsbc2.0 source directory")
set(os_sbc_sources)
if(SUNSTONE_BT_SECURITY_DIR AND SUNSTONE_BT_SBC_DIR)
  set(os_bluetooth "${os_build}/bluetooth")
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${os_root}/apps/bluetooth/security-source.py"
    --source "${SUNSTONE_BT_SECURITY_DIR}" --output "${os_bluetooth}" RESULT_VARIABLE os_security_status)
  if(NOT os_security_status EQUAL 0)
    message(FATAL_ERROR "Pinned Bluetooth security source verification failed")
  endif()
  file(READ "${os_root}/apps/bluetooth/security-source.json" os_security_record)
  string(JSON os_security_count LENGTH "${os_security_record}" sources)
  math(EXPR os_security_last "${os_security_count}-1")
  foreach(index RANGE 0 ${os_security_last})
    string(JSON name MEMBER "${os_security_record}" sources ${index})
    list(APPEND os_external_inputs "${SUNSTONE_BT_SECURITY_DIR}/${name}")
  endforeach()
  list(APPEND os_added apps/os/headset.c apps/bluetooth/bluetooth.c apps/bluetooth/headset.c apps/bluetooth/pcm.c
    drivers/uart/a733_uart.c drivers/uart/a733_uart_platform.c drivers/uart/a733_uart_platform_native.c
    drivers/bluetooth/bt_h4.c drivers/bluetooth/bt_hci.c drivers/bluetooth/bt_security.c drivers/bluetooth/bt_acl.c
    drivers/bluetooth/bt_l2cap.c drivers/bluetooth/bt_sdp.c drivers/bluetooth/bt_sdp_server.c
    drivers/bluetooth/bt_rfcomm.c drivers/bluetooth/bt_hfp_ag.c drivers/bluetooth/bt_sco.c)
  list(APPEND os_sources "${os_bluetooth}/zephyr-ssp.c" "${os_bluetooth}/zephyr-keys_br.c"
    "${os_bluetooth}/zephyr-security-level.c")
  list(APPEND os_external_inputs "${os_root}/apps/bluetooth/security-source.json"
    "${os_root}/apps/bluetooth/security-source.py"
    "${os_bluetooth}/zephyr-security-wire.h")
  file(GLOB os_bluetooth_headers "${os_root}/apps/bluetooth/*.h")
  list(APPEND os_external_inputs ${os_bluetooth_headers})
  list(APPEND os_headset_include_dirs "${os_bluetooth}")
  # The Bluetooth image's mSBC codec (cmake/a733-bluetooth.cmake) with its pinned libsbc sources, checked against
  # apps/bluetooth/msbc-source.json. The profile's mode chooses CVSD or mSBC at run time, so the codec is always linked
  # with the headset.
  file(READ "${os_root}/apps/bluetooth/msbc-source.json" os_sbc_manifest)
  string(JSON os_sbc_count LENGTH "${os_sbc_manifest}" sources)
  math(EXPR os_sbc_last "${os_sbc_count}-1")
  foreach(index RANGE 0 ${os_sbc_last})
    string(JSON name GET "${os_sbc_manifest}" sources ${index} path)
    string(JSON expected GET "${os_sbc_manifest}" sources ${index} sha256)
    file(SHA256 "${SUNSTONE_BT_SBC_DIR}/${name}" actual)
    if(NOT actual STREQUAL expected)
      message(FATAL_ERROR "Pinned libsbc input differs: ${name}")
    endif()
    list(APPEND os_external_inputs "${SUNSTONE_BT_SBC_DIR}/${name}")
  endforeach()
  list(APPEND os_external_inputs "${os_root}/apps/bluetooth/msbc-source.json")
  # The libsbc license (LGPL-2.1-or-later) and its pinned source record stay beside the build that links the codec.
  file(COPY "${SUNSTONE_BT_SBC_DIR}/COPYING.LIB" "${os_root}/apps/bluetooth/msbc-source.json"
    DESTINATION "${os_bluetooth}/msbc-license")
  list(APPEND os_added apps/bluetooth/msbc.c apps/bluetooth/playback.c)
  set(os_sbc_sources "${SUNSTONE_BT_SBC_DIR}/sbc/sbc.c" "${SUNSTONE_BT_SBC_DIR}/sbc/sbc_primitives.c")
  list(APPEND os_sources ${os_sbc_sources})
  list(APPEND os_headset_include_dirs "${SUNSTONE_BT_SBC_DIR}")
endif()
# A76 placement, CPU operating-point grants and the performance-mode service are in every image, and a session grants
# them (docs/designs/one-os-image-cpu.md). The admission record reads the CPU clock words once before the scheduler.
list(APPEND os_added apps/os/cpu.c drivers/clock/a733_cpu_clock.c apps/os/perf.c drivers/power/a733_perf_mode.c)
list(APPEND os_external_inputs "${os_root}/include/a733_cpu_clock.h" "${os_root}/include/a733_perf_mode.h"
  "${os_root}/include/a733_clock_reference.h")
if(SUNSTONE_A733_OS_GPU)
  set(os_recognition_core 6)
  if(SUNSTONE_GPU_TIMING)
    list(APPEND os_added apps/os/gpu-timing.c)
    if(SUNSTONE_GPU_NSI_PMU)
      list(APPEND os_added drivers/gpu/a733_gpu_nsi_pmu.c)
      list(APPEND os_external_inputs "${os_root}/include/a733_gpu_nsi_pmu.h")
    endif()
    list(APPEND os_external_inputs "${os_root}/include/gpu_queue_timing.h")
  endif()
  # The GPU runtime: the service, compute queue and the A733 platform,
  # backend and firmware memory, as the accelerator image composes them. The
  # service's compute, render and free-list objects come with it.
  list(APPEND os_added apps/os/gpu.c apps/os/gpu-share.c
    platform/a733/gpu_reserved.c platform/a733/gpu_queue.c platform/a733/gpu_compute_reserved.c
    platform/a733/gpu_graphics_memory.c platform/a733/gpu_memory.c platform/a733/gpu_vm_memory.c
    platform/a733/gpu_backend.c drivers/gpu/a733_gpu_power.c drivers/gpu/a733_gpu_platform.c
    drivers/gpu/a733_gpu_platform_native.c drivers/gpu/pvr_compute_mode.c drivers/gpu/pvr_compute_service.c
    drivers/gpu/pvr_compute_package.c drivers/gpu/pvr_compute_job.c drivers/gpu/pvr_compute_programs.c
    drivers/gpu/pvr_compute_context.c drivers/gpu/gpu_queue.c
    drivers/gpu/pvr_compute_queue.c drivers/gpu/pvr_compute_fast.c runtime/gpu_call.S drivers/gpu/pvr_cccb.c drivers/gpu/pvr_gpu_vm.c
    drivers/gpu/pvr_render_target.c drivers/gpu/pvr_render_programs.c drivers/gpu/pvr_render_draw.c
    drivers/gpu/pvr_render_texture.c drivers/gpu/pvr_render_context.c drivers/gpu/pvr_render_job.c
    drivers/gpu/pvr_render_service.c drivers/gpu/pvr_gpu_service.c drivers/gpu/pvr_gpu_flush.c drivers/gpu/pvr_firmware.c
    drivers/gpu/pvr_mips.c drivers/gpu/pvr_fw_state.c drivers/gpu/pvr_ccb.c drivers/gpu/pvr_gpu_start.c
    drivers/gpu/pvr_gpu_control.c drivers/gpu/pvr_gpu_stop.c drivers/gpu/pvr_free_list.c
    drivers/gpu/pvr_gpu_events.c)
  # The GPU service's performance-mode hooks, which the OS performance service drives.
  list(APPEND os_added drivers/gpu/a733_gpu_performance.c drivers/gpu/pvr_gpu_clock.c)
  file(WRITE "${os_build}/gpu-firmware.S"
    ".section .rodata.gpu_firmware,\"a\"\n.balign 16\n.global gpu_firmware\ngpu_firmware:\n.incbin \"${SUNSTONE_GPU_FIRMWARE}\"\n.global gpu_firmware_end\ngpu_firmware_end:\n")
  list(APPEND os_sources "${os_build}/gpu-firmware.S")
  file(GLOB os_gpu_headers "${os_root}/include/pvr_*.h" "${os_root}/include/a733_gpu_*.h")
  list(APPEND os_external_inputs "${SUNSTONE_GPU_FIRMWARE}" ${os_gpu_headers} "${os_root}/drivers/gpu/pvr_compute_probe.inc"
    "${os_root}/drivers/gpu/pvr_compute_usc.inc")
  list(APPEND os_include_dirs "${os_build}")
endif()
set(os_boot_final "${os_build}/boot.c")
if(SUNSTONE_A733_OS_CPU_RETURN)
  include("${os_root}/cmake/a733-os-cpu-return.cmake")
endif()
if(SUNSTONE_A733_OS_USB_JPEG)
  list(APPEND os_added apps/os/usb-jpeg-native.c apps/jpeg/input.c
    drivers/video/a733_video_jpeg.c drivers/video/a733_video_jpeg_run.c
    drivers/video/a733_video_platform.c drivers/video/a733_video_unit.c drivers/iommu/a733_iommu.c)
endif()
if(SUNSTONE_A733_CPU_TOPOLOGY)
  list(APPEND os_added kernel/scheduler/topology.c platform/a733/cpu_topology.c
    platform/a733/cpu_observation.c drivers/board/orangepi_zero3w.c
    drivers/clock/a733_cpu_clock.c)
  # Topology references the clock owner's read-only OPP tables even when the
  # clock service is disabled. Section GC drops its unused transition code.
endif()
if(SUNSTONE_A733_HW_FACTS)
  if(NOT SUNSTONE_A733_CPU_TOPOLOGY)
    message(FATAL_ERROR "The hardware facts probe runs at the CPU observation point; it needs SUNSTONE_A733_CPU_TOPOLOGY=ON")
  endif()
  list(APPEND os_added platform/a733/hw_facts.c platform/a733/hw_facts_a733.c)
endif()
list(TRANSFORM os_added PREPEND "${os_root}/")
option(SUNSTONE_PMU_ACCOUNT "Enable per-core PMU counters with EL0 read access for region accounting (needs SUNSTONE_IRQ_COUNT)" OFF)
option(SUNSTONE_BG_LOAD "Add the unpinned background load of the scheduler comparison (measurement)" OFF)
if(SUNSTONE_BG_LOAD)
  list(APPEND os_sources "${os_root}/runtime/bg_load.c")
endif()
option(SUNSTONE_LOCK_PREWAIT "Wait for the kernel task lock with the interrupt mask open before masking and queueing" OFF)
set(SUNSTONE_PREWAIT_DEPTH 1 CACHE STRING "Cores allowed to hold or wait for the task lock before an unmasked waiter masks")
set(SUNSTONE_PREWAIT_US 100 CACHE STRING "Longest unmasked wait for the task lock, in microseconds")
if(SUNSTONE_LOCK_PREWAIT)
  list(APPEND os_sources "${os_root}/runtime/lock_prewait.c")
endif()
option(SUNSTONE_CONTROL_IRQ "Hard real-time control interrupts above the kernel API mask (measurement)" OFF)
if(SUNSTONE_CONTROL_IRQ)
  list(APPEND os_sources "${os_root}/runtime/control_irq.c")
endif()
option(SUNSTONE_CONTROL_TASK_EL1 "Run the woken task of the control-interrupt measurement at EL1 (measurement)" OFF)
set(SUNSTONE_TLBI_ISSUER "" CACHE STRING "A task that issues broadcast TLB invalidations from one core (measurement): 0 wake only, 1 TLBI VAE1IS, 2 TLBI ASIDE1IS, 3 TLBI VMALLE1IS")
set(SUNSTONE_TLBI_PERIOD_TICKS "1" CACHE STRING "Kernel ticks between the invalidations of SUNSTONE_TLBI_ISSUER")
set(SUNSTONE_TLBI_CORE "5" CACHE STRING "The core that issues them")
if(NOT SUNSTONE_TLBI_ISSUER STREQUAL "")
  if(NOT SUNSTONE_TLBI_ISSUER MATCHES "^[0-3]$")
    message(FATAL_ERROR "SUNSTONE_TLBI_ISSUER is 0 to 3")
  endif()
  list(APPEND os_sources "${os_root}/apps/os/tlbi-issuer.c")
endif()
set(SUNSTONE_PULL_MOVES_PER_PASS 0 CACHE STRING "Idle-pull transfers one service pass may start (0 unlimited)")
option(SUNSTONE_OWNER_COUNTERS "Count owner-work drains and time the phases of the wake path (measurement)" OFF)
option(SUNSTONE_OBJECT_TRACE "Record who waits for and who holds the kernel object locks, by object and call site (measurement)" OFF)
option(SUNSTONE_IRQ_COUNT "Count interrupts per core and interrupt ID (measurement)" OFF)
if(SUNSTONE_IRQ_COUNT)
  list(APPEND os_sources "${os_root}/runtime/irq_count.c")
endif()
if(SUNSTONE_LATENCY)
  list(APPEND os_sources "${os_root}/runtime/latency.c" "${os_root}/runtime/latency_irq.c")
  list(APPEND os_external_inputs "${os_root}/include/sunstone_latency.h"
    "${os_root}/cmake/latency-instrumentation.cmake")
endif()
list(APPEND os_sources "${os_boot_final}" ${os_added})
list(REMOVE_DUPLICATES os_sources)
# Device implementations are separate object libraries. Each selected owner
# receives the same OS configuration; the core itself holds shared services.
set(os_service_targets)
foreach(service audio headset gpu npu radio)
  set(service_sources)
  foreach(source IN LISTS os_sources)
    set(selected FALSE)
    if(service STREQUAL "audio" AND source MATCHES "/(apps/os/audio\\.c|drivers/audio/|drivers/dma/|platform/a733/dma[^/]*)")
      set(selected TRUE)
    elseif(service STREQUAL "headset" AND source MATCHES "/(apps/os/headset\\.c|apps/bluetooth/|drivers/bluetooth/|drivers/uart/|bluetooth/|sbc/)")
      set(selected TRUE)
    elseif(service STREQUAL "gpu" AND source MATCHES "/(apps/os/gpu\\.c|drivers/gpu/pvr_|drivers/gpu/a733_gpu_(power|platform|performance)|platform/a733/gpu_(reserved|mimus|compute_reserved|graphics_memory|memory|vm_memory|backend)|gpu-firmware\\.S)")
      set(selected TRUE)
    elseif(service STREQUAL "npu" AND source MATCHES "/(apps/os/npu[^/]*|drivers/npu/|platform/a733/npu_native\\.c|npu-models\\.c|npu_admitted_models\\.c)")
      set(selected TRUE)
    elseif(service STREQUAL "radio" AND source MATCHES "/apps/os/radio[^/]*\\.c$")
      set(selected TRUE)
    endif()
    if(selected)
      list(APPEND service_sources "${source}")
    endif()
  endforeach()
  if(service_sources)
    list(REMOVE_ITEM os_sources ${service_sources})
    add_library(sunstone-a733-os-${service} OBJECT EXCLUDE_FROM_ALL ${service_sources})
    target_link_libraries(sunstone-a733-os-${service} PRIVATE sunstone-os-settings)
    list(APPEND os_external_inputs ${service_sources})
    if(service STREQUAL "headset")
      # Linked per image, by the application's manifest (cmake/sunstone-image.cmake), not into every image.
      target_compile_definitions(sunstone-a733-os-headset PRIVATE SUNSTONE_A733_OS_HEADSET=1
        SUNSTONE_BLUETOOTH_HEADSET=1 SUNSTONE_BLUETOOTH_CAPTURE_PROFILE=1 SUNSTONE_BLUETOOTH_MSBC=1)
      target_include_directories(sunstone-a733-os-headset BEFORE PRIVATE ${os_headset_include_dirs})
      set_source_files_properties(${os_sbc_sources} TARGET_DIRECTORY sunstone-a733-os-headset
        PROPERTIES COMPILE_DEFINITIONS SBC_HIGH_PRECISION=1)
    else()
      list(APPEND os_service_targets sunstone-a733-os-${service})
    endif()
  endif()
endforeach()
add_library(sunstone-a733-os-core OBJECT EXCLUDE_FROM_ALL ${os_sources})
# The loader's SD0 binding (apps/os/large_sd.c) holds three 128 KiB transfer buffers, so an image links it only when
# it reads or writes the card in large pieces: SUNSTONE_A733_OS_SD_WRITE, or an application whose manifest declares large
# regions (add_sunstone_image decides). Without it the loader's weak stubs report no device and authorize no pins.
add_library(sunstone-a733-os-sd OBJECT EXCLUDE_FROM_ALL "${os_root}/apps/os/large_sd.c")
target_link_libraries(sunstone-a733-os-sd PRIVATE sunstone-os-settings)
list(APPEND os_external_inputs "${os_root}/apps/os/large_sd.c")
add_library(sunstone-os-settings INTERFACE)
foreach(property COMPILE_OPTIONS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_FEATURES LINK_OPTIONS LINK_LIBRARIES)
  get_target_property(value sunstone-a733-board ${property})
  if(NOT value STREQUAL "value-NOTFOUND")
    set_property(TARGET sunstone-a733-os-core PROPERTY ${property} "${value}")
  endif()
endforeach()
if(SUNSTONE_A733_OS_USB_CLASSES)
  get_target_property(os_links sunstone-a733-os-core LINK_OPTIONS)
  list(FILTER os_links EXCLUDE REGEX "^-Wl,-T,|__uncached_pool_size")
  list(APPEND os_links "-Wl,-T,${os_build}/link-usb.ld")
  set_property(TARGET sunstone-a733-os-core PROPERTY LINK_OPTIONS "${os_links}")
endif()
get_target_property(os_includes sunstone-a733-os-core INCLUDE_DIRECTORIES)
list(REMOVE_ITEM os_includes "${os_root}/runtime/include" "${SUNSTONE_NETWORK_C_HEADERS}")
set_property(TARGET sunstone-a733-os-core PROPERTY INCLUDE_DIRECTORIES "${os_includes}")
sunstone_smp_compile(sunstone-a733-os-core)
get_target_property(os_definitions sunstone-a733-os-core COMPILE_DEFINITIONS)
list(FILTER os_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-os-core PROPERTY COMPILE_DEFINITIONS "${os_definitions}")
if(SUNSTONE_A733_OS_USB_JPEG)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_USB_JPEG=1)
endif()
if(SUNSTONE_A733_OS_USB_CLASSES)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_USB_CLASSES=1 SUNSTONE_USB_HOST_GENERIC=1 SUNSTONE_USB_APP_ALIAS=1)
  if(NOT SUNSTONE_A733_OS_USB_JPEG)
    # The retained JPEG adapter still needs owner passes for setup and copied
    # readout. Enable its precise wait only after IRQ81 and readiness are wired.
    target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_BOARD_HOST_DEADLINE=1)
  endif()
endif()
target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_BOARD_APPLICATION=1 SUNSTONE_BOARD_USB2_DUAL=1 SUNSTONE_A733_OS=1 SUNSTONE_TASK_DEADLINES=1 SUNSTONE_A733_IRQ_TABLE=1 SUNSTONE_A733_FAULT_PAGE=1
  "$<$<BOOL:${SUNSTONE_A733_OS_CPU_RETURN}>:SUNSTONE_A733_OS_CPU_RETURN=1>"
  "$<$<BOOL:${SUNSTONE_A733_OS_CPU_RETURN}>:SUNSTONE_CRUMBS=1>"
  "$<$<BOOL:${SUNSTONE_GPU_FAST_TIMING}>:SUNSTONE_GPU_FAST_TIMING=1>"
  A733_SMP_CONTRACT_MAGIC=${os_contract_magic} A733_SMP_RESULT_MAGIC=${os_result_magic}
  "$<$<BOOL:${SUNSTONE_A733_OS_RESIDENT}>:SUNSTONE_BOARD_RESIDENT=1>"
  "$<$<BOOL:${SUNSTONE_A733_OS_RADIO_SCAN}>:SUNSTONE_A733_OS_RADIO_SCAN=1>")
# The EL0 ring is linked too; the application's declaration selects it (apps/os/npu-session.h, os_npu_session_queued).
target_sources(sunstone-a733-os-core PRIVATE "${os_root}/runtime/npu_app_queue.c" "${os_root}/apps/os/npu-queue.c")
if(SUNSTONE_A733_OS_GPU)
  if(SUNSTONE_A733_OS_CPU_RETURN)
    target_compile_definitions(sunstone-a733-os-core PRIVATE INCLUDE_eTaskGetState=1)
  endif()
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_GPU_LINEAR=1)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_GPU=1 ${os_gpu_bisect_definitions})
  if(SUNSTONE_A733_OS_GPU_RECOVERY_TEST)
    target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_GPU_RECOVERY_TEST=1)
  endif()
  set_source_files_properties("${os_root}/drivers/gpu/a733_gpu_platform_native.c" TARGET_DIRECTORY sunstone-a733-os-core
    PROPERTIES COMPILE_OPTIONS "-iquote${os_root}/platform/a733")
endif()
target_include_directories(sunstone-a733-os-core BEFORE PRIVATE "${os_root}/apps/os" "${os_build}")
if(SUNSTONE_A733_CPU_TOPOLOGY)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_CPU_TOPOLOGY=1)
endif()
if(SUNSTONE_A733_HW_FACTS)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_HW_FACTS=1)
endif()
if(os_include_dirs)
  target_include_directories(sunstone-a733-os-core PRIVATE ${os_include_dirs})
endif()
target_compile_definitions(sunstone-a733-os-core PRIVATE PVR_GPU_PERFORMANCE=1)
get_target_property(os_link sunstone-a733-os-core LINK_OPTIONS)
list(FILTER os_link EXCLUDE REGEX "Map,")
set_property(TARGET sunstone-a733-os-core PROPERTY LINK_OPTIONS "${os_link}")
target_link_options(sunstone-a733-os-core PRIVATE "-Wl,-Map,${os_build}/image.map")
# The loader's SD0 binding is read-only: the core has no write path (checks/sd-read-only.py scans the image).
if(SUNSTONE_A733_OS_SD_WRITE)
  # The write-capable build: the SD core keeps its write path, and the loader's binding and the owner task know it.
  set_source_files_properties("${os_root}/apps/os/large_sd.c" "${os_root}/apps/os/sd_owner.c" PROPERTIES COMPILE_DEFINITIONS OS_SD_WRITE=1)
  # The bounded writer's rule in the native register filter exists only where the bounds object is linked.
  set_source_files_properties("${os_root}/drivers/storage/a733_mmc_platform_native.c" PROPERTIES COMPILE_DEFINITIONS A733_MMC_BOUNDED_WRITER=1)
else()
  set_source_files_properties("${os_root}/drivers/storage/sunxi_mmc.c" "${os_root}/drivers/storage/a733_mmc_platform.c"
    "${os_root}/drivers/storage/a733_mmc_platform_native.c" PROPERTIES COMPILE_DEFINITIONS SUNXI_MMC_READ_ONLY=1)
endif()
if(SUNSTONE_CONTROL_IRQ)
  if(SUNSTONE_LATENCY OR SUNSTONE_IRQ_COUNT)
    message(FATAL_ERROR "The control interrupts, the latency recorder and the interrupt counter each wrap vApplicationIRQHandler")
  endif()
  target_link_options(sunstone-a733-os-core PRIVATE -Wl,--wrap=vApplicationIRQHandler)
  set_source_files_properties("${os_root}/runtime/control_irq.c" PROPERTIES COMPILE_OPTIONS -mgeneral-regs-only)
endif()
if(SUNSTONE_LOCK_PREWAIT)
  target_link_options(sunstone-a733-os-core PRIVATE -Wl,--wrap=uxPortSetInterruptMask -Wl,--wrap=vTaskEnterCritical -Wl,--wrap=vTaskSuspendAll)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_PREWAIT_DEPTH=${SUNSTONE_PREWAIT_DEPTH}u SUNSTONE_PREWAIT_US=${SUNSTONE_PREWAIT_US}u)
  set_source_files_properties("${os_root}/runtime/lock_prewait.c" PROPERTIES COMPILE_OPTIONS -mgeneral-regs-only)
endif()
if(SUNSTONE_OBJECT_TRACE)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_OBJECT_TRACE=1)
endif()
target_compile_definitions(sunstone-a733-os-core PRIVATE SS_PULL_MOVES_PER_PASS=${SUNSTONE_PULL_MOVES_PER_PASS}u)
if(SUNSTONE_OWNER_COUNTERS)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_OWNER_COUNTERS=1)
endif()
if(SUNSTONE_CONTROL_TASK_EL1)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_CONTROL_TASK_EL1=1)
  set_source_files_properties("${os_root}/runtime/control_irq.c" PROPERTIES COMPILE_DEFINITIONS SUNSTONE_CONTROL_TASK_EL1=1)
endif()
if(SUNSTONE_IRQ_COUNT)
  if(SUNSTONE_LATENCY)
    message(FATAL_ERROR "The interrupt counter and the latency recorder both wrap vApplicationIRQHandler")
  endif()
  target_link_options(sunstone-a733-os-core PRIVATE -Wl,--wrap=vApplicationIRQHandler)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_IRQ_COUNT=1)
  if(SUNSTONE_PMU_ACCOUNT)
    target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_PMU_ACCOUNT=1)
  endif()
endif()
if(SUNSTONE_LATENCY_SWEEP AND NOT SUNSTONE_LATENCY)
  message(FATAL_ERROR "The span sweep needs the latency recorder")
endif()
if(SUNSTONE_LATENCY)
  if(SUNSTONE_LATENCY_SWEEP)
    target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_LATENCY_SWEEP=1)
  endif()
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_LATENCY=1)
  if(SUNSTONE_LATENCY_PMU)
    target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_LATENCY_PMU=1)
  endif()
  target_link_options(sunstone-a733-os-core PRIVATE -Wl,--wrap=vApplicationIRQHandler)
  sunstone_enable_latency_port(sunstone-a733-os-core)
  get_target_property(os_sources sunstone-a733-os-core SOURCES)
  get_target_property(latency_port_inputs sunstone-a733-os-core SUNSTONE_LATENCY_PORT_INPUTS)
  list(APPEND os_external_inputs ${latency_port_inputs})
endif()
if(NOT SUNSTONE_TLBI_ISSUER STREQUAL "")
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_TLBI_ISSUER=${SUNSTONE_TLBI_ISSUER}
    SUNSTONE_TLBI_PERIOD_TICKS=${SUNSTONE_TLBI_PERIOD_TICKS} SUNSTONE_TLBI_CORE=${SUNSTONE_TLBI_CORE})
endif()
if(SUNSTONE_A733_OS_SD_WRITE)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_SD_WRITE=1)
endif()
if(SUNSTONE_A733_OS_DMA)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_DMA=1)
endif()
# Every function of the OS, its device objects and the application starts on a 16-byte boundary. Otherwise a function's
# address modulo 8 depends on the size of all code linked before it, so an unrelated OS change can move an application's
# hot loops to the other alignment and change their speed on the Cortex-A55. The NPU encoder's scalar conversion loops
# ran 25% slower with its conversion function at 4 modulo 8 than at 0.
target_compile_options(sunstone-a733-os-core PRIVATE -falign-functions=16)
# Publish the exact OS compile configuration to application and device objects.
foreach(property COMPILE_OPTIONS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_FEATURES)
  get_target_property(value sunstone-a733-os-core ${property})
  if(NOT value STREQUAL "value-NOTFOUND")
    set_property(TARGET sunstone-os-settings PROPERTY INTERFACE_${property} "${value}")
  endif()
endforeach()
# Keep libraries with their existing device owner. The main core is built even
# when no application is selected; descriptors and result storage belong to images.
file(GLOB os_headers "${os_root}/apps/os/*.h")
if(SUNSTONE_A733_OS_USB_CLASSES)
  list(APPEND os_headers "${os_root}/include/a733_usb_memory.h" "${os_root}/include/sunstone_usb_service.h"
    "${os_root}/include/sunstone_usb_hid.h" "${os_root}/include/sunstone_usb_serial.h"
    "${os_root}/include/sunstone_usb_uvc.h" "${os_root}/include/sunstone_usb_transport.h")
endif()
if(SUNSTONE_A733_OS_USB_JPEG)
  list(APPEND os_headers "${os_root}/apps/jpeg/native-shared.h" "${os_root}/include/sunstone_usb_jpeg.h")
endif()
set(os_inputs ${board_inputs} ${os_sources} ${os_headers} ${os_generated} ${os_patches} ${os_external_inputs}
  "${os_root}/platform/a733-smp-cached/irq_table.h" "${os_root}/include/a733_fault.h"
  "${os_root}/include/a733_thermal.h" "${os_root}/include/a733_thermal_native.h"
  "${os_root}/include/gicv3_spi.h" "${os_root}/include/sunstone_deadline.h" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_ITEM os_inputs "${board_build}/build-id.c")
list(REMOVE_DUPLICATES os_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${os_inputs})
set_property(TARGET sunstone-a733-os-core PROPERTY SUNSTONE_INPUTS "${os_inputs}")
if(SUNSTONE_A733_OS_USB_CLASSES)
  set_property(TARGET sunstone-a733-os-core PROPERTY SUNSTONE_LINK_SCRIPT "${os_build}/link-usb.ld")
else()
  set_property(TARGET sunstone-a733-os-core PROPERTY SUNSTONE_LINK_SCRIPT "${storage_build}/link.ld")
endif()
set_property(TARGET sunstone-a733-os-core PROPERTY SUNSTONE_DEVICE_OBJECTS "${os_service_targets}")

if(SUNSTONE_GPU_TIMING)
  target_compile_definitions(sunstone-a733-os-core PUBLIC SUNSTONE_GPU_TIMING=1)
  target_compile_definitions(sunstone-a733-os-gpu PRIVATE SUNSTONE_GPU_TIMING=1)
  if(SUNSTONE_GPU_NSI_PMU)
    target_compile_definitions(sunstone-a733-os-core PUBLIC SUNSTONE_GPU_NSI_PMU=1)
    target_compile_definitions(sunstone-a733-os-gpu PRIVATE SUNSTONE_GPU_NSI_PMU=1)
  endif()
  foreach(symbol gpu_queue_submit gpu_queue_wait pvr_cccb_begin pvr_cccb_poll pvr_ccb_submit aarch64_device_cache xQueueReceive)
    target_link_options(sunstone-a733-os-core INTERFACE "-Wl,--wrap=${symbol}")
  endforeach()
endif()
