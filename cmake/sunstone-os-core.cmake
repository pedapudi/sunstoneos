# OS implementation and device owners, independent of application sources.
include("${CMAKE_CURRENT_LIST_DIR}/latency-instrumentation.cmake")
option(SUNSTONE_A733_OS "Build the integrated OS image" OFF)
option(SUNSTONE_A733_OS_RESIDENT "Integrated OS with the resident lifetime" OFF)
option(SUNSTONE_A733_OS_USB_CLASSES "OS input, serial and camera host services" OFF)
option(SUNSTONE_A733_OS_USB_JPEG "OS camera MJPEG hardware decoder" OFF)
option(SUNSTONE_A733_OS_AUDIO "OS I2S capture service" OFF)
option(SUNSTONE_A733_OS_DMA "Adopted DMA0 RAM-copy service with normal retirement" OFF)
option(SUNSTONE_A733_OS_RADIO "Integrated OS with the board's radio station" OFF)
option(SUNSTONE_A733_OS_HEADSET "OS Bluetooth headset capture service" OFF)
option(SUNSTONE_A733_OS_MSBC "Integrated OS headset with wideband mSBC speech" OFF)
option(SUNSTONE_A733_OS_NPU "Integrated OS with a task-bound NPU catalogue service" OFF)
option(SUNSTONE_A733_OS_NPU_OBSERVE "Opt-in serialized NPU diagnostic sampling" OFF)
option(SUNSTONE_A733_OS_NPU_QUEUE "EL0 resident NPU submission ring on the routed core" OFF)
set(SUNSTONE_OS_NPU_QUEUE_MODELS "" CACHE STRING "Comma-separated resident model indices, client model first")
if(SUNSTONE_A733_OS_NPU_QUEUE AND NOT SUNSTONE_A733_OS_NPU)
  message(FATAL_ERROR "The NPU queue requires SUNSTONE_A733_OS_NPU=ON")
endif()
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
if(SUNSTONE_A733_OS_NPU_OBSERVE AND
   (NOT SUNSTONE_A733_OS_NPU OR NOT SUNSTONE_A733_OS_CPU_RETURN))
  message(FATAL_ERROR "NPU diagnostic sampling requires returning NPU composition")
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
if(SUNSTONE_A733_OS_DMA AND (SUNSTONE_A733_OS_AUDIO OR NOT SUNSTONE_A733_OS_CPU_RETURN))
  message(FATAL_ERROR "Adopted RAM DMA currently requires the returning CPU profile without I2S")
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
if(SUNSTONE_A733_OS_USB_JPEG AND
   (SUNSTONE_A733_OS_AUDIO OR SUNSTONE_A733_OS_RADIO OR SUNSTONE_A733_OS_HEADSET OR
    SUNSTONE_A733_OS_NPU OR SUNSTONE_A733_OS_GPU_LINEAR))
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
if(SUNSTONE_A733_OS_HEADSET AND SUNSTONE_A733_OS_AUDIO)
  message(FATAL_ERROR "An image selects one capture source: I2S or the headset")
endif()
if(SUNSTONE_A733_OS_MSBC AND NOT SUNSTONE_A733_OS_HEADSET)
  message(FATAL_ERROR "Wideband mSBC speech is a headset option")
endif()
# A76 admission permits application placement on cores 6 and 7. Device owners
# retain their existing A55 affinity.
option(A733_A76_ADMITTED "Admit Cortex-A76 cores for application placement" OFF)
# CPU operating points granted by the sealed supervised or resident session
# policy (docs/cpu-clock.md).
option(SUNSTONE_A733_OS_CPU_CLOCK "Integrated OS with granted CPU operating-point changes" OFF)
if(SUNSTONE_A733_OS_CPU_CLOCK AND SUNSTONE_A733_OS_RESIDENT AND
   (SUNSTONE_A733_OS_AUDIO OR SUNSTONE_A733_OS_RADIO OR SUNSTONE_A733_OS_HEADSET))
  message(FATAL_ERROR "Resident CPU grants require the recorded-card session profile")
endif()
if(SUNSTONE_A733_OS_CPU_CLOCK AND NOT A733_A76_ADMITTED)
  message(FATAL_ERROR "CPU operating-point grants need A733_A76_ADMITTED")
