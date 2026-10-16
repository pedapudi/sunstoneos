# Current work

The active objective is [goal.md](goal.md). A FreeRTOS kernel-check image runs in
emulation, and a small SRAM probe has executed on the board. No physical OS image
boots yet and no complete hardware subsystem is qualified. Development is on branch `bringup` in the isolated
`/home/sunil/git/sunstoneos-worktrees/bringup` worktree.

## Observed host and device state

On 2026-10-15, USB enumeration reports an Allwinner FEL device, ID `1f3a:efe8`,
at bus 003 device 029. The FEL query identifies A733, and a returning SRAM probe
identifies the boot Cortex-A55 r2p0. [Boot evidence](boot.md) records the registers,
SRAM restoration and recovery-tool limitations. Board revision, RAM, and populated
storage remain unconfirmed.
The device node is owned by root and the session lacks passwordless sudo.
The owner granted a per-user ACL; read/write access is confirmed. The temporary
access helper and undo commands are documented in [tools](../tools/README.md).
USB addresses can change; identify the device again before access.

The separate `mimus keyboard` device is an existing ESP32 board. It must not be
used as the Orange Pi console. No Orange Pi UART adapter is currently visible.

The host has Clang 21, CMake, and LLVM object tools. LLD 21, ARM system emulation,
and USB/FDT development headers are extracted under `~/.cache/sunstoneos/host`.
Pinned sources, builds, packages, and raw evidence are under `~/.cache/sunstoneos`.
The [build instructions](build.md) describe the working configuration.

## Reusable validation evidence

On 2026-10-15, the USB access helper passed one focused check using real ACLs on
temporary host files. The check covered repeated grants, original ACL restoration,
and refusal to restore after an ACL change or device-node replacement. File
identity replaced character-device identity in this fixture. A read-only status
check confirmed access to the actual FEL node; its ACL was not modified by tests.

The [kernel check](build.md) passed 100 exchanges in 100 ticks on the final
emulator image. It covers timer preemption, blocking queue traffic and
floating-point context preservation. The [physical SRAM result](boot.md) is
separate evidence and does not establish FreeRTOS operation on the board.

## Next actions

1. Review the larger A733 SRAM layout against the 240 KiB board boot0 image.
   The original FEL patch's scratch and return code overlap this image.
2. Establish a DDR-only entry with return to FEL, instruction-cache maintenance,
   and no unknown storage boot path. The public FEL write bypasses its cache fix.
3. Establish the monitor/AArch64 handoff and adapt the kernel foundation to the
   board's interrupt controller, UART and memory layout.
4. Inspect mimus model/storage requirements and start its isolated port alongside
   the remaining hardware investigations.

Project issues [1–11](https://github.com/pedapudi/sunstoneos/issues) now track
implementation, dependencies and acceptance criteria. Their scope includes all
hardware and final qualification independently of mimus.

The first application milestone uses recorded audio. No microphone is available.
I2S and Bluetooth headset input remain required. OV5640 is the first camera;
the DSI panel is undecided. Missing hardware must not remove coverage requirements.
