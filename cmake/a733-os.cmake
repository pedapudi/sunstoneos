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

else()
  if(NOT DEFINED SUNSTONE_APPLICATION_${SUNSTONE_APPLICATION}_ROOT)
    message(FATAL_ERROR "Unknown SUNSTONE_APPLICATION: ${SUNSTONE_APPLICATION}")
  endif()
  include("${SUNSTONE_APPLICATION_${SUNSTONE_APPLICATION}_ROOT}/cmake/application.cmake")
  add_sunstone_image(sunstone-a733-os LIFETIME ${os_lifetime}
    SERVICES ${os_services} APPLICATION ${SUNSTONE_APPLICATION})
endif()