endif()
# High and low performance modes (docs/designs/performance-modes.md): the
# thermal task's CPU clock owner switches the CPU domains, and the NPU when
# the image has it, between the speed bin's vendor points. The boot mode is
# the image manifest's perf_boot_mode.
option(SUNSTONE_A733_OS_PERF "Integrated OS with the high and low performance-mode service" OFF)
set(SUNSTONE_A733_OS_PERF_BOOT "high" CACHE STRING "Performance mode at boot: high or low")
if(SUNSTONE_A733_OS_PERF AND NOT SUNSTONE_A733_OS_CPU_CLOCK)
  message(FATAL_ERROR "The performance-mode service runs in the CPU clock owner; it needs SUNSTONE_A733_OS_CPU_CLOCK")
endif()
if(NOT SUNSTONE_A733_OS_PERF_BOOT STREQUAL "high" AND NOT SUNSTONE_A733_OS_PERF_BOOT STREQUAL "low")
  message(FATAL_ERROR "SUNSTONE_A733_OS_PERF_BOOT is high or low")
endif()
if(SUNSTONE_A733_OS_NPU)
  # CPU grants compose with recorded resident/supervised GPU/NPU, or performance modes.
  if(SUNSTONE_A733_OS_AUDIO OR SUNSTONE_A733_OS_RADIO OR
     SUNSTONE_A733_OS_HEADSET OR (SUNSTONE_A733_OS_CPU_CLOCK AND NOT SUNSTONE_A733_OS_PERF AND NOT SUNSTONE_A733_OS_RESIDENT AND NOT SUNSTONE_A733_OS_GPU AND NOT SUNSTONE_A733_OS_CPU_RETURN))
    message(FATAL_ERROR "The NPU OS profile requires recorded recognition and a supported GPU/CPU composition")
  endif()
  set(SUNSTONE_OS_NPU_CATALOGUE "" CACHE FILEPATH "Authenticated NPU artifacts map for apps/os/npu-models.json")
  set(SUNSTONE_OS_NPU_MODELS "${SUNSTONE_SOURCE_ROOT}/apps/os/npu-models.json" CACHE FILEPATH "Application NPU model-list data")
  set(SUNSTONE_OS_NPU_CLIENT_MODEL "0" CACHE STRING "Legacy numeric selection within the model-list data")
  set(SUNSTONE_OS_NPU_MODEL_NAME "" CACHE STRING "Semantic name of the selected linked NPU model")
  option(SUNSTONE_OS_NPU_ALL_MODELS "Link every entry in the selected NPU model list" OFF)
  if(NOT EXISTS "${SUNSTONE_OS_NPU_MODELS}")
    message(FATAL_ERROR "OS NPU requires SUNSTONE_OS_NPU_MODELS model-list data")
  endif()
  if(NOT EXISTS "${SUNSTONE_OS_NPU_CATALOGUE}")
    message(FATAL_ERROR "OS NPU requires SUNSTONE_OS_NPU_CATALOGUE")
  endif()
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
  # GPU replies retain bounded failure handling for the recorded two- and
  # eight-worker compositions.
  if(NOT SUNSTONE_A733_OS_RESIDENT AND
     (SUNSTONE_A733_OS_AUDIO OR SUNSTONE_A733_OS_RADIO OR SUNSTONE_A733_OS_HEADSET))
    message(FATAL_ERROR "Supervised GPU linear requires recorded recognition without capture/radio/headset")
  endif()
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
  # The armed board boot reads the watchdog, redirect and FEL registers back
  # against the contract; the resident contract arms the first and zeroes the rest.
  # Card reset uses the normal ROM boot selection, without a diagnostic
  # requirement to hold BOOT low. All contract/WDT readbacks remain intact.
  apply_build_patch("${os_build}/boot-resident.c" "${board_build}/boot.c"
    "${os_root}/apps/os/boot-resident.patch")
  set(os_boot_base "${os_build}/boot-resident.c")
  set(os_contract_magic 0x415337334f524331ULL)
  set(os_result_magic 0x415337334f525231ULL)
else()
  set(os_boot_base "${board_build}/boot-supervised.c")
  set(os_contract_magic 0x415337334f534331ULL)
  set(os_result_magic 0x415337334f535231ULL)
