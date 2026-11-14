include("${CMAKE_CURRENT_LIST_DIR}/kernel-flags.cmake")
# Cold-entry monitor with the stage-2 slot loader (docs/boot-slots.md). The
# flat image is a boot0 monitor payload for tools/prepare-microsd-boot.py.
if(NOT SUNSTONE_SOURCE_ROOT)
  get_filename_component(SUNSTONE_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
set(stage2_root "${SUNSTONE_SOURCE_ROOT}")
find_program(SUNSTONE_OBJCOPY NAMES llvm-objcopy-21 llvm-objcopy
  HINTS /usr/lib/llvm-21/bin REQUIRED)
# a733-stage2 enters the slot's OS. a733-stage2-armed-stop is the diagnostic
# build (docs/designs/card-boot-chain.md): in a boot-once boot it arms 0x41
# from mode 0 and parks without entering the OS; in any other cold boot it
# parks under the card first stage's 16 s watchdog.
function(sunstone_a733_stage2 name)
  add_executable(${name} EXCLUDE_FROM_ALL
    "${stage2_root}/boot/a733/cold-entry.S"
    "${stage2_root}/boot/a733/stage2/loader.c"
    "${stage2_root}/boot/a733/stage2/admission.c"
    "${stage2_root}/boot/a733/stage2/cpu.c"
    "${stage2_root}/platform/a733/remote_writers.c"
    "${stage2_root}/drivers/storage/sunxi_mmc.c"
    "${stage2_root}/drivers/storage/storage_digest.c"
    "${stage2_root}/runtime/memory.c")
  target_include_directories(${name} PRIVATE
    "${stage2_root}/include" "${stage2_root}/runtime/include")
  # The loader only reads and uses 3.3 V high-speed four-bit SD; leave the write and UHS-I code out.
  target_compile_definitions(${name} PRIVATE A733_COLD_STAGE2=1 SUNXI_MMC_NO_UHS=1 SUNXI_MMC_READ_ONLY=1 SUNXI_MMC_NO_IRQ=1 ${ARGN})
  # MMU and caches stay off at EL3: Device memory requires aligned accesses, and
  # no FP/SIMD trap configuration exists before this code runs.
  target_compile_options(${name} PRIVATE
    ${SUNSTONE_KERNEL_FLAGS} -Werror
    "$<$<COMPILE_LANGUAGE:C>:-mgeneral-regs-only>")
  target_link_options(${name} PRIVATE -nostdlib -static -Wl,--gc-sections
    "-Wl,-T,${stage2_root}/boot/a733/stage2/link.ld"
    "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/${name}.map")
  set_target_properties(${name} PROPERTIES SUFFIX ".elf"
    LINK_DEPENDS "${stage2_root}/boot/a733/stage2/link.ld")
  add_custom_command(TARGET ${name} POST_BUILD
    COMMAND "${SUNSTONE_OBJCOPY}" -O binary "$<TARGET_FILE:${name}>"
      "${CMAKE_CURRENT_BINARY_DIR}/${name}.bin"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/${name}.bin"
    VERBATIM)
endfunction()
sunstone_a733_stage2(a733-stage2)
sunstone_a733_stage2(a733-stage2-armed-stop A733_STAGE2_STOP_ARMED=1)

# Keep cold ownership within the fixed monitor reservation. SD and hashing
# retain the shared -O2 setting; these control paths run only during boot.
set_source_files_properties(
  "${stage2_root}/drivers/storage/sunxi_mmc.c"
  PROPERTIES COMPILE_OPTIONS "-Os")
set_source_files_properties(
  "${stage2_root}/boot/a733/stage2/loader.c"
  "${stage2_root}/boot/a733/stage2/cpu.c"
  PROPERTIES COMPILE_OPTIONS "-Oz")
# Admission includes the fixed predecessor checks; size optimization keeps
# the report ABI unchanged. The SD transfer and digest implementations stay O2.
set_source_files_properties("${stage2_root}/boot/a733/stage2/admission.c"
  PROPERTIES COMPILE_OPTIONS "-Oz")
