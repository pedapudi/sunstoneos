# Fixed single-boot-CPU storage composition. Inherit the accepted service and
# providers, but compile the physically used single-core SRE port and EL1 path.
set(sdsc_root "${SUNSTONE_SOURCE_ROOT}")
set(sdsc_app "${sdsc_root}/apps/storage-read-single-core")
set(sdsc_build "${CMAKE_CURRENT_BINARY_DIR}/storage-read-single-core")
set(sdsc_port "${FREERTOS_DIR}/portable/GCC/ARM_AARCH64_SRE")
file(MAKE_DIRECTORY "${sdsc_build}")
get_target_property(sdsc_sources sunstone-a733-storage-read SOURCES)
list(FILTER sdsc_sources EXCLUDE REGEX "(/platform/a733-smp-|/platform/a733/cpu_|/freertos-a733-smp-cached/|/storage-maintenance/(boot|platform|continue|port|build-id)\\.|/board-application/boot|/storage-read-application/(boot|build-id)\\.)")
list(REMOVE_ITEM sdsc_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${sdsc_root}/apps/board/session.c" "${sdsc_root}/apps/storage-read/snapshot.c")
# USB2 host sources are dormant in workload 2 but referenced by common board
# functions; preserve their namespaced object ABI with this target's kernel ABI.
get_target_property(sdsc_host_sources a733-board-usb2-host SOURCES)
add_library(a733-sd-single-core-usb2-host OBJECT ${sdsc_host_sources})
apply_build_patch("${sdsc_build}/freertos-port.c" "${sdsc_port}/port.c"
 "${sdsc_root}/patches/freertos-aarch64-clang.patch")
apply_build_patch("${sdsc_build}/platform.c" "${sdsc_root}/platform/a733/platform.c" "${sdsc_app}/platform.patch")
apply_build_patch("${sdsc_build}/board-session.c" "${sdsc_root}/apps/board/session.c" "${sdsc_app}/board-session.patch")
apply_build_patch("${sdsc_build}/sd-session.c" "${sdsc_root}/apps/storage-read/snapshot.c" "${sdsc_app}/sd-session.patch")
# The SMP base links its patched tasks.c from the port directory, which the
# filter removes; a boot-CPU-only image links the kernel's own tasks.c, as the
# single-core GPU and NPU images do (cmake/a733-gpu-single-core.cmake).
list(APPEND sdsc_sources "${FREERTOS_DIR}/tasks.c")
list(APPEND sdsc_sources "${sdsc_app}/start.S" "${sdsc_app}/monitor.c" "${sdsc_app}/runtime.c"
 "${sdsc_build}/platform.c" "${sdsc_build}/board-session.c" "${sdsc_build}/sd-session.c"
 "${sdsc_build}/freertos-port.c" "${sdsc_port}/portASM.S"
 "${sdsc_root}/arch/aarch64/start.S" "${sdsc_root}/arch/aarch64/vectors.S"
 "${sdsc_root}/arch/aarch64/cache.S" "${sdsc_root}/platform/a733/cpu_coherence.S")
add_executable(sunstone-a733-storage-read-single-core EXCLUDE_FROM_ALL ${sdsc_sources}
 $<TARGET_OBJECTS:a733-sd-single-core-usb2-host>)
get_target_property(sdsc_options sunstone-a733-storage-read COMPILE_OPTIONS)
get_target_property(sdsc_includes sunstone-a733-storage-read INCLUDE_DIRECTORIES)
get_target_property(sdsc_definitions sunstone-a733-storage-read COMPILE_DEFINITIONS)
get_target_property(sdsc_links sunstone-a733-storage-read LINK_LIBRARIES)
list(FILTER sdsc_definitions EXCLUDE REGEX "^SUNSTONE_A733_SMP_DIAGNOSTIC=")
# Never inherit an SMP port/config directory ahead of this fixed configuration.
list(FILTER sdsc_includes EXCLUDE REGEX "(a733-smp-cached|freertos-a733-smp-cached)")
foreach(target sunstone-a733-storage-read-single-core a733-sd-single-core-usb2-host)
 set_property(TARGET ${target} PROPERTY COMPILE_OPTIONS "${sdsc_options}")
 target_include_directories(${target} BEFORE PRIVATE "${sdsc_app}" "${sdsc_port}")
 target_include_directories(${target} PRIVATE ${sdsc_includes})
 target_compile_definitions(${target} PRIVATE ${sdsc_definitions} GUEST=1 SUNSTONE_SD_SINGLE_CORE=1)
endforeach()
# The USB2 host objects compile against the board's SuperSpeed TinyUSB copy and
# the TinyUSB host headers, as a733-board-usb2-host does (cmake/a733-board.cmake);
# without them hcd.h is missing and TUSB_SPEED_SUPER is undeclared.
target_include_directories(a733-sd-single-core-usb2-host BEFORE PRIVATE "${board_host_tiny}" "${sdsc_root}/drivers/usb/host")
target_include_directories(a733-sd-single-core-usb2-host PRIVATE "${TINYUSB_DIR}/src/host")
target_compile_options(a733-sd-single-core-usb2-host PRIVATE "SHELL:-include ${sdsc_root}/apps/board/host-symbols.h")
if(sdsc_links)
 target_link_libraries(sunstone-a733-storage-read-single-core PRIVATE ${sdsc_links})
endif()
# Include all inherited producer inputs plus this adapter and the actual port.
file(GLOB sdsc_headers "${sdsc_app}/*.h")
set(sdsc_inputs ${sdsc_sources} ${sdsc_headers} ${storage_read_inputs} ${sdsc_host_sources}
 "${sdsc_port}/portmacro.h" "${sdsc_port}/port.c" "${sdsc_app}/link.ld"
 "${sdsc_app}/platform.patch" "${sdsc_app}/board-session.patch" "${sdsc_app}/sd-session.patch"
 "${sdsc_app}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_FILE}"
 "${sdsc_root}/patches/freertos-aarch64-clang.patch")
list(REMOVE_DUPLICATES sdsc_inputs)
set(sdsc_identity "sd0-read-service2;single-boot-cpu;SRE;supervised60;retained;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS sdsc_inputs)
 file(SHA256 "${input}" sdsc_hash)
 string(APPEND sdsc_identity "${sdsc_hash}")
endforeach()
string(SHA256 sdsc_build_id "${sdsc_identity}")
string(REGEX REPLACE "(..)" "0x\\1," sdsc_id_bytes "${sdsc_build_id}")
file(WRITE "${sdsc_build}/build-id.c.tmp" "const unsigned char a733_smp_build_id[32]={${sdsc_id_bytes}};\n")
configure_file("${sdsc_build}/build-id.c.tmp" "${sdsc_build}/build-id.c" COPYONLY)
file(WRITE "${sdsc_build}/build-id.txt" "${sdsc_build_id}\n")
target_sources(sunstone-a733-storage-read-single-core PRIVATE "${sdsc_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${sdsc_inputs})
target_link_options(sunstone-a733-storage-read-single-core PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${sdsc_app}/link.ld" "-Wl,-Map,${sdsc_build}/image.map")
set_target_properties(sunstone-a733-storage-read-single-core PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${sdsc_app}/link.ld")
