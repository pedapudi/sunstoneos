# Images derived from the supervised board image, and the build identity that
# names each of them.
include_guard(GLOBAL)

# Give an image a build ID that changes with every input that can change it,
# and record the ID beside the image.
#
# The ID hashes the given input files, the compiler and linker binaries and
# every option that selects code: the configuration flags, and the compile
# definitions, options, include directories and link options of the target and
# of each object library listed in TARGETS. Checkout and build paths are
# replaced by fixed names first. BUILD_DIR receives build-id.c (rewritten only
# when the ID changes), build-id.txt, the hashed input list identity-inputs.txt
# and the hashed option text identity-options.txt. After each link, BUILD_DIR
# also receives manifest.json with the ID and the SHA-256 of the final ELF.
# RESULT names a variable that receives the ID.
function(sunstone_build_identity target)
  cmake_parse_arguments(PARSE_ARGV 1 id "" "TAG;BUILD_DIR;PROFILE;WORKLOAD;RESULT" "INPUTS;TARGETS")
  string(TOUPPER "${CMAKE_BUILD_TYPE}" config)
  set(options "${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
  foreach(variable CMAKE_C_FLAGS CMAKE_ASM_FLAGS CMAKE_EXE_LINKER_FLAGS)
    string(APPEND options ";${variable}=${${variable}};${${variable}_${config}}")
  endforeach()
  foreach(item IN ITEMS ${target} ${id_TARGETS})
    foreach(property COMPILE_OPTIONS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES LINK_OPTIONS LINK_LIBRARIES)
      get_target_property(value ${item} ${property})
      string(APPEND options "\n${item} ${property}=${value}")
    endforeach()
  endforeach()
  sunstone_portable_text(options "${options}")
  file(SHA256 "${CMAKE_C_COMPILER}" compiler_digest)
  file(SHA256 "${SUNSTONE_LLD}" linker_digest)
  set(identity "${id_TAG};${options};${compiler_digest};${linker_digest}")
  set(inputs ${id_INPUTS} "${CMAKE_CURRENT_FUNCTION_LIST_FILE}")
  list(REMOVE_DUPLICATES inputs)
  foreach(input IN LISTS inputs)
    file(SHA256 "${input}" digest)
    string(APPEND identity "${digest}")
  endforeach()
  string(SHA256 build_id "${identity}")
  string(REGEX REPLACE "(..)" "0x\\1," id_bytes "${build_id}")
  file(WRITE "${id_BUILD_DIR}/build-id.c.tmp" "const unsigned char a733_smp_build_id[32]={${id_bytes}};\n")
  configure_file("${id_BUILD_DIR}/build-id.c.tmp" "${id_BUILD_DIR}/build-id.c" COPYONLY)
  file(REMOVE "${id_BUILD_DIR}/build-id.c.tmp")
  file(WRITE "${id_BUILD_DIR}/build-id.txt" "${build_id}\n")
  string(REPLACE ";" "\n" input_lines "${inputs}")
  file(WRITE "${id_BUILD_DIR}/identity-inputs.txt" "${input_lines}\n")
  file(WRITE "${id_BUILD_DIR}/identity-options.txt" "${options}\n")
  target_sources(${target} PRIVATE "${id_BUILD_DIR}/build-id.c")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${inputs})
  set(fields "\\\"target\\\":\\\"${target}\\\"")
  if(id_PROFILE)
    string(APPEND fields ",\\\"execution_profile\\\":\\\"${id_PROFILE}\\\"")
  endif()
  if(id_WORKLOAD)
    string(APPEND fields ",\\\"workload\\\":\\\"${id_WORKLOAD}\\\"")
  endif()
  file(WRITE "${id_BUILD_DIR}/finish-manifest.cmake" "file(SHA256 \"\${IMAGE}\" digest)\n"
    "file(WRITE \"${id_BUILD_DIR}/manifest.json\" \"{${fields},\\\"build_id\\\":\\\"${build_id}\\\",\\\"final_elf\\\":\\\"\${digest}\\\"}\\n\")\n")
  add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}"
    "-DIMAGE=$<TARGET_FILE:${target}>" -P "${id_BUILD_DIR}/finish-manifest.cmake" VERBATIM)
  if(id_RESULT)
    set(${id_RESULT} "${build_id}" PARENT_SCOPE)
  endif()
endfunction()

# Prepare the supervised board target for an application derived from it.
# A macro, so that the board's variables reach the including recipe.
macro(sunstone_board_application_parent label)
  if(SUNSTONE_BOARD_USB2_IRQ)
    message(FATAL_ERROR "USB2 CPU interrupt completion is board-only; configure this application with SUNSTONE_BOARD_USB2_IRQ=OFF")
  endif()
  set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
  if(NOT TARGET sunstone-a733-board)
    include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
  endif()
  get_target_property(board_application_parent sunstone-a733-board COMPILE_DEFINITIONS)
  if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST board_application_parent)
    message(FATAL_ERROR "${label} cannot inherit an armed board target or generated boot")
  endif()
endmacro()

