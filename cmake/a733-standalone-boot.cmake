include("${CMAKE_CURRENT_LIST_DIR}/kernel-flags.cmake")
# Cold-entry monitor with the stage-2 slot loader (docs/boot-slots.md). The
# flat image is a boot0 monitor payload for tools/prepare-microsd-boot.py.
if(NOT SUNSTONE_SOURCE_ROOT)
  get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
set(stage2_root "${SUNSTONE_SOURCE_ROOT}")
find_program(SUNSTONE_OBJCOPY NAMES llvm-objcopy-21 llvm-objcopy
  HINTS /usr/lib/llvm-21/bin REQUIRED)
add_executable(a733-stage2 EXCLUDE_FROM_ALL
  "${stage2_root}/boot/a733/cold-entry.S"
  "${stage2_root}/boot/a733/stage2/loader.c"
  "${stage2_root}/boot/a733/stage2/admission.c"
  "${stage2_root}/drivers/storage/sunxi_mmc.c"
  "${stage2_root}/drivers/storage/storage_digest.c"
  "${stage2_root}/runtime/memory.c")
target_include_directories(a733-stage2 PRIVATE
  "${stage2_root}/include" "${stage2_root}/runtime/include")
target_compile_definitions(a733-stage2 PRIVATE A733_COLD_STAGE2=1)
# MMU and caches stay off at EL3: Device memory requires aligned accesses, and
# no FP/SIMD trap configuration exists before this code runs.
target_compile_options(a733-stage2 PRIVATE
  ${SUNSTONE_KERNEL_FLAGS} -Werror
  "$<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>")
target_link_options(a733-stage2 PRIVATE -nostdlib -static -Wl,--gc-sections
  "-Wl,-T,${stage2_root}/boot/a733/stage2/link.ld"
  "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/a733-stage2.map")
set_target_properties(a733-stage2 PROPERTIES SUFFIX ".elf"
  LINK_DEPENDS "${stage2_root}/boot/a733/stage2/link.ld")
add_custom_command(TARGET a733-stage2 POST_BUILD
  COMMAND "${SUNSTONE_OBJCOPY}" -O binary "$<TARGET_FILE:a733-stage2>"
    "${CMAKE_CURRENT_BINARY_DIR}/a733-stage2.bin"
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/a733-stage2.bin"
  VERBATIM)
