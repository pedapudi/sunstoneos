# Cached diagnostic with a separately admitted USB result observer.
if(NOT DEFINED SUNSTONE_SOURCE_ROOT)
  set(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
if(NOT TARGET a733-smp-cached-diagnostic)
  include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-smp-cached.cmake")
endif()

# An observer image is the cached diagnostic, with its sources and patched
# kernel, plus a patched boot and vector file, the observer diagnostic and USB
# publication. PATCHES are output|input|patch triples; a relative input names an
# earlier output. ORDER puts the patched files where each image's link order has
# always had them, so that every image keeps its layout.
function(a733_smp_observer target tag)
  cmake_parse_arguments(arg "" "" "ADMISSION;PATCHES;ORDER;DEFINITIONS;INCLUDES" ${ARGN})
  set(root "${SUNSTONE_SOURCE_ROOT}")
  set(cached "${root}/platform/a733-smp-cached")
  set(observe "${root}/platform/a733-smp-observe")
  set(build "${CMAKE_CURRENT_BINARY_DIR}/freertos-${target}")
  set(kernel "${CMAKE_CURRENT_BINARY_DIR}/freertos-a733-smp-cached")
  set(usb "${TINYUSB_DIR}/src/portable/sunxi")
  file(MAKE_DIRECTORY "${build}/tinyusb")
  # Private patches preserve the original cached image and its evidence.
  file(SHA256 "${cached}/boot.c" boot_hash)
  file(SHA256 "${cached}/continue.S" vector_hash)
  if(NOT boot_hash STREQUAL "51de45525a794b74dbb3504840d64a33299ca755598377305b197897f7c72514" OR
     NOT vector_hash STREQUAL "da0a16b31d65479202cf49cf4dc23b63e2924ce1a2279f220f750b4626ef2a91")
    message(FATAL_ERROR "Observer hooks require review against changed cached boot/vector inputs")
  endif()
  set(inputs "${cached}/boot.c" "${cached}/continue.S" "${usb}/dcd_sunxi_musb.c" "${usb}/musb_def.h")
  foreach(triple IN LISTS arg_PATCHES ITEMS
      "tinyusb/dcd_sunxi_musb.c|${usb}/dcd_sunxi_musb.c|patches/tinyusb-a733-dcd.patch"
      "tinyusb/musb_def.h|${usb}/musb_def.h|patches/tinyusb-a733-registers.patch")
    string(REPLACE "|" ";" parts "${triple}")
    list(GET parts 0 output)
    list(GET parts 1 input)
    list(GET parts 2 patch)
    cmake_path(ABSOLUTE_PATH input BASE_DIRECTORY "${build}")
    apply_build_patch("${build}/${output}" "${input}" "${root}/${patch}")
    list(APPEND inputs "${root}/${patch}")
  endforeach()

  get_target_property(sources a733-smp-cached-diagnostic SOURCES)
  list(FILTER sources EXCLUDE REGEX "/(build-id\\.c|platform/a733-smp-cached/(boot\\.c|continue\\.S))$")
  list(TRANSFORM sources REPLACE "^${cached}/diagnostic\\.c$" "${observe}/diagnostic.c")
  list(FIND sources "${cached}/admission.c" at)
  list(REMOVE_AT sources ${at})
  list(INSERT sources ${at} ${arg_ADMISSION})
  list(FIND sources "${root}/runtime/memory.c" at)
  list(INSERT sources ${at} "${observe}/usb.c" "${observe}/usb_service.c" "${observe}/result.c"
    "${observe}/service.S")
  list(TRANSFORM arg_ORDER PREPEND "${build}/")
  list(FIND sources "${kernel}/tasks.c" at)
  list(INSERT sources ${at} ${arg_ORDER} "${TINYUSB_DIR}/src/tusb.c" "${TINYUSB_DIR}/src/common/tusb_fifo.c"
    "${TINYUSB_DIR}/src/device/usbd.c" "${TINYUSB_DIR}/src/class/vendor/vendor_device.c"
    "${build}/tinyusb/dcd_sunxi_musb.c")

  # This is a build-input identity, not the final ELF digest. The host records both.
  file(GLOB_RECURSE usb_headers "${TINYUSB_DIR}/src/*.h")
  file(GLOB kernel_headers "${FREERTOS_DIR}/include/*.h")
  list(APPEND inputs ${sources} ${usb_headers} ${kernel_headers} "${kernel}/portmacro.h"
    "${build}/tinyusb/musb_def.h" "${root}/platform/${target}/CMakeLists.txt" "${root}/cmake/${target}.cmake"
    "${CMAKE_CURRENT_FUNCTION_LIST_FILE}" "${root}/cmake/a733-smp-cached.cmake"
    "${root}/platform/a733/cpu_continue.c" "${cached}/continuation-core.patch" "${cached}/cached.h"
    "${cached}/FreeRTOSConfig.h" "${root}/include/a733_smp_placement.h" "${cached}/boot.h" "${cached}/link.ld"
    "${root}/include/FreeRTOSConfig.h"
    "${observe}/result.h" "${observe}/tusb_config.h" "${observe}/a733_usb.h"
    "${root}/include/platform.h" "${root}/runtime/include/string.h")
  foreach(header cpu_start cpu_entry cpu_continue cpu_coherence cached_memory cpu_power)
    list(APPEND inputs "${root}/include/a733_${header}.h")
  endforeach()
  set(identity "${tag};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};armv8-a;O2;WBWA;Device-monitor;8")
  foreach(input IN LISTS inputs)
    file(SHA256 "${input}" digest)
    string(APPEND identity "${digest}")
  endforeach()
  string(SHA256 build_id "${identity}")
  string(REGEX REPLACE "(..)" "0x\\1," id_bytes "${build_id}")
  file(WRITE "${build}/build-id.c.tmp" "const unsigned char a733_smp_build_id[32]={${id_bytes}};\n")
  configure_file("${build}/build-id.c.tmp" "${build}/build-id.c" COPYONLY)
  file(WRITE "${build}/build-id.txt" "${build_id}\n")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${inputs})

  add_executable(${target} EXCLUDE_FROM_ALL ${sources} "${build}/build-id.c")
  foreach(property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_OPTIONS)
    get_target_property(value a733-smp-cached-diagnostic ${property})
    set_property(TARGET ${target} PROPERTY ${property} "${value}")
  endforeach()
  target_include_directories(${target} BEFORE PRIVATE ${arg_INCLUDES} "${observe}" "${root}/drivers/usb"
    "${TINYUSB_DIR}/src")
  target_compile_definitions(${target} PRIVATE SUNSTONE_A733_USB=1 ${arg_DEFINITIONS})
  get_target_property(links ${target} LINK_OPTIONS)
  list(TRANSFORM links REPLACE "^-Wl,-Map,.*$" "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
  set_target_properties(${target} PROPERTIES LINK_OPTIONS "${links}" SUFFIX ".elf"
    LINK_DEPENDS "${cached}/link.ld")
endfunction()

set(observe_cached "${SUNSTONE_SOURCE_ROOT}/platform/a733-smp-cached")
a733_smp_observer(a733-smp-observe "observer-OB-C1-OB-R1"
  ADMISSION "${observe_cached}/admission.c" ORDER boot.c continue.S
  PATCHES "boot.c|${observe_cached}/boot.c|platform/a733-smp-observe/boot.patch"
    "continue.S|${observe_cached}/continue.S|platform/a733-smp-observe/continue.patch"
  DEFINITIONS A733_SMP_CONTRACT_MAGIC=0x415337334f424331ULL A733_SMP_RESULT_MAGIC=0x415337334f425231ULL)
