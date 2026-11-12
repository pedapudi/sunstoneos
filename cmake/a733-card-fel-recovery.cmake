set(card_fel_root "${SUNSTONE_SOURCE_ROOT}")
set(card_fel_build "${CMAKE_CURRENT_BINARY_DIR}/card-fel-recovery")
set(SUNSTONE_CARD_BOOT0 "$ENV{HOME}/.cache/sunstoneos/sources/board-boot/boot0_sdcard_orangepizero3w.fex"
    CACHE FILEPATH "Pinned boot0 input for the ordinary-file recovery package")
find_program(card_fel_objcopy NAMES llvm-objcopy-21 llvm-objcopy REQUIRED)
file(MAKE_DIRECTORY "${card_fel_build}")
add_custom_command(OUTPUT "${card_fel_build}/gate.elf" "${card_fel_build}/gate.bin"
 COMMAND "${CMAKE_C_COMPILER}" --target=arm-none-eabi -march=armv8-a -marm -c
   -I "${card_fel_root}/include"
   "${card_fel_root}/boot/a733/card-fel-gate.S" -o "${card_fel_build}/gate.o"
 COMMAND "${CMAKE_LINKER}" -Ttext=0x52000 --entry=a733_card_fel_gate
   "${card_fel_build}/gate.o" -o "${card_fel_build}/gate.elf"
 COMMAND "${card_fel_objcopy}" -O binary "${card_fel_build}/gate.elf" "${card_fel_build}/gate.bin"
 DEPENDS "${card_fel_root}/boot/a733/card-fel-gate.S"
   "${card_fel_root}/include/a733_card_fel_gate.h" VERBATIM)
add_custom_command(OUTPUT "${card_fel_build}/gate-info.h" "${card_fel_build}/boot0-fel-first-stage.fex"
   "${card_fel_build}/first-stage.json"
 COMMAND "${Python3_EXECUTABLE}" -B "${card_fel_root}/tools/card-fel-image.py"
   --gate-elf "${card_fel_build}/gate.elf" --gate-bin "${card_fel_build}/gate.bin"
   --boot0 "${SUNSTONE_CARD_BOOT0}" --output "${card_fel_build}"
 DEPENDS "${card_fel_build}/gate.elf" "${card_fel_build}/gate.bin" "${SUNSTONE_CARD_BOOT0}"
   "${card_fel_root}/tools/card-fel-image.py" VERBATIM)
# One proof image per entry state: the FEL request (1) and a warm reset with
# the cold-boot marker set (2). The gate bytes are identical.
foreach(card_fel_proof IN ITEMS "recovery;1" "warm;2")
 list(GET card_fel_proof 0 card_fel_name)
 list(GET card_fel_proof 1 card_fel_entry)
 add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/a733-card-fel-${card_fel_name}.elf"
  COMMAND "${CMAKE_C_COMPILER}" --target=arm-none-eabi -march=armv8-a -marm -c
    -I "${card_fel_root}/include" -I "${card_fel_build}"
    -DCARD_FEL_GATE_BINARY="${card_fel_build}/gate.bin" -DCARD_FEL_PROOF_ENTRY=${card_fel_entry}
    "${card_fel_root}/apps/card-fel-recovery/entry.S" -o "${card_fel_build}/proof-${card_fel_name}.o"
  COMMAND "${CMAKE_LINKER}" -T "${card_fel_root}/apps/card-fel-recovery/link.ld"
    "${card_fel_build}/proof-${card_fel_name}.o" -o "${CMAKE_CURRENT_BINARY_DIR}/a733-card-fel-${card_fel_name}.elf"
  DEPENDS "${card_fel_build}/gate-info.h" "${card_fel_build}/gate.bin"
    "${card_fel_root}/apps/card-fel-recovery/entry.S" "${card_fel_root}/apps/card-fel-recovery/link.ld"
    "${card_fel_root}/include/a733_card_fel_gate.h" VERBATIM)
endforeach()
add_custom_target(a733-card-fel-recovery
 DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/a733-card-fel-recovery.elf"
   "${CMAKE_CURRENT_BINARY_DIR}/a733-card-fel-warm.elf")
