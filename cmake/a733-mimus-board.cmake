# Fixed concurrent recorded recognition and actual read-only USB2 storage.
if(NOT SUNSTONE_BOARD_SUPERVISED OR NOT TARGET sunstone-a733-board OR NOT TARGET mimus_asr_smp)
  message(FATAL_ERROR "Mimus board requires supervised board and SMP Mimus/newlib")
endif()
if(SUNSTONE_BOARD_USB2_IRQ)
  message(FATAL_ERROR "Mimus board currently uses the accepted polling USB2 owner")
endif()
set(mb_root "${SUNSTONE_SOURCE_ROOT}")
set(mb_build "${CMAKE_CURRENT_BINARY_DIR}/mimus-board-application")
file(MAKE_DIRECTORY "${mb_build}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${mb_root}/apps/mimus-board/configure.py"
  --root "${mb_root}" --output "${mb_build}" RESULT_VARIABLE mb_status)
if(NOT mb_status EQUAL 0)
  message(FATAL_ERROR "Mimus board fixed input generation failed")
endif()
apply_build_patch("${mb_build}/link.ld" "${storage_build}/link.ld" "${mb_root}/apps/mimus/link.patch")
get_target_property(mb_sources sunstone-a733-board SOURCES)
# The board's runtime/memory.c and memmove.c stay linked: newlib's SIMD string
# routines fault when EL3 boot code clears or copies Device memory.
list(REMOVE_ITEM mb_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
  "${mb_root}/apps/board/client.c" "${mb_root}/apps/board/usb0.c" "${mb_root}/apps/board/identity.c"
  "${board_build}/build-id.c")
set(mb_added apps/mimus-board/application.c apps/mimus-board/keyboard.c apps/mimus-board/identity.c apps/mimus-board/recognition.cc
  apps/mimus-board/usb0.c apps/mimus/recorded.cc apps/mimus/platform.cc runtime/newlib.c runtime/newlib_smp.c
  runtime/newlib_syscalls_smp.c runtime/new_delete.cc runtime/cxx_terminate.cc)
foreach(source IN LISTS mb_added)
  list(APPEND mb_sources "${mb_root}/${source}")
endforeach()
list(APPEND mb_sources "${mb_build}/mimus-client.c" "${MIMUS_DIR}/firmware/components/pipeline/src/formatter.cc")
add_executable(sunstone-a733-mimus-board EXCLUDE_FROM_ALL ${mb_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
foreach(property COMPILE_OPTIONS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS LINK_DEPENDS)
  get_target_property(value sunstone-a733-board ${property})
  if(NOT value STREQUAL "value-NOTFOUND")
    set_property(TARGET sunstone-a733-mimus-board PROPERTY ${property} "${value}")
  endif()
endforeach()
# This new executable still uses the board's actual dual USB device/host build.
target_compile_definitions(sunstone-a733-mimus-board PRIVATE SUNSTONE_BOARD_APPLICATION=1 SUNSTONE_BOARD_USB2_DUAL=1
  "$<$<COMPILE_LANGUAGE:CXX>:SUNSTONE_MIMUS_NATIVE=1;SUNSTONE_MIMUS_SUPERVISED=1>")
get_target_property(mb_includes sunstone-a733-mimus-board INCLUDE_DIRECTORIES)
list(REMOVE_ITEM mb_includes "${mb_root}/runtime/include" "${SUNSTONE_NETWORK_C_HEADERS}")
set_property(TARGET sunstone-a733-mimus-board PROPERTY INCLUDE_DIRECTORIES "${mb_includes}")
sunstone_smp_compile(sunstone-a733-mimus-board)
target_include_directories(sunstone-a733-mimus-board BEFORE PRIVATE "${mb_root}/apps/mimus-board" "${mb_build}")
target_include_directories(sunstone-a733-mimus-board PRIVATE "${MIMUS_DIR}/firmware/components/pipeline/include")
target_link_libraries(sunstone-a733-mimus-board PRIVATE mimus_asr_smp
  "${newlib_sysroot}/lib/libstdc++.a" "${newlib_sysroot}/lib/libsupc++.a"
  "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a"
  "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
get_target_property(mb_link sunstone-a733-mimus-board LINK_OPTIONS)
list(FILTER mb_link EXCLUDE REGEX "Map,|-T,")
set_property(TARGET sunstone-a733-mimus-board PROPERTY LINK_OPTIONS "${mb_link}")
target_link_options(sunstone-a733-mimus-board PRIVATE "-Wl,-T,${mb_build}/link.ld"
  "-Wl,-Map,${mb_build}/image.map" -Wl,--wrap=_impure_ptr -Wl,--defsym=__runtime_heap_size=4194304)
get_target_property(mb_asr_sources mimus_asr_smp SOURCES)
file(GLOB_RECURSE mb_asr_headers "${MIMUS_DIR}/firmware/components/asr/include/*.h"
  "${MIMUS_DIR}/firmware/components/pipeline/include/*.h")
file(GLOB mb_headers "${mb_root}/apps/mimus-board/*.h")
set(mb_inputs ${board_inputs} ${mb_sources} ${mb_headers} ${mb_asr_sources} ${mb_asr_headers}
  "${mb_root}/apps/mimus-board/configure.py" "${mb_root}/apps/mimus-board/inputs.json"
  "${mb_root}/apps/bluetooth/recognition-models.json" "${mb_build}/assets.h"
  "${mb_build}/mimus-usb-base.c" "${mb_build}/link.ld" "${CMAKE_CURRENT_LIST_FILE}"
  "${mb_root}/cmake/mimus-smp.cmake" "${mb_root}/cmake/mimus.cmake"
  "${mb_root}/CMakeLists.txt" "${mb_root}/cmake/smp.cmake"
  "${mb_root}/apps/mimus/source.json" "${mb_root}/apps/mimus/recorded.h"
  "${mb_root}/apps/mimus/link.patch" "${newlib_sysroot}/lib/libstdc++.a"
  "${newlib_sysroot}/lib/libsupc++.a" "${smp_libc}/lib/libm.a" "${smp_libc}/lib/libc.a"
  "${NEWLIB_ROOT}/lib/gcc/aarch64-none-elf/14.3.1/libgcc.a")
list(REMOVE_DUPLICATES mb_inputs)
set(mb_identity "native-mimus-board-1;fixed-assets;manual-power-removal;4MiB-heap;${MIMUS_AARCH64_NEON};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION}")
foreach(input IN LISTS mb_inputs)
  file(SHA256 "${input}" hash)
  string(APPEND mb_identity "${hash}")
endforeach()
string(SHA256 mb_build_id "${mb_identity}")
string(REGEX REPLACE "(..)" "0x\\1," mb_id_bytes "${mb_build_id}")
file(WRITE "${mb_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${mb_id_bytes}};\n")
file(WRITE "${mb_build}/build-id.txt" "${mb_build_id}\n")
target_sources(sunstone-a733-mimus-board PRIVATE "${mb_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${mb_inputs})
set_target_properties(sunstone-a733-mimus-board PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${mb_build}/link.ld")
add_custom_command(TARGET sunstone-a733-mimus-board POST_BUILD
  COMMAND "${Python3_EXECUTABLE}" "${mb_root}/apps/mimus-board/manifest.py"
    --image "$<TARGET_FILE:sunstone-a733-mimus-board>" --build-id "${mb_build}/build-id.txt"
    --output "${mb_build}/image-manifest.json" VERBATIM)
