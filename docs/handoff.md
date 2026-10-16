# Current work

The active objective is [goal.md](goal.md). DDR initialization and an AArch64
EL1 handoff probe work on the board. The probe returns to FEL with its DRAM result
intact. FreeRTOS passes the emulator kernel check; physical FreeRTOS startup
and mimus execution remain outstanding. No complete hardware subsystem is
qualified. Development uses branch `bringup` in the isolated
`/home/sunil/git/sunstoneos-worktrees/bringup` worktree.

## Observed host and device state

The attached FEL device (`1f3a:efe8`) identifies A733. The boot CPU identifies
as Cortex-A55 r2p0. DDR firmware reports 12 GiB LPDDR5, with transfers checked
at two distinct pages. [Boot evidence](boot.md) records the layout, registers,
artifact identities, and limitations. Board revision and populated storage
remain unconfirmed; no persistent-storage writes have been issued.

The latest observed node is `/dev/bus/usb/003/031`. The installed
[session access rule](../tools/README.md) grants access after FEL reconnects on
physical port `3-1`. The EL1 handoff check physically verified this behavior.
The rule lasts until host reboot and can be removed with `disable-session`.
Before another intentional reconnect, verify the rule and current permissions
with `python3 tools/usb-access.py check-session`.

The separate `mimus keyboard` device is an existing ESP32 board. It must not be
used as the Orange Pi console. No Orange Pi UART adapter is currently visible.

The host has Clang 21, CMake, and LLVM object tools. LLD 21, ARM system emulation,
and USB/FDT development headers are extracted under `~/.cache/sunstoneos/host`.
Pinned sources, builds, packages, and raw evidence are under `~/.cache/sunstoneos`.
The [build instructions](build.md) describe the emulator configuration.
The local Python environment has Unicorn 2.1.4 for the DDR wrapper check.

## Reusable validation evidence

The [kernel check](build.md) passed 100 exchanges in 100 ticks. Its inputs remain
unchanged. Reuse this result for the emulator platform.

The DDR wrapper check passed simulated success and failure returns, register and
FEL code preservation, parameter copying, and the logging trampoline. The
physical loader then initialized DDR and verified both RAM pages. The builder's
explicit compatibility guards produced identical payloads with Python `-O`.

The AArch64 probe completed at EL3 and returned to forced FEL. Its saved result
was read after access was restored; no repeat execution was needed. The EL1
handoff check then configured the counter frequency declaration to 24 MHz,
entered nonsecure EL1, read the counter, and returned through EL3 using SMC.
It passed once in QEMU and once on the board.

The USB session helper passed a focused test with real ACLs on temporary files.
It covers adoption of the previous grant, reconnect grants, unrelated-event
refusal, changed-ACL refusal, and undo with the device present or absent.
`udevadm verify` accepted the generated rule. During the EL1 check, the physical
board re-enumerated and the helper granted access automatically. The host read
the result through the new node without user intervention.

## Next actions

1. Extend the tested EL3-to-EL1 handoff with physical GIC setup and FreeRTOS
   startup. Adapt the kernel check to record results in DRAM and return to FEL.
2. Preserve storage before any persistent installation, and establish native
   USB diagnostics so continued operation does not depend on test-return code.
3. Inspect mimus model/storage requirements and start its isolated port alongside
   the remaining hardware investigations.

Project issues [1–11](https://github.com/pedapudi/sunstoneos/issues) track
implementation, dependencies and acceptance criteria. Their scope includes all
hardware and final qualification independently of mimus.

The first application milestone uses recorded audio. No microphone is available.
I2S and Bluetooth headset input remain required. OV5640 is the first camera;
the DSI panel is undecided. Missing hardware must not remove coverage requirements.
