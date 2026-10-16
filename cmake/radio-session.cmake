# Compose the actual native session and owned socket service with modeled MMIO.
if(TARGET radio-network-model AND SUNSTONE_SESSION_BLOBS AND NOT TARGET radio-session-model)
  get_filename_component(session_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  get_target_property(session_sources radio-network-model SOURCES)
  list(FILTER session_sources EXCLUDE REGEX "/checks/radio-network[.]c$")
  list(APPEND session_sources
    "${session_root}/drivers/net/a733_radio_session.c"
    "${session_root}/drivers/net/a733_radio_platform.c"
    "${session_root}/drivers/clock/a733_clock_leases.c"
    "${session_root}/drivers/power/a733_rail_leases.c"
    "${session_root}/drivers/power/a733_pmic.c"
    "${session_root}/drivers/gpu/a733_gpu_resources.c"
    "${session_root}/drivers/pinctrl/a733_pins.c"
    "${session_root}/drivers/pinctrl/a733_pin_tables.c")
  if(NOT "${session_root}/drivers/net/aic8800_dispatch.c" IN_LIST session_sources)
    list(APPEND session_sources "${session_root}/drivers/net/aic8800_dispatch.c")
  endif()
  add_executable(radio-session-model EXCLUDE_FROM_ALL ${session_sources}
    "${session_root}/checks/a733-radio-session.c" "${SUNSTONE_SESSION_BLOBS}")
  foreach(property INCLUDE_DIRECTORIES COMPILE_OPTIONS LINK_OPTIONS LINK_DEPENDS COMPILE_DEFINITIONS)
    get_target_property(value radio-network-model ${property})
    set_property(TARGET radio-session-model PROPERTY ${property} "${value}")
  endforeach()
  foreach(source IN LISTS session_sources)
    get_source_file_property(options "${source}" TARGET_DIRECTORY radio-network-model COMPILE_OPTIONS)
    if(NOT options STREQUAL "NOTFOUND")
      set_source_files_properties("${source}" TARGET_DIRECTORY radio-session-model PROPERTIES COMPILE_OPTIONS "${options}")
    endif()
  endforeach()
  # The external freestanding libc's stdatomic.h uses integer typedefs before
  # including stdint.h. Supply them for the newly composed platform units.
  foreach(source IN ITEMS drivers/net/a733_radio_session.c drivers/net/a733_radio_platform.c
      drivers/clock/a733_clock_leases.c drivers/power/a733_rail_leases.c
      drivers/power/a733_pmic.c drivers/gpu/a733_gpu_resources.c
      drivers/pinctrl/a733_pins.c drivers/pinctrl/a733_pin_tables.c checks/a733-radio-session.c)
    set_property(SOURCE "${session_root}/${source}" TARGET_DIRECTORY radio-session-model
      APPEND PROPERTY COMPILE_OPTIONS -include stdint.h)
  endforeach()
  get_target_property(session_link_options radio-session-model LINK_OPTIONS)
  list(FILTER session_link_options EXCLUDE REGEX "Map,")
  set_property(TARGET radio-session-model PROPERTY LINK_OPTIONS "${session_link_options}")
  target_link_options(radio-session-model PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/radio-session-model.map")
  set_target_properties(radio-session-model PROPERTIES SUFFIX ".elf")
endif()
