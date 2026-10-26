# Only the supported OS8 configuration reaches this check target. The board
# liveness/thermal inputs are modeled; the generated checkpoint is production.
set(os_workers_check smp-mimus-os-workers-model)
get_target_property(os_workers_sources smp-mimus-linear-model SOURCES)
list(REMOVE_ITEM os_workers_sources checks/mimus-linear.cc)
file(WRITE "${os_build}/workers-checkpoint.cc"
  "#define platform_puts os_check_transcript_puts\n#define platform_hex os_check_transcript_hex\n#include \"${os_recognition}\"\n")
add_executable(${os_workers_check} EXCLUDE_FROM_ALL ${os_workers_sources}
  checks/mimus-os-workers.cc "${os_build}/workers-checkpoint.cc")
foreach(property COMPILE_OPTIONS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_FEATURES LINK_OPTIONS LINK_LIBRARIES LINK_DEPENDS)
  get_target_property(value smp-mimus-linear-model ${property})
  if(NOT value STREQUAL "value-NOTFOUND")
    set_property(TARGET ${os_workers_check} PROPERTY ${property} "${value}")
  endif()
endforeach()
# The cloned linker options name the arithmetic fixture's map; keep it intact.
get_target_property(os_workers_link ${os_workers_check} LINK_OPTIONS)
list(FILTER os_workers_link EXCLUDE REGEX "Map,")
set_property(TARGET ${os_workers_check} PROPERTY LINK_OPTIONS "${os_workers_link}")
target_link_options(${os_workers_check} PRIVATE "-Wl,-Map,${os_build}/workers-check.map")
target_include_directories(${os_workers_check} PRIVATE "${os_root}/apps/mimus-board" "${mb_build}")
target_compile_definitions(${os_workers_check} PRIVATE SUNSTONE_A733_OS=1
  SUNSTONE_MIMUS_NATIVE=1 SUNSTONE_MIMUS_SUPERVISED=1 A733_A76_ADMITTED=1
  INCLUDE_uxTaskPriorityGet=1)
set_target_properties(${os_workers_check} PROPERTIES SUFFIX ".elf")
