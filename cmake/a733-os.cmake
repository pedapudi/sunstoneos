# Select one application of the native OS core. An empty selection builds the
# same OS services with no application task or application manifest.
set(SUNSTONE_APPLICATION "" CACHE STRING "Registered OS application name; empty builds only OS services")
include("${CMAKE_CURRENT_LIST_DIR}/sunstone-os-core.cmake")
if(NOT TARGET sunstone-a733-os-core)
  return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/sunstone-image.cmake")
set(os_lifetime supervised)
if(SUNSTONE_A733_OS_RESIDENT)
  set(os_lifetime resident)
elseif(SUNSTONE_A733_OS_CPU_RETURN)
  set(os_lifetime return)
endif()
set(os_services)
foreach(service radio npu)
  string(TOUPPER "${service}" option)
  if(SUNSTONE_A733_OS_${option})
    list(APPEND os_services ${service})
  endif()
endforeach()
if(SUNSTONE_APPLICATION STREQUAL "")
  add_sunstone_image(sunstone-a733-os LIFETIME ${os_lifetime} SERVICES ${os_services})
elseif(SUNSTONE_APPLICATION STREQUAL "hello-compute")
  add_sunstone_application(hello-compute
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/hello-compute/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/hello-compute/manifest.json")
  add_sunstone_image(sunstone-a733-os-hello LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION hello-compute)
elseif(SUNSTONE_APPLICATION STREQUAL "latency-cost")
  include("${SUNSTONE_SOURCE_ROOT}/apps/latency-cost/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os-latency-cost LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION latency-cost)
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
  add_sunstone_image(sunstone-a733-os-${SUNSTONE_APPLICATION} LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION ${SUNSTONE_APPLICATION})
elseif(SUNSTONE_APPLICATION MATCHES "^(cpu-capacity|cpu-split)$")
  if(NOT SUNSTONE_A733_CPU_TOPOLOGY)
    message(FATAL_ERROR "CPU capacity observations require SUNSTONE_A733_CPU_TOPOLOGY=ON")
  endif()
  if(NOT SUNSTONE_A733_OS_CPU_CLOCK)
    message(FATAL_ERROR "CPU capacity measurements require SUNSTONE_A733_OS_CPU_CLOCK=ON and A733_A76_ADMITTED=ON")
  endif()
  include("${SUNSTONE_SOURCE_ROOT}/apps/scheduler-capacity/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os-${SUNSTONE_APPLICATION} LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION ${SUNSTONE_APPLICATION})
elseif(SUNSTONE_APPLICATION STREQUAL "dram-bandwidth")
  if(NOT SUNSTONE_A733_OS_CPU_CLOCK OR NOT A733_A76_ADMITTED)
    message(FATAL_ERROR "The DRAM bandwidth measurement requires SUNSTONE_A733_OS_CPU_CLOCK=ON and A733_A76_ADMITTED=ON")
  endif()
  include("${SUNSTONE_SOURCE_ROOT}/apps/dram-bandwidth/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os-dram-bandwidth LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION dram-bandwidth)
elseif(SUNSTONE_APPLICATION STREQUAL "dram-scaling")
  if(NOT SUNSTONE_A733_OS_CPU_CLOCK OR NOT A733_A76_ADMITTED)
    message(FATAL_ERROR "The DRAM scaling measurement requires SUNSTONE_A733_OS_CPU_CLOCK=ON and A733_A76_ADMITTED=ON")
  endif()
  include("${SUNSTONE_SOURCE_ROOT}/apps/dram-scaling/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os-dram-scaling LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION dram-scaling)
elseif(SUNSTONE_APPLICATION STREQUAL "dram-nsi")
  if(NOT SUNSTONE_A733_OS_CPU_CLOCK OR NOT A733_A76_ADMITTED)
    message(FATAL_ERROR "The DRAM interconnect study requires SUNSTONE_A733_OS_CPU_CLOCK=ON and A733_A76_ADMITTED=ON")
  endif()
  set(SUNSTONE_DRAM_NSI_MODE "read" CACHE STRING "read: only read the NSI words; write: also write boot0's values and repeat the study; study: no NSI write, one phase")
  include("${SUNSTONE_SOURCE_ROOT}/apps/dram-nsi/cmake/application.cmake")
  if(SUNSTONE_DRAM_NSI_MODE STREQUAL "write")
    target_compile_definitions(dram-nsi PRIVATE DN_WRITE=1)
  elseif(SUNSTONE_DRAM_NSI_MODE STREQUAL "read")
    target_compile_definitions(dram-nsi PRIVATE DN_PROBE_ONLY=1)
  elseif(NOT SUNSTONE_DRAM_NSI_MODE STREQUAL "study")
    message(FATAL_ERROR "SUNSTONE_DRAM_NSI_MODE is read, write or study")
  endif()
  add_sunstone_image(sunstone-a733-os-dram-nsi LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION dram-nsi)
elseif(SUNSTONE_APPLICATION STREQUAL "dram-qos")
  if(NOT SUNSTONE_A733_OS_CPU_CLOCK OR NOT A733_A76_ADMITTED)
    message(FATAL_ERROR "The DRAM latency study requires SUNSTONE_A733_OS_CPU_CLOCK=ON and A733_A76_ADMITTED=ON")
  endif()
  set(SUNSTONE_DRAM_QOS_LOAD "0" CACHE STRING "0 none, 1 streaming cores, 2 NPU job stream (needs SUNSTONE_A733_OS_NPU), 3 DMA0 copies (needs SUNSTONE_A733_OS_DMA)")
  include("${SUNSTONE_SOURCE_ROOT}/apps/dram-qos/cmake/application.cmake")
  target_compile_definitions(dram-qos PRIVATE DQ_LOAD=${SUNSTONE_DRAM_QOS_LOAD})
  add_sunstone_image(sunstone-a733-os-dram-qos LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION dram-qos)
elseif(SUNSTONE_APPLICATION STREQUAL "hw-facts")
  if(NOT SUNSTONE_A733_HW_FACTS OR NOT SUNSTONE_A733_CPU_TOPOLOGY)
    message(FATAL_ERROR "The hardware facts image requires SUNSTONE_A733_HW_FACTS=ON and SUNSTONE_A733_CPU_TOPOLOGY=ON")
  endif()
  add_sunstone_application(hw-facts
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/hw-facts/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/hw-facts/manifest.json")
  add_sunstone_image(sunstone-a733-os-hw-facts LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION hw-facts)
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
  add_sunstone_image(sunstone-a733-os-${SUNSTONE_APPLICATION} LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION ${SUNSTONE_APPLICATION})
elseif(SUNSTONE_APPLICATION STREQUAL "gpu-fast")
  if(NOT SUNSTONE_A733_OS_GPU OR NOT SUNSTONE_GPU_FAST_TIMING)
    message(FATAL_ERROR "GPU benchmark requires the GPU and optional GPU timing")
  endif()
  add_sunstone_application(gpu-fast
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/gpu-fast/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/gpu-fast/manifest.json")
  add_sunstone_image(sunstone-a733-os-gpu-fast LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION gpu-fast)
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
  add_sunstone_image(sunstone-a733-os-gpu-lifecycle LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION gpu-lifecycle)
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
  add_sunstone_image(sunstone-a733-os-${SUNSTONE_APPLICATION} LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION ${SUNSTONE_APPLICATION})
else()
  if(NOT DEFINED SUNSTONE_APPLICATION_${SUNSTONE_APPLICATION}_ROOT)
    message(FATAL_ERROR "Unknown SUNSTONE_APPLICATION: ${SUNSTONE_APPLICATION}")
  endif()
  include("${SUNSTONE_APPLICATION_${SUNSTONE_APPLICATION}_ROOT}/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION ${SUNSTONE_APPLICATION})
endif()

if(SUNSTONE_A733_OS_NPU)
  target_sources(sunstone-a733-os-core PRIVATE "${SUNSTONE_SOURCE_ROOT}/runtime/npu_app.c")
endif()