endif()
apply_build_patch("${os_build}/boot.c" "${os_boot_base}" "${os_root}/apps/os/boot.patch")
# Retained per-core fault records and their previous-boot report
# (docs/fault-capture.md); SUNSTONE_A733_FAULT_PAGE enables them.
set(os_added apps/os/admission.c apps/os/identity.c apps/os/irq.c apps/os/services.c apps/os/start.c
  runtime/cpu_topology_app.c
  apps/os/usb-service.c apps/os/application.c apps/os/session.c apps/os/usb0.c apps/os/client.c apps/os/capture.c
  runtime/deadline.c runtime/deadline-a733.c
  runtime/newlib.c runtime/newlib_smp.c runtime/newlib_syscalls_smp.c runtime/new_delete.cc runtime/cxx_terminate.cc
  apps/os/thermal.c apps/os/watchdog.c platform/a733-smp-cached/irq_table.c drivers/thermal/a733_thermal.c
  drivers/thermal/a733_thermal_native.c drivers/gpu/gicv3_spi.c platform/a733/fault.S platform/a733/fault.c)
set(os_patches "${os_root}/apps/os/boot.patch")
if(SUNSTONE_A733_OS_USB_CLASSES)
  apply_build_patch("${os_build}/link-usb.ld" "${storage_build}/link.ld"
    "${os_root}/apps/os/usb-memory-link.patch")
  list(APPEND os_patches "${os_root}/apps/os/usb-memory-link.patch")
endif()
if(SUNSTONE_A733_OS_RESIDENT)
  list(APPEND os_patches "${os_root}/apps/os/boot-resident.patch")
endif()
set(os_generated "${os_build}/boot.c")
if(SUNSTONE_A733_OS_RADIO)
  # The board's own radio composition serves workload 1; its owner and client
  # report resident progress through the patched main.c.
  list(APPEND os_added apps/os/radio.c)
  apply_build_patch("${os_build}/main.c" "${os_root}/apps/board/main.c" "${os_root}/apps/os/main.patch")
  list(REMOVE_ITEM os_sources "${os_root}/apps/board/main.c")
  list(APPEND os_sources "${os_build}/main.c")
  list(APPEND os_patches "${os_root}/apps/os/main.patch")
  list(APPEND os_generated "${os_build}/main.c")
  if(SUNSTONE_A733_OS_RESIDENT)
    list(REMOVE_ITEM os_sources "${os_root}/apps/board/radio_profile.c")
    list(APPEND os_added apps/os/radio_profile.c)
  endif()
endif()
if(SUNSTONE_A733_OS_DMA)
  list(APPEND os_added apps/os/dma.c platform/a733/dma_pool_session.c platform/a733/dma_startup.c
    platform/a733/dma_native.c drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
    drivers/dma/a733_dma_platform.c drivers/dma/a733_dma_platform_native.c drivers/dma/dma_service.c
    drivers/dma/dma_service_i2s.c drivers/dma/dma_service_i2s_stream.c drivers/audio/a733_i2s.c
    drivers/audio/a733_i2s_stream.c drivers/audio/dma_service_i2s_irq.c)
endif()
if(SUNSTONE_A733_OS_AUDIO)
  # The I2S image's receive composition (cmake/a733-i2s-application.cmake).
  list(APPEND os_added apps/os/audio.c platform/a733/dma_pool_session.c platform/a733/dma_startup.c
    platform/a733/dma_native.c drivers/dma/dma_memory.c drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c
    drivers/dma/a733_dma_platform.c drivers/dma/a733_dma_platform_native.c drivers/dma/dma_service.c
    drivers/dma/dma_service_i2s.c drivers/dma/dma_service_i2s_stream.c drivers/audio/a733_i2s.c
    drivers/audio/a733_i2s_stream.c drivers/audio/a733_i2s_platform.c drivers/audio/a733_i2s_platform_native.c
    drivers/audio/a733_audio_leases.c drivers/audio/dma_service_i2s_irq.c)
