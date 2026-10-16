# Current work

The active objective is [goal.md](goal.md). FreeRTOS now runs on the physical
A733 boot CPU. The kernel check passes timer preemption, blocking queues, and
floating-point context checks. An EL3 secure-timer timeout recovers a forced
EL1 stall and returns to FEL. [Physical kernel evidence](a733-kernel.md) records
the image, resources, measurements, and limitations.

Persistent boot, native USB diagnostics, mimus execution, other CPU cores, and
complete hardware qualification remain outstanding. No complete hardware
subsystem is qualified. Development uses branch `bringup` in the isolated
`/home/sunil/git/sunstoneos-worktrees/bringup` worktree.

## Observed host and device state

The attached FEL device (`1f3a:efe8`) identifies A733. The boot CPU identifies
as Cortex-A55 r2p0. DDR firmware reports 12 GiB LPDDR5, with transfers checked
at two distinct pages. [Boot evidence](boot.md) records initialization and the
EL1 handoff. Board revision and populated storage remain unconfirmed.
No persistent-storage writes have been issued.

The board returned to FEL after the kernel check. The installed
[session access rule](../tools/README.md) grants access after reconnects on
physical port `3-1`. Both physical kernel cases regained access automatically.
The rule lasts until host reboot and can be removed with `disable-session`.
Rediscover the node and run `python3 tools/usb-access.py check-session` before
an intentional reconnect.

The separate `mimus keyboard` device is an existing ESP32 board. It must not be
used as the Orange Pi console. No Orange Pi UART adapter is currently visible.

The host has Clang 21, CMake, and LLVM object tools. LLD 21, ARM system emulation,
and USB/FDT development headers are extracted under `~/.cache/sunstoneos/host`.
Pinned sources, builds, packages, and raw evidence are under `~/.cache/sunstoneos`.
The [build instructions](build.md) describe the emulator configuration.
The local Python environment has Unicorn 2.1.4 for the DDR wrapper check.

## Reusable validation evidence

The DDR wrapper passed its simulated control-flow check and physical
initialization. The explicit compatibility guards produced identical payloads
with Python `-O`. DDR and the EL1 handoff have not changed since those checks.

The secure-monitor kernel model passed normal execution and a forced EL1 stall.
The physical image then passed both cases without rebuilding between them.
The normal case completed 100 exchanges in 100 ticks. The forced stall returned
through secure timer interrupt 29 after approximately two seconds. The monitor
restored the modified GIC settings before returning to ROM.

The USB session helper passed its focused ACL lifecycle check and rule validation.
Automatic access after physical re-enumeration passed during the EL1 handoff and
both kernel cases. No access-helper changes require another check.

The original nonsecure QEMU platform remains available. Its earlier check passed
100 exchanges in 100 ticks. The added secure-monitor configuration is selected
by a separate build definition; the original configuration remains in the
other preprocessor branch. The physical and secure-model results cover the
new startup and configuration.

## Next actions

1. Establish native USB diagnostics and a persistent runtime loop. The current
   monitor arms a two-second recovery timer for bounded kernel checks; a running
   OS needs an explicit policy for servicing or disarming it.
2. Identify and preserve populated storage before persistent installation.
   Add storage access and recovery without overwriting existing contents.
3. Inspect mimus model/storage requirements and implement its isolated FreeRTOS
   port using recorded audio. Keep accelerator and remaining hardware driver
   investigations active independently of this application milestone.
4. Extend CPU and memory support to secondary cores, usable capacity, protection,
   and coherence, with checks selected from each change.

Project issues [1–11](https://github.com/pedapudi/sunstoneos/issues) track
implementation, dependencies and acceptance criteria. Their scope includes all
hardware and final qualification independently of mimus.

No microphone is available. I2S and Bluetooth headset input remain required.
OV5640 is the first camera; the DSI panel is undecided. Missing hardware must
not remove coverage requirements.
