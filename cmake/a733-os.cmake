# Select one application of the native OS core. An empty selection builds the
# same OS services with no application task or application manifest.
set(SUNSTONE_APPLICATION "" CACHE STRING "Registered OS application name; empty builds only OS services")
include("${CMAKE_CURRENT_LIST_DIR}/sunstone-os-core.cmake")
if(NOT TARGET sunstone-a733-os-core)
  return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/sunstone-image.cmake")
# Every image admits the returning lifetime, except one with the USB class services, and selects its lifetime at run
# time from the contract magic (apps/os/admission.c); the manifest bounds the supervised and returning ones.
set(os_lifetime return)
if(SUNSTONE_A733_OS_USB_CLASSES)
  set(os_lifetime supervised)
endif()
if(SUNSTONE_APPLICATION STREQUAL "")
  add_sunstone_image(sunstone-a733-os LIFETIME ${os_lifetime})
elseif(SUNSTONE_APPLICATION STREQUAL "hello-compute")
  set(SUNSTONE_HELLO_CLUSTER "a55" CACHE STRING "CPU-return control placement: a55 or a76")
  if(NOT SUNSTONE_HELLO_CLUSTER MATCHES "^(a55|a76)$")
    message(FATAL_ERROR "SUNSTONE_HELLO_CLUSTER must be a55 or a76")
  endif()
  set(hello_manifest "${SUNSTONE_SOURCE_ROOT}/apps/hello-compute/manifest.json")
  if(SUNSTONE_HELLO_CLUSTER STREQUAL "a76")
    set(hello_manifest "${SUNSTONE_SOURCE_ROOT}/apps/hello-compute/manifest-a76.json")
  endif()
  add_sunstone_application(hello-compute
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/hello-compute/main.c"
    MANIFEST "${hello_manifest}")
  add_sunstone_image(sunstone-a733-os-hello LIFETIME ${os_lifetime} APPLICATION hello-compute)
  if(SUNSTONE_PMU_RESULT)
    target_compile_definitions(sunstone-a733-os-hello PRIVATE SUNSTONE_PMU_HELLO=1)
    target_link_options(sunstone-a733-os-hello PRIVATE -Wl,--wrap=hello_compute_main)
  endif()
