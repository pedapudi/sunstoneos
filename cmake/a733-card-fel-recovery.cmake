set(card_fel_root "${SUNSTONE_SOURCE_ROOT}")
set(card_fel_build "${CMAKE_CURRENT_BINARY_DIR}/card-fel-recovery")
set(SUNSTONE_CARD_BOOT0 "$ENV{HOME}/.cache/sunstoneos/sources/board-boot/boot0_sdcard_orangepizero3w.fex"
    CACHE FILEPATH "Pinned boot0 input for the ordinary-file recovery package")
find_program(card_fel_objcopy NAMES llvm-objcopy-21 llvm-objcopy REQUIRED)
file(MAKE_DIRECTORY "${card_fel_build}")
add_custom_command(OUTPUT "${card_fel_build}/gate.elf" "${card_fel_build}/gate.bin"
 COMMAND "${CMAKE_C_COMPILER}" --target=arm-none-eabi -march=armv8-a -marm -c
   "${card_fel_root}/boot/a733/card-fel-gate.S" -o "${card_fel_build}/gate.o"
 COMMAND "${CMAKE_LINKER}" -Ttext=0x41001000 --entry=a733_card_fel_gate
   "${card_fel_build}/gate.o" -o "${card_fel_build}/gate.elf"
 COMMAND "${card_fel_objcopy}" -O binary "${card_fel_build}/gate.elf" "${card_fel_build}/gate.bin"
 DEPENDS "${card_fel_root}/boot/a733/card-fel-gate.S" VERBATIM)
add_custom_command(OUTPUT "${card_fel_build}/gate-info.h" "${card_fel_build}/boot0-fel-first-stage.fex"
   "${card_fel_build}/first-stage.json"
 COMMAND "${Python3_EXECUTABLE}" -B "${card_fel_root}/tools/card-fel-image.py"
   --gate-elf "${card_fel_build}/gate.elf" --gate-bin "${card_fel_build}/gate.bin"
   --boot0 "${SUNSTONE_CARD_BOOT0}" --output "${card_fel_build}"
 DEPENDS "${card_fel_build}/gate.elf" "${card_fel_build}/gate.bin" "${SUNSTONE_CARD_BOOT0}"
   "${card_fel_root}/tools/card-fel-image.py" VERBATIM)
add_executable(a733-card-fel-recovery EXCLUDE_FROM_ALL
 "${card_fel_root}/apps/card-fel-recovery/entry.S"
 "${card_fel_root}/apps/card-fel-recovery/admission.c"
 "${card_fel_root}/apps/card-fel-recovery/profile.c"
 "${card_fel_build}/gate-info.h")
target_include_directories(a733-card-fel-recovery PRIVATE "${card_fel_build}" "${card_fel_root}/include")
target_compile_definitions(a733-card-fel-recovery PRIVATE
 CARD_FEL_GATE_BINARY="${card_fel_build}/gate.bin")
target_compile_options(a733-card-fel-recovery PRIVATE -O2 -ffreestanding -fno-builtin -mgeneral-regs-only
 "$<$<COMPILE_LANGUAGE:C>:-fstack-usage;-fno-stack-protector>")
target_link_options(a733-card-fel-recovery PRIVATE -nostdlib -static -Wl,--gc-sections
 "-Wl,-T,${card_fel_root}/apps/card-fel-recovery/link.ld")
set_target_properties(a733-card-fel-recovery PROPERTIES SUFFIX ".elf"
 LINK_DEPENDS "${card_fel_root}/apps/card-fel-recovery/link.ld")
