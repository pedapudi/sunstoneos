include_guard(GLOBAL)

# Application sources are compiled independently of the OS. The manifest remains
# an image input, so one application can be used by images with different lifetimes.
function(add_sunstone_application name)
  cmake_parse_arguments(APP "" "MANIFEST" "SOURCES;LIBRARIES;INCLUDE_DIRECTORIES;INPUTS" ${ARGN})
  if(APP_UNPARSED_ARGUMENTS OR NOT APP_MANIFEST OR NOT APP_SOURCES)
    message(FATAL_ERROR "add_sunstone_application requires SOURCES and MANIFEST")
  endif()
  if(NOT TARGET sunstone-os-settings)
    message(FATAL_ERROR "Configure the OS core before registering applications")
  endif()
  get_filename_component(manifest "${APP_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  set(output "${CMAKE_CURRENT_BINARY_DIR}/sunstone-applications/${name}")
  file(MAKE_DIRECTORY "${output}")
  configure_file("${manifest}" "${output}/manifest-input.json" @ONLY)
  set(resident_option)
  if(SUNSTONE_A733_OS_RESIDENT)
    set(resident_option --resident)
  endif()
  execute_process(COMMAND "${Python3_EXECUTABLE}" "${SUNSTONE_SOURCE_ROOT}/tools/sunstone-manifest.py"
    emit --manifest "${output}/manifest-input.json" --output "${output}" ${resident_option} COMMAND_ERROR_IS_FATAL ANY)
  string(REPLACE "-" "_" cname "${name}")
  add_library(${name} OBJECT EXCLUDE_FROM_ALL ${APP_SOURCES})
  target_link_libraries(${name} PRIVATE sunstone-os-settings ${APP_LIBRARIES})
  target_include_directories(${name} PRIVATE "${output}" ${APP_INCLUDE_DIRECTORIES})
  set_target_properties(${name} PROPERTIES SUNSTONE_MANIFEST "${output}/manifest-input.json"
    SUNSTONE_MANIFEST_DIRECTORY "${output}" SUNSTONE_APPLICATION_NAME "${name}"
    SUNSTONE_APPLICATION_LIBRARIES "${APP_LIBRARIES}"
    SUNSTONE_APPLICATION_INPUTS "${manifest};${APP_INPUTS}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${manifest}" "${SUNSTONE_SOURCE_ROOT}/tools/sunstone-manifest.py"
    # The manifest tool reads the device bits from the SDK header.
    "${SUNSTONE_SOURCE_ROOT}/include/sunstone_app.h")
endfunction()
