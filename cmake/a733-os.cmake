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
elseif(SUNSTONE_APPLICATION STREQUAL "queue-add")
  add_sunstone_application(queue-add
    SOURCES "${SUNSTONE_SOURCE_ROOT}/apps/queue-add/main.c"
    MANIFEST "${SUNSTONE_SOURCE_ROOT}/apps/queue-add/manifest.json"
    INCLUDE_DIRECTORIES "${SUNSTONE_SOURCE_ROOT}/apps/queue-add")
  add_sunstone_image(sunstone-a733-os-queue-add LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION queue-add)
elseif(SUNSTONE_APPLICATION STREQUAL "gpu-fast")
  if(NOT SUNSTONE_A733_OS_GPU OR NOT SUNSTONE_GPU_TIMING)
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