# Build sunstone-a733-<name> from the supervised board image.
#
# The image keeps every board source except the board's USB0 owner, admission,
# supervised boot and build ID, and compiles them with the board's options,
# include directories and definitions, so that each application runs the same
# providers as the board. The board's radio sources stay; garbage collection
# removes inactive services. The application adds its own sources from
# apps/<name>, which also comes first on the include path, and its own
# contract and result magics.
#
# By default apps/<name>/boot.patch turns the board's supervised boot into the
# application boot; BOOT names an already prepared boot source instead.
# PLATFORM_PATCH replaces the storage platform with one patched by
# apps/<name>/platform.patch. The image links with the storage linker script
# and 2 MiB uncached and device pools unless LINK_SCRIPT names another script
# or NO_POOLS removes the pools. SOURCES, INPUTS and INCLUDES are relative to
# the source root unless absolute; INPUTS are hashed into the build ID beside
# the sources, the app headers and the board inputs. The build directory
# defaults to <name>-application. The recipe receives <prefix>_build,
# <prefix>_build_id, <prefix>_sources and <prefix>_inputs, where PREFIX
# defaults to name with '-' replaced by '_'.
function(sunstone_board_application name)
  cmake_parse_arguments(PARSE_ARGV 1 app "PLATFORM_PATCH;NO_POOLS"
    "PREFIX;BUILD_DIR;BOOT;LINK_SCRIPT;CONTRACT_MAGIC;RESULT_MAGIC;TAG;PROFILE;WORKLOAD"
    "SOURCES;INCLUDES;DEFINITIONS;LINK_OPTIONS;INPUTS")
  set(target sunstone-a733-${name})
  set(root "${SUNSTONE_SOURCE_ROOT}")
  set(app_dir "${root}/apps/${name}")
  if(NOT app_PREFIX)
    string(REPLACE "-" "_" app_PREFIX "${name}")
  endif()
  if(NOT app_BUILD_DIR)
    set(app_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/${name}-application")
  endif()
  set(build "${app_BUILD_DIR}")
  file(MAKE_DIRECTORY "${build}")
  foreach(list SOURCES INCLUDES INPUTS)
    list(TRANSFORM app_${list} PREPEND "${root}/" REGEX "^[^/]")
  endforeach()
  get_target_property(sources sunstone-a733-board SOURCES)
  list(REMOVE_ITEM sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
    "${board_build}/boot-supervised.c" "${root}/apps/board/usb0.c"
    "${root}/apps/board/admission.c" "${board_build}/build-id.c")
  set(inputs "${app_dir}/boot.patch")
  if(app_BOOT)
    list(APPEND sources "${app_BOOT}")
  else()
    apply_build_patch("${build}/boot.c" "${board_build}/boot-supervised.c" "${app_dir}/boot.patch")
    list(APPEND sources "${build}/boot.c")
  endif()
  if(app_PLATFORM_PATCH)
    list(REMOVE_ITEM sources "${storage_build}/platform.c")
    apply_build_patch("${build}/platform.c" "${storage_build}/platform.c" "${app_dir}/platform.patch")
    list(APPEND sources "${build}/platform.c")
    list(APPEND inputs "${app_dir}/platform.patch")
  endif()
  list(APPEND sources ${app_SOURCES})
  list(REMOVE_DUPLICATES sources)
  add_executable(${target} EXCLUDE_FROM_ALL ${sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
  get_target_property(options sunstone-a733-board COMPILE_OPTIONS)
  get_target_property(includes sunstone-a733-board INCLUDE_DIRECTORIES)
  get_target_property(definitions sunstone-a733-board COMPILE_DEFINITIONS)
  get_target_property(links sunstone-a733-board LINK_LIBRARIES)
  list(FILTER definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
  set_property(TARGET ${target} PROPERTY COMPILE_OPTIONS "${options}")
  target_include_directories(${target} BEFORE PRIVATE "${app_dir}")
  target_include_directories(${target} PRIVATE ${includes} ${app_INCLUDES})
  target_compile_definitions(${target} PRIVATE ${definitions} SUNSTONE_BOARD_APPLICATION=1 ${app_DEFINITIONS}
    A733_SMP_CONTRACT_MAGIC=${app_CONTRACT_MAGIC} A733_SMP_RESULT_MAGIC=${app_RESULT_MAGIC})
  if(links)
    target_link_libraries(${target} PRIVATE ${links})
  endif()
  set(pools)
  if(app_LINK_SCRIPT)
    set(script "${app_LINK_SCRIPT}")
    list(APPEND inputs "${script}")
  else()
    set(script "${storage_build}/link.ld")
    if(NOT app_NO_POOLS)
      set(pools -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152)
    endif()
  endif()
  target_link_options(${target} PRIVATE -nostdlib -static -Wl,--gc-sections ${pools} ${app_LINK_OPTIONS}
    "-Wl,-T,${script}" "-Wl,-Map,${build}/image.map")
  set_target_properties(${target} PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${script}")
  file(GLOB headers "${app_dir}/*.h")
  set(inputs ${sources} ${headers} ${board_inputs} ${inputs} ${app_INPUTS} "${CMAKE_CURRENT_LIST_FILE}")
  list(REMOVE_DUPLICATES inputs)
  set(manifest)
  foreach(field PROFILE WORKLOAD)
    if(app_${field})
      list(APPEND manifest ${field} "${app_${field}}")
    endif()
  endforeach()
  sunstone_build_identity(${target} TAG "${app_TAG}" BUILD_DIR "${build}" INPUTS ${inputs}
    TARGETS a733-board-usb2-host ${manifest} RESULT build_id)
  set(${app_PREFIX}_build "${build}" PARENT_SCOPE)
  set(${app_PREFIX}_build_id "${build_id}" PARENT_SCOPE)
  set(${app_PREFIX}_sources "${sources}" PARENT_SCOPE)
  set(${app_PREFIX}_inputs "${inputs}" PARENT_SCOPE)
endfunction()
