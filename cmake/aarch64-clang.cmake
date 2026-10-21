set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
foreach(language C ASM CXX)
  if(NOT DEFINED CMAKE_${language}_COMPILER)
    if(language STREQUAL "CXX")
      set(CMAKE_${language}_COMPILER clang++)
    else()
      set(CMAKE_${language}_COMPILER clang)
    endif()
  endif()
endforeach()
set(CMAKE_CXX_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_C_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SUNSTONE_LLD)
find_program(SUNSTONE_LLD NAMES ld.lld REQUIRED)
set(CMAKE_EXE_LINKER_FLAGS_INIT "--ld-path=${SUNSTONE_LLD}")

# This file is the one CMake input shared by the central build and the
# standalone application projects, so it also holds the path names used for
# reproducible output. Compiled output must not depend on where the checkout,
# the build directory or the fetched sources live. Each root below gets a fixed
# name: compilers replace it through -ffile-prefix-map in __FILE__ strings and
# debug information, and sunstone_portable_text() replaces it in text that a
# build identity records, such as compiler options.
set(SUNSTONE_SOURCE_CACHE "$ENV{HOME}/.cache/sunstoneos/sources" CACHE PATH
  "Source cache filled by tools/fetch-sources.py")
function(sunstone_path_maps output)
  get_filename_component(source_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
  get_filename_component(cache_root "${SUNSTONE_SOURCE_CACHE}/.." ABSOLUTE)
  set(maps)
  foreach(pair "cache|${cache_root}" "sources|${SUNSTONE_SOURCE_CACHE}" "freertos|${FREERTOS_DIR}"
      "tinyusb|${TINYUSB_DIR}" "tcp|${SUNSTONE_TCP_DIR}" "rsn|${SUNSTONE_RSN_DIR}" "mimus|${MIMUS_DIR}"
      "newlib|${NEWLIB_ROOT}" "newlib-smp|${NEWLIB_SMP_ROOT}" "c-headers|${SUNSTONE_NETWORK_C_HEADERS}"
      "src|${source_root}" "build|${CMAKE_BINARY_DIR}")
    string(FIND "${pair}" "|" split)
    string(SUBSTRING "${pair}" 0 ${split} name)
    math(EXPR split "${split} + 1")
    string(SUBSTRING "${pair}" ${split} -1 root)
    if(root)
      cmake_path(NORMAL_PATH root)
      string(REGEX REPLACE "(.)/$" "\\1" root "${root}")
      # Order by length so a nested root's map is both longer and later;
      # Clang applies the longest macro map and the last debug map.
      string(LENGTH "${root}" length)
      string(LENGTH "0000${length}" digits)
      math(EXPR digits "${digits} - 4")
      string(SUBSTRING "0000${length}" ${digits} 4 length)
      list(APPEND maps "${length}|${root}=${name}")
    endif()
  endforeach()
  list(SORT maps)
  list(TRANSFORM maps REPLACE "^[0-9]+\\|" "")
  set(${output} ${maps} PARENT_SCOPE)
endfunction()
function(sunstone_portable_text output text)
  sunstone_path_maps(maps)
  list(REVERSE maps)
  foreach(map IN LISTS maps)
    string(FIND "${map}" "=" split REVERSE)
    string(SUBSTRING "${map}" 0 ${split} root)
    math(EXPR split "${split} + 1")
    string(SUBSTRING "${map}" ${split} -1 name)
    string(REPLACE "${root}" "${name}" text "${text}")
  endforeach()
  set(${output} "${text}" PARENT_SCOPE)
endfunction()