endif()
if(SUNSTONE_A733_OS_NPU)
  set(os_npu_select --model "${SUNSTONE_OS_NPU_CLIENT_MODEL}")
  if(SUNSTONE_OS_NPU_MODEL_NAME)
    set(os_npu_select --model-name "${SUNSTONE_OS_NPU_MODEL_NAME}")
  endif()
  if(SUNSTONE_OS_NPU_ALL_MODELS)
    list(APPEND os_npu_select --all)
  endif()
  if(SUNSTONE_A733_OS_NPU_QUEUE AND SUNSTONE_OS_NPU_QUEUE_MODELS)
    list(APPEND os_npu_select --queue-models "${SUNSTONE_OS_NPU_QUEUE_MODELS}")
  endif()
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${os_root}/apps/os/npu-catalogue.py"
    --artifacts "${SUNSTONE_OS_NPU_CATALOGUE}" --models "${SUNSTONE_OS_NPU_MODELS}" ${os_npu_select}
    --output "${os_build}/npu-models.c" COMMAND_ERROR_IS_FATAL ANY)
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${os_root}/tools/npu-session-catalogue.py" generate
    --application "${SUNSTONE_OS_NPU_MODELS}" --output "${os_build}/npu_admitted_models.c"
    COMMAND_ERROR_IS_FATAL ANY)
  list(APPEND os_sources "${os_build}/npu-models.c" "${os_build}/npu_admitted_models.c")
  list(APPEND os_added apps/os/npu.c apps/os/npu-client.c apps/os/npu-irq.c
    platform/a733/npu_native.c runtime/counter_clock.c drivers/npu/a733_npu_runtime.c drivers/npu/a733_npu_runtime_interim.c
    drivers/npu/npu_service.c drivers/npu/npu_queue.c drivers/npu/npu_runtime_queue.c drivers/npu/npu_idle.c drivers/npu/a733_npu_sequence.c
    drivers/npu/a733_npu_controller.c drivers/npu/a733_npu_memory.c
    drivers/npu/a733_npu_mmu.c drivers/npu/nbg_model.c drivers/npu/nbg_assembly.c drivers/npu/nbg_checksum.c
    drivers/npu/nbg_relocation.c drivers/npu/a733_npu_startup.c drivers/npu/a733_npu_platform.c
    drivers/npu/a733_npu_platform_native.c)
  if(SUNSTONE_A733_OS_CPU_RETURN)
    list(APPEND os_added apps/os/npu-observe.c)
  endif()
  file(GLOB os_npu_headers "${os_root}/include/*npu*.h" "${os_root}/include/nbg*.h")
  list(APPEND os_external_inputs ${os_npu_headers} "${os_root}/apps/os/npu-catalogue.py"
    "${os_root}/platform/a733/npu_irq.c" "${os_root}/include/a733_npu_irq.h"
    "${os_root}/tools/npu-session-models.json" "${os_root}/tools/npu-session-catalogue.py"
    "${SUNSTONE_OS_NPU_MODELS}" "${os_root}/drivers/npu/nbg_command.h" "${os_build}/npu-models.c"
    "${os_build}/npu_admitted_models.c" "${SUNSTONE_OS_NPU_CATALOGUE}")
endif()
set(os_include_dirs)
if(SUNSTONE_A733_OS_HEADSET)
  # The Bluetooth capture provider shares the radio owner when selected.
  set(SUNSTONE_BT_SECURITY_DIR "" CACHE PATH "Pinned Zephyr BR/EDR security source directory")
  if(NOT SUNSTONE_BT_SECURITY_DIR)
    message(FATAL_ERROR "The headset build requires SUNSTONE_BT_SECURITY_DIR")
  endif()
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
  list(APPEND os_include_dirs "${os_bluetooth}")
  if(SUNSTONE_A733_OS_MSBC)
    # The Bluetooth image's mSBC codec (cmake/a733-bluetooth.cmake) with its
    # pinned libsbc sources, checked against apps/bluetooth/msbc-source.json.
    set(SUNSTONE_BT_SBC_DIR "" CACHE PATH "Pinned BlueZ libsbc2.0 source directory")
    if(NOT SUNSTONE_BT_SBC_DIR)
      message(FATAL_ERROR "The mSBC headset build requires SUNSTONE_BT_SBC_DIR")
    endif()
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
    list(APPEND os_added apps/bluetooth/msbc.c apps/bluetooth/playback.c)
    set(os_sbc_sources "${SUNSTONE_BT_SBC_DIR}/sbc/sbc.c" "${SUNSTONE_BT_SBC_DIR}/sbc/sbc_primitives.c")
    list(APPEND os_sources ${os_sbc_sources})
    list(APPEND os_include_dirs "${SUNSTONE_BT_SBC_DIR}")
  endif()
endif()
if(A733_A76_ADMITTED)
  # The admission record reads the CPU clock words once before the scheduler.
  list(APPEND os_added apps/os/cpu.c drivers/clock/a733_cpu_clock.c)
  list(APPEND os_external_inputs "${os_root}/include/a733_cpu_clock.h")
endif()
if(SUNSTONE_A733_OS_PERF)
  list(APPEND os_added apps/os/perf.c drivers/power/a733_perf_mode.c)
  list(APPEND os_external_inputs "${os_root}/include/a733_perf_mode.h" "${os_root}/include/a733_clock_reference.h")
