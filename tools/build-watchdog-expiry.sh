#!/bin/bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
probe_build="${SUNSTONE_WATCHDOG_BUILD:-$HOME/.cache/sunstoneos/build/watchdog-expiry}"
mkdir -p "$probe_build"
probe_flags=(--target=arm-none-eabi -march=armv8-a -marm -mfloat-abi=soft
  -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
  -fno-asynchronous-unwind-tables -Os -g -Wall -Wextra -Werror
  -Iinclude -Iplatform/a733)
clang-21 "${probe_flags[@]}" -c checks/a733-watchdog-expiry.S -o "$probe_build/entry.o"
clang-21 "${probe_flags[@]}" -c checks/a733-watchdog-expiry.c -o "$probe_build/probe.o"
clang-21 "${probe_flags[@]}" -c drivers/watchdog/a733_rom_return.c -o "$probe_build/return.o"
clang-21 "${probe_flags[@]}" -c drivers/watchdog/a733_watchdog.c -o "$probe_build/watchdog.o"
"$HOME/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld" -T checks/a733-watchdog-expiry.ld "$probe_build/entry.o" "$probe_build/probe.o" "$probe_build/return.o" "$probe_build/watchdog.o" -o "$probe_build/probe.elf"
/usr/lib/llvm-21/bin/llvm-objcopy -O binary "$probe_build/probe.elf" "$probe_build/probe.bin"
/usr/lib/llvm-21/bin/llvm-size "$probe_build/probe.elf"
sha256sum "$probe_build/probe.bin"