elseif(SUNSTONE_APPLICATION STREQUAL "memcpy-bench")
  add_sunstone_application(memcpy-bench
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/memcpy-bench/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/memcpy-bench/manifest.json")
  add_sunstone_image(sunstone-a733-os-memcpy-bench LIFETIME ${os_lifetime} APPLICATION memcpy-bench)
elseif(SUNSTONE_APPLICATION STREQUAL "headset-fixture")
  # Declares the headset device, so the image links the headset library with the Bluetooth stack and mSBC.
  add_sunstone_application(headset-fixture
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/headset-fixture/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/headset-fixture/manifest.json")
  add_sunstone_image(sunstone-a733-os LIFETIME ${os_lifetime} APPLICATION headset-fixture)
elseif(SUNSTONE_APPLICATION STREQUAL "radio-scan")
  if(NOT SUNSTONE_A733_OS_RADIO_SCAN)
    message(FATAL_ERROR "radio-scan needs SUNSTONE_A733_OS_RADIO_SCAN=ON")
  endif()
  add_sunstone_application(radio-scan
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/radio-scan/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/radio-scan/manifest.json")
  add_sunstone_image(sunstone-a733-os-radio-scan LIFETIME ${os_lifetime} APPLICATION radio-scan)
elseif(SUNSTONE_APPLICATION STREQUAL "large-region-test")
  # The 1 GiB region needs the upper memory mapped, which the table builder does only after the address-test
  # decode check (cmake/a733-smp-cached.cmake).
  if(NOT SUNSTONE_A733_DRAM_DECODE_PROBE)
    message(FATAL_ERROR "large-region-test needs -DSUNSTONE_A733_DRAM_DECODE_PROBE=ON")
  endif()
  # The test runs on the A76 cluster with CPU clock grants. The session lifetime follows the manifest's run deadline
  # (tools/session_lifetime.py).
  set(large_region_manifest "${SUNSTONE_SOURCE_ROOT}/apps/large-region-test/manifest-return.json")
  add_sunstone_application(large-region
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/large-region-test/main.c"
    MANIFEST "${large_region_manifest}")
  add_sunstone_image(sunstone-a733-os-large-region-test LIFETIME ${os_lifetime} APPLICATION large-region)
elseif(SUNSTONE_APPLICATION STREQUAL "heap-test")
  # The heap is a region of upper memory (step 1 of docs/designs/application-memory-layout.md); the table builder
  # maps upper memory only after the address-test decode check (cmake/a733-smp-cached.cmake).
  if(NOT SUNSTONE_A733_DRAM_DECODE_PROBE)
    message(FATAL_ERROR "heap-test needs -DSUNSTONE_A733_DRAM_DECODE_PROBE=ON")
  endif()
  set(SUNSTONE_HEAP_TEST_MODE "0" CACHE STRING "0: use the heap; 1: read it from a task in no space; 2: read the byte after it; 3: share it with a worker")
  add_sunstone_application(heap-test
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/heap-test/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/heap-test/manifest.json")
  set(SUNSTONE_HEAP_TEST_NOTIFY_WAIT "0" CACHE STRING "1: mode 3 waits for the worker's task notification, not its flag")
  target_compile_definitions(heap-test PRIVATE HEAP_TEST_MODE=${SUNSTONE_HEAP_TEST_MODE}
    HEAP_TEST_NOTIFY_WAIT=${SUNSTONE_HEAP_TEST_NOTIFY_WAIT})
  add_sunstone_image(sunstone-a733-os-heap-test LIFETIME ${os_lifetime} APPLICATION heap-test)
elseif(SUNSTONE_APPLICATION STREQUAL "slot1-test")
  if(NOT SUNSTONE_A733_DRAM_DECODE_PROBE)
    message(FATAL_ERROR "slot1-test needs -DSUNSTONE_A733_DRAM_DECODE_PROBE=ON")
  endif()
  set(SUNSTONE_SLOT1_TEST_MODE "0" CACHE STRING "0: use the region; 1: use it, then read the byte after it; 2: overflow the main stack")
  set(SUNSTONE_SLOT1_TEST_MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/slot1-test/manifest.json" CACHE FILEPATH
    "The manifest of slot1-test (apps/slot1-test/manifest-preserve.json: small regions, one of them scratch, for the preserving run)")
  add_sunstone_application(slot1-test
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/slot1-test/main.c"
    MANIFEST "${SUNSTONE_SLOT1_TEST_MANIFEST}")
  target_compile_definitions(slot1-test PRIVATE SLOT1_TEST_MODE=${SUNSTONE_SLOT1_TEST_MODE})
  add_sunstone_image(sunstone-a733-os-slot1-test LIFETIME ${os_lifetime} APPLICATION slot1-test)
elseif(SUNSTONE_APPLICATION STREQUAL "large-6g")
  # A 6 GiB region with no source: bandwidth and integrity (probe 0), or one isolation fault (probes 1 to 3).
  if(NOT SUNSTONE_A733_DRAM_DECODE_PROBE)
    message(FATAL_ERROR "large-6g needs -DSUNSTONE_A733_DRAM_DECODE_PROBE=ON")
  endif()
  set(SUNSTONE_LARGE_6G_PROBE 0 CACHE STRING "large-6g: 0 bandwidth and integrity, 1 below, 2 above, 3 other space")
  set_source_files_properties("${SUNSTONE_SOURCE_ROOT}/apps/large-6g/main.c" PROPERTIES COMPILE_DEFINITIONS LARGE_6G_PROBE=${SUNSTONE_LARGE_6G_PROBE})
  add_sunstone_application(large-6g
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/large-6g/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/large-6g/manifest.json")
  add_sunstone_image(sunstone-a733-os-large-6g LIFETIME ${os_lifetime} APPLICATION large-6g)
elseif(SUNSTONE_APPLICATION STREQUAL "storage-write-test")
  # Creates the region test file through the FAT32 write service; the image is write-capable, so the runner needs the
  # coordinator's approval record for its bounds (tools/storage_bounds.py).
  if(NOT SUNSTONE_A733_OS_SD_WRITE)
    message(FATAL_ERROR "storage-write-test needs -DSUNSTONE_A733_OS_SD_WRITE=ON")
  endif()
  add_sunstone_application(storage-write
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/storage-write-test/main.c"
            "${SUNSTONE_SOURCE_ROOT}/apps/storage-write-test/sha256_ce.S"
            "${SUNSTONE_SOURCE_ROOT}/drivers/storage/fat32_rw.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/storage-write-test/manifest.json")
  add_sunstone_image(sunstone-a733-os-storage-write-test LIFETIME ${os_lifetime} APPLICATION storage-write)
elseif(SUNSTONE_APPLICATION STREQUAL "latency-cost")
  include("${SUNSTONE_SOURCE_ROOT}/apps/latency-cost/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os-latency-cost LIFETIME ${os_lifetime} APPLICATION latency-cost)
  target_sources(sunstone-a733-os-latency-cost PRIVATE
    "${SUNSTONE_SOURCE_ROOT}/apps/latency-cost/switch.c" "${SUNSTONE_SOURCE_ROOT}/apps/latency-cost/irq.c")
  target_link_options(sunstone-a733-os-latency-cost PRIVATE
    -Wl,--wrap=vTaskSwitchContext -Wl,--wrap=vApplicationIRQHandler)
  if(SUNSTONE_LATENCY)
    set_property(SOURCE "${SUNSTONE_SOURCE_ROOT}/runtime/latency_irq.c"
      TARGET_DIRECTORY sunstone-a733-os-core APPEND PROPERTY COMPILE_DEFINITIONS
      __wrap_vApplicationIRQHandler=latency_cost_inner_irq)
    target_compile_definitions(sunstone-a733-os-latency-cost PRIVATE SUNSTONE_LATENCY=1)
  endif()
  set_source_files_properties("${SUNSTONE_SOURCE_ROOT}/apps/latency-cost/switch.c"
    "${SUNSTONE_SOURCE_ROOT}/apps/latency-cost/irq.c" PROPERTIES COMPILE_OPTIONS -mgeneral-regs-only)
elseif(SUNSTONE_APPLICATION MATCHES "^latency(-dma)?$")
  if(NOT SUNSTONE_LATENCY)
    message(FATAL_ERROR "The latency diagnostic requires SUNSTONE_LATENCY=ON")
  endif()
  if(SUNSTONE_APPLICATION STREQUAL "latency-dma" AND NOT SUNSTONE_A733_OS_DMA)
    message(FATAL_ERROR "The combined latency diagnostic requires SUNSTONE_A733_OS_DMA=ON")
  endif()
  include("${SUNSTONE_SOURCE_ROOT}/apps/latency/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os-${SUNSTONE_APPLICATION} LIFETIME ${os_lifetime} APPLICATION ${SUNSTONE_APPLICATION})
elseif(SUNSTONE_APPLICATION MATCHES "^(cpu-capacity|cpu-split)$")
  if(NOT SUNSTONE_A733_CPU_TOPOLOGY)
    message(FATAL_ERROR "CPU capacity observations require SUNSTONE_A733_CPU_TOPOLOGY=ON")
  endif()
  include("${SUNSTONE_SOURCE_ROOT}/apps/scheduler-capacity/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os-${SUNSTONE_APPLICATION} LIFETIME ${os_lifetime} APPLICATION ${SUNSTONE_APPLICATION})
elseif(SUNSTONE_APPLICATION STREQUAL "table-walks")
  add_sunstone_application(table-walks
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/table-walks/main.c"
      "${SUNSTONE_SOURCE_ROOT}/apps/table-walks/walk.S"
      "${SUNSTONE_SOURCE_ROOT}/platform/a733/cpu_pmu.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/table-walks/manifest.json"
    INPUTS "${SUNSTONE_SOURCE_ROOT}/apps/table-walks/privilege.c"
    INCLUDE_DIRECTORIES "${SUNSTONE_SOURCE_ROOT}/apps/table-walks")
  add_sunstone_image(sunstone-a733-os-table-walks LIFETIME ${os_lifetime} APPLICATION table-walks)
  target_sources(sunstone-a733-os-table-walks PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/table-walks/privilege.c")
  target_link_options(sunstone-a733-os-table-walks PRIVATE -Wl,--wrap=os_application_task_privileged)
elseif(SUNSTONE_APPLICATION STREQUAL "dram-bandwidth")
  include("${SUNSTONE_SOURCE_ROOT}/apps/dram-bandwidth/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os-dram-bandwidth LIFETIME ${os_lifetime} APPLICATION dram-bandwidth)
elseif(SUNSTONE_APPLICATION STREQUAL "dram-scaling")
  include("${SUNSTONE_SOURCE_ROOT}/apps/dram-scaling/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os-dram-scaling LIFETIME ${os_lifetime} APPLICATION dram-scaling)
elseif(SUNSTONE_APPLICATION STREQUAL "dram-nsi")
  set(SUNSTONE_DRAM_NSI_MODE "read" CACHE STRING "read: only read the NSI words; write: also write boot0's values and repeat the study; study: no NSI write, one phase")
  include("${SUNSTONE_SOURCE_ROOT}/apps/dram-nsi/cmake/application.cmake")
  if(SUNSTONE_DRAM_NSI_MODE STREQUAL "write")
    target_compile_definitions(dram-nsi PRIVATE DN_WRITE=1)
  elseif(SUNSTONE_DRAM_NSI_MODE STREQUAL "read")
    target_compile_definitions(dram-nsi PRIVATE DN_PROBE_ONLY=1)
  elseif(NOT SUNSTONE_DRAM_NSI_MODE STREQUAL "study")
    message(FATAL_ERROR "SUNSTONE_DRAM_NSI_MODE is read, write or study")
  endif()
  add_sunstone_image(sunstone-a733-os-dram-nsi LIFETIME ${os_lifetime} APPLICATION dram-nsi)
elseif(SUNSTONE_APPLICATION STREQUAL "hw-facts")
  if(NOT SUNSTONE_A733_HW_FACTS OR NOT SUNSTONE_A733_CPU_TOPOLOGY)
    message(FATAL_ERROR "The hardware facts image requires SUNSTONE_A733_HW_FACTS=ON and SUNSTONE_A733_CPU_TOPOLOGY=ON")
  endif()
  add_sunstone_application(hw-facts
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/hw-facts/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/hw-facts/manifest.json")
  add_sunstone_image(sunstone-a733-os-hw-facts LIFETIME ${os_lifetime} APPLICATION hw-facts)
elseif(SUNSTONE_APPLICATION MATCHES "^queue-(add|gemm)$")
  add_sunstone_application(${SUNSTONE_APPLICATION}
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/${SUNSTONE_APPLICATION}/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/${SUNSTONE_APPLICATION}/manifest.json"
    INCLUDE_DIRECTORIES "${SUNSTONE_SOURCE_ROOT}/apps/${SUNSTONE_APPLICATION}")
  if(SUNSTONE_APPLICATION STREQUAL "queue-gemm")
    option(SUNSTONE_GPU_GEMM_CONTROL "Use direct global loads for the GEMM control" OFF)
    if(SUNSTONE_GPU_GEMM_CONTROL)
      target_compile_definitions(queue-gemm PRIVATE SUNSTONE_GPU_GEMM_CONTROL=1)
    endif()
  endif()
  add_sunstone_image(sunstone-a733-os-${SUNSTONE_APPLICATION} LIFETIME ${os_lifetime} APPLICATION ${SUNSTONE_APPLICATION})
elseif(SUNSTONE_APPLICATION STREQUAL "gpu-fast")
  if(NOT SUNSTONE_A733_OS_GPU OR NOT SUNSTONE_GPU_FAST_TIMING)
    message(FATAL_ERROR "GPU benchmark requires the GPU and optional GPU timing")
  endif()
  add_sunstone_application(gpu-fast
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/gpu-fast/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/gpu-fast/manifest.json")
  add_sunstone_image(sunstone-a733-os-gpu-fast LIFETIME ${os_lifetime} APPLICATION gpu-fast)
elseif(SUNSTONE_APPLICATION STREQUAL "gpu-lifecycle")
  if(NOT SUNSTONE_A733_OS_GPU)
    message(FATAL_ERROR "GPU lifecycle requires SUNSTONE_A733_OS_GPU=ON")
  endif()
  option(SUNSTONE_GPU_LIFECYCLE_PEER "Link the fixed sharing peer into the lifecycle image" ON)
  if(NOT SUNSTONE_GPU_LIFECYCLE_PEER)
    message(FATAL_ERROR "GPU lifecycle qualification requires the fixed sharing peer")
  endif()
  set(os_lifecycle_sources "${SUNSTONE_SOURCE_ROOT}/apps/gpu-lifecycle/main.c")
  if(SUNSTONE_GPU_LIFECYCLE_PEER)
    list(APPEND os_lifecycle_sources
      "${SUNSTONE_SOURCE_ROOT}/apps/gpu-queue-peer/main.c"
      "${SUNSTONE_SOURCE_ROOT}/apps/gpu-queue-peer/manifest.c")
  endif()
  add_sunstone_application(gpu-lifecycle
    SOURCES ${os_lifecycle_sources}
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/gpu-lifecycle/manifest.json"
    INCLUDE_DIRECTORIES "${SUNSTONE_SOURCE_ROOT}/apps/gpu-lifecycle"
      "${SUNSTONE_SOURCE_ROOT}/apps/gpu-queue-peer")
  target_compile_definitions(gpu-lifecycle PRIVATE
    GPU_LIFECYCLE_PEER=$<BOOL:${SUNSTONE_GPU_LIFECYCLE_PEER}>
    GPU_LIFECYCLE_RECOVERY=$<BOOL:${SUNSTONE_A733_OS_GPU_RECOVERY_TEST}>)
  add_sunstone_image(sunstone-a733-os-gpu-lifecycle LIFETIME ${os_lifetime} APPLICATION gpu-lifecycle)
elseif(SUNSTONE_APPLICATION MATCHES "^usb-(input|serial|camera)$")
  if(NOT SUNSTONE_A733_OS_USB_CLASSES)
    message(FATAL_ERROR "USB demos require SUNSTONE_A733_OS_USB_CLASSES=ON")
  endif()
  add_sunstone_application(${SUNSTONE_APPLICATION}
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/${SUNSTONE_APPLICATION}/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/${SUNSTONE_APPLICATION}/manifest.json")
  if(SUNSTONE_A733_OS_USB_JPEG)
    target_compile_definitions(${SUNSTONE_APPLICATION} PRIVATE SUNSTONE_A733_OS_USB_JPEG=1)
  endif()
  add_sunstone_image(sunstone-a733-os-${SUNSTONE_APPLICATION} LIFETIME ${os_lifetime} APPLICATION ${SUNSTONE_APPLICATION})
else()
  if(NOT DEFINED SUNSTONE_APPLICATION_${SUNSTONE_APPLICATION}_ROOT)
    message(FATAL_ERROR "Unknown SUNSTONE_APPLICATION: ${SUNSTONE_APPLICATION}")
  endif()
  include("${SUNSTONE_APPLICATION_${SUNSTONE_APPLICATION}_ROOT}/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os LIFETIME ${os_lifetime} APPLICATION ${SUNSTONE_APPLICATION})
endif()

target_sources(sunstone-a733-os-core PRIVATE "${SUNSTONE_SOURCE_ROOT}/runtime/npu_app.c")