endif()
if(SUNSTONE_A733_OS_GPU)
  if(A733_A76_ADMITTED)
    set(os_recognition_core 6)
  else()
    set(os_recognition_core 2)
  endif()
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
  if(SUNSTONE_A733_OS_PERF)
    list(APPEND os_added drivers/gpu/a733_gpu_performance.c drivers/gpu/pvr_gpu_clock.c)
  endif()
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
    list(APPEND os_service_targets sunstone-a733-os-${service})
    list(APPEND os_external_inputs ${service_sources})
  endif()
endforeach()
add_library(sunstone-a733-os-core OBJECT EXCLUDE_FROM_ALL ${os_sources})
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
  "$<$<BOOL:${SUNSTONE_A733_OS_AUDIO}>:SUNSTONE_A733_OS_AUDIO=1>"
  "$<$<BOOL:${SUNSTONE_A733_OS_RADIO}>:SUNSTONE_A733_OS_RADIO=1>"
  "$<$<BOOL:${SUNSTONE_A733_OS_HEADSET}>:SUNSTONE_A733_OS_HEADSET=1>")
if(SUNSTONE_A733_OS_NPU)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_NPU=1)
  if(SUNSTONE_A733_OS_NPU_OBSERVE)
    target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_NPU_OBSERVE=1)
  endif()
  if(SUNSTONE_A733_OS_NPU_QUEUE)
    target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_NPU_QUEUE=1)
    target_sources(sunstone-a733-os-core PRIVATE "${os_root}/runtime/npu_app_queue.c" "${os_root}/apps/os/npu-queue.c")
  endif()
endif()
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
if(SUNSTONE_A733_OS_HEADSET)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_BLUETOOTH_HEADSET=1 SUNSTONE_BLUETOOTH_CAPTURE_PROFILE=1)
endif()
if(A733_A76_ADMITTED)
  target_compile_definitions(sunstone-a733-os-core PRIVATE A733_A76_ADMITTED=1
    "$<$<BOOL:${SUNSTONE_A733_OS_CPU_CLOCK}>:SUNSTONE_A733_OS_CPU_CLOCK=1>")
endif()
if(SUNSTONE_A733_OS_PERF)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_PERF=1
    OS_PERF_BOOT_MODE=$<IF:$<STREQUAL:${SUNSTONE_A733_OS_PERF_BOOT},low>,2,1>)
endif()
if(SUNSTONE_A733_OS_MSBC)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_MSBC=1 SUNSTONE_BLUETOOTH_MSBC=1)
  set_source_files_properties(${os_sbc_sources} TARGET_DIRECTORY sunstone-a733-os-core
    PROPERTIES COMPILE_DEFINITIONS SBC_HIGH_PRECISION=1)
endif()
get_target_property(os_link sunstone-a733-os-core LINK_OPTIONS)
list(FILTER os_link EXCLUDE REGEX "Map,")
if(SUNSTONE_A733_OS_NPU)
  # A fixed private reservation, not an expansion of the board's local pool.
  if(SUNSTONE_A733_OS_USB_CLASSES)
    file(READ "${os_build}/link-usb.ld" os_npu_link)
  else()
    file(READ "${storage_build}/link.ld" os_npu_link)
  endif()
  string(APPEND os_npu_link "\n__os_npu_pool_start = 0x47400000;\n__os_npu_pool_end = 0x47c00000;\n")
  file(WRITE "${os_build}/link-npu.ld" "${os_npu_link}")
  list(FILTER os_link EXCLUDE REGEX "-T,")
  list(APPEND os_link "-Wl,-T,${os_build}/link-npu.ld")
  list(APPEND os_generated "${os_build}/link-npu.ld")
endif()
set_property(TARGET sunstone-a733-os-core PROPERTY LINK_OPTIONS "${os_link}")
target_link_options(sunstone-a733-os-core PRIVATE "-Wl,-Map,${os_build}/image.map")
if(SUNSTONE_LATENCY)
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
if(SUNSTONE_A733_OS_DMA)
  target_compile_definitions(sunstone-a733-os-core PRIVATE SUNSTONE_A733_OS_DMA=1)
endif()
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
if(SUNSTONE_A733_OS_NPU)
  set_property(TARGET sunstone-a733-os-core PROPERTY SUNSTONE_LINK_SCRIPT "${os_build}/link-npu.ld")
elseif(SUNSTONE_A733_OS_USB_CLASSES)
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
