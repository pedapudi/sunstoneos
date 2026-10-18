# Same supervised board provider composition with one fixed native PWM task.
set(SUNSTONE_BOARD_SUPERVISED ON CACHE BOOL "Supervised board lifetime" FORCE)
if(NOT TARGET sunstone-a733-board)
 include("${SUNSTONE_SOURCE_ROOT}/cmake/a733-board.cmake")
endif()
get_target_property(pwm_parent_definitions sunstone-a733-board COMPILE_DEFINITIONS)
if(NOT "SUNSTONE_BOARD_SUPERVISED=1" IN_LIST pwm_parent_definitions)
 message(FATAL_ERROR "PWM cannot inherit an armed board target or generated boot")
endif()
set(pwm_build "${CMAKE_CURRENT_BINARY_DIR}/pwm-application")
file(MAKE_DIRECTORY "${pwm_build}")
get_target_property(pwm_sources sunstone-a733-board SOURCES)
list(REMOVE_ITEM pwm_sources "$<TARGET_OBJECTS:a733-board-usb2-host>"
 "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/usb0.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/board/admission.c" "${board_build}/build-id.c")
# Keep normal source inclusion, actual boot and provider source identities.
apply_build_patch("${pwm_build}/boot.c" "${board_build}/boot-supervised.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/pwm/boot.patch")
list(REMOVE_ITEM pwm_sources "${storage_build}/platform.c")
apply_build_patch("${pwm_build}/platform.c" "${storage_build}/platform.c"
 "${SUNSTONE_SOURCE_ROOT}/apps/pwm/platform.patch")
list(APPEND pwm_sources "${pwm_build}/boot.c" "${pwm_build}/platform.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/pwm/a733_pwm.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/pwm/a733_pwm_native.c"
 "${SUNSTONE_SOURCE_ROOT}/drivers/gpu/gicv3_spi.c")
foreach(source IN ITEMS apps/pwm/main.c apps/pwm/native.c apps/pwm/usb0.c apps/pwm/admission.c apps/pwm/snapshot.c)
 list(APPEND pwm_sources "${SUNSTONE_SOURCE_ROOT}/${source}")
endforeach()
add_executable(sunstone-a733-pwm EXCLUDE_FROM_ALL ${pwm_sources} $<TARGET_OBJECTS:a733-board-usb2-host>)
# The inherited board source list contains real radio dependencies. Preserve its
# normal compile/link properties; garbage collection removes inactive services.
get_target_property(pwm_options sunstone-a733-board COMPILE_OPTIONS)
get_target_property(pwm_includes sunstone-a733-board INCLUDE_DIRECTORIES)
get_target_property(pwm_definitions sunstone-a733-board COMPILE_DEFINITIONS)
get_target_property(pwm_links sunstone-a733-board LINK_LIBRARIES)
list(FILTER pwm_definitions EXCLUDE REGEX "^A733_SMP_(CONTRACT|RESULT)_MAGIC=")
set_property(TARGET sunstone-a733-pwm PROPERTY COMPILE_OPTIONS "${pwm_options}")
target_include_directories(sunstone-a733-pwm BEFORE PRIVATE "${SUNSTONE_SOURCE_ROOT}/apps/pwm")
target_include_directories(sunstone-a733-pwm PRIVATE ${pwm_includes})
target_compile_definitions(sunstone-a733-pwm PRIVATE ${pwm_definitions}
 SUNSTONE_BOARD_APPLICATION=1 A733_SMP_CONTRACT_MAGIC=0x4153373350574331ULL A733_SMP_RESULT_MAGIC=0x4153373350575231ULL)
if(pwm_links)
 target_link_libraries(sunstone-a733-pwm PRIVATE ${pwm_links})
endif()
file(GLOB pwm_headers "${SUNSTONE_SOURCE_ROOT}/apps/pwm/*.h")
set(pwm_inputs ${pwm_sources} ${pwm_headers} ${board_inputs}
 "${SUNSTONE_SOURCE_ROOT}/include/a733_pwm.h" "${SUNSTONE_SOURCE_ROOT}/include/a733_pwm_native.h" "${SUNSTONE_SOURCE_ROOT}/include/gicv3_spi.h"
 "${SUNSTONE_SOURCE_ROOT}/apps/pwm/boot.patch"
 "${SUNSTONE_SOURCE_ROOT}/apps/pwm/platform.patch" "${CMAKE_CURRENT_LIST_FILE}")
list(REMOVE_DUPLICATES pwm_inputs)
set(pwm_identity "native-pwm-service2;supervised60;PD5-40k-continuous;bounded-duty-stop-restart;PL7-GPIO;IRQ67;retained-low;${CMAKE_C_COMPILER_VERSION}")
foreach(input IN LISTS pwm_inputs)
 file(SHA256 "${input}" pwm_hash)
 string(APPEND pwm_identity "${pwm_hash}")
endforeach()
string(SHA256 pwm_build_id "${pwm_identity}")
string(REGEX REPLACE "(..)" "0x\\1," pwm_id_bytes "${pwm_build_id}")
file(WRITE "${pwm_build}/build-id.c" "const unsigned char a733_smp_build_id[32]={${pwm_id_bytes}};\n")
file(WRITE "${pwm_build}/build-id.txt" "${pwm_build_id}\n")
target_sources(sunstone-a733-pwm PRIVATE "${pwm_build}/build-id.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${pwm_inputs})
target_link_options(sunstone-a733-pwm PRIVATE -nostdlib -static -Wl,--gc-sections
 -Wl,--defsym=__uncached_pool_size=2097152 -Wl,--defsym=__device_pool_size=2097152
 "-Wl,-T,${storage_build}/link.ld" "-Wl,-Map,${pwm_build}/image.map")
set_target_properties(sunstone-a733-pwm PROPERTIES SUFFIX ".elf" LINK_DEPENDS "${storage_build}/link.ld")
set(pwm_manifest_script [=[
file(SHA256 "${IMAGE}" digest)
file(WRITE "@pwm_build@/manifest.json" "{\"target\":\"sunstone-a733-pwm\",\"execution_profile\":\"pwm-service-supervised-v1\",\"build_id\":\"@pwm_build_id@\",\"final_elf\":\"${digest}\"}\n")
]=])
string(CONFIGURE "${pwm_manifest_script}" pwm_manifest_script @ONLY)
file(WRITE "${pwm_build}/finish-manifest.cmake" "${pwm_manifest_script}")
add_custom_command(TARGET sunstone-a733-pwm POST_BUILD COMMAND "${CMAKE_COMMAND}"
 "-DIMAGE=$<TARGET_FILE:sunstone-a733-pwm>" -P "${pwm_build}/finish-manifest.cmake" VERBATIM)
