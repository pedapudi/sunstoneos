# Current work

The active objective is [goal.md](goal.md). FreeRTOS now runs on the physical
A733 boot CPU. The kernel check passes timer preemption, blocking queues, and
floating-point context checks. An EL3 secure-timer timeout recovers a forced
EL1 stall and returns to FEL. [Physical kernel evidence](a733-kernel.md) records
the image, resources, measurements, and limitations.

A native USB channel now passes status, packet-boundary echo, log, and commanded
ROM return checks. [USB evidence](usb.md) records the implementation and limits.
The [C++ runtime](runtime.md) passes a physical check for allocation, containers,
constructors, and task-local newlib state.
Persistent boot, mimus execution, other CPU cores, and
complete hardware qualification remain outstanding. No complete hardware
subsystem is qualified. Development uses branch `bringup` in the isolated
`/home/sunil/git/sunstoneos-worktrees/bringup` worktree.

## Observed host and device state

The attached FEL device (`1f3a:efe8`) identifies A733. The boot CPU identifies
as Cortex-A55 r2p0. DDR firmware reports 12 GiB LPDDR5, with transfers checked
at two distinct pages. [Boot evidence](boot.md) records initialization and the
EL1 handoff. Board revision and populated storage remain unconfirmed.
No persistent-storage writes have been issued.

The board returned to FEL after the C++ runtime check. The installed
[session access rule](../tools/README.md) grants access after reconnects on
physical port `3-1`. Kernel and native USB checks regained access automatically.
The rule lasts until host reboot and can be removed with `disable-session`.
Rediscover the node and run `python3 tools/usb-access.py check-session` before
an intentional reconnect.

The separate `mimus keyboard` device is an existing ESP32 board. It must not be
used as the Orange Pi console. No Orange Pi UART adapter is currently visible.

The host has Clang 21, CMake, and LLVM object tools. LLD 21, ARM system emulation,
and USB/FDT development headers are extracted under `~/.cache/sunstoneos/host`.
Pinned sources, builds, packages, and raw evidence are under `~/.cache/sunstoneos`.
The [build instructions](build.md) describe the emulator configuration.
The local Python environment supplies the dependencies in
`checks/host-requirements.txt` for USB transport and compiled-code checks.

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

The USB image passed one bounded physical session after corrections to control
completion and zero-length IN submission. The monitor renewal model and focused
compiled-driver check also passed. Their identities and limits are in the USB
document. The runtime renews the secure deadline while its USB task runs.
The resident mode is implemented but has not been physically tested.

The C++ runtime passed its secure-monitor emulator check and one physical
session. The physical session exercised two allocating tasks for 100 rounds
each and returned to ROM automatically. [Runtime evidence](runtime.md) records
source identities, library dependencies, memory use, and limits. Mimus itself
has not been modified or executed on the A733.

## Next actions

1. Extend the USB transport for application loading and device diagnostics.
   Preserve bounded recovery during controller work. Add interrupt delivery,
   cold clock/PHY setup, and the remaining USB roles through separate checks.
2. Identify and preserve populated storage before persistent installation.
   Add storage access and recovery without overwriting existing contents.
3. Add mimus clock and task-identity adapters, an explicit Orange Pi model
   profile, and model/audio loading. Run recorded-audio parity before optimizing
   arithmetic or adding workers. Keep accelerator and remaining driver work
   active independently of this application milestone.
4. Extend CPU and memory support to secondary cores, usable capacity, protection,
   and coherence, with checks selected from each change.

Project issues [1–11](https://github.com/pedapudi/sunstoneos/issues) track
implementation, dependencies and acceptance criteria. Their scope includes all
hardware and final qualification independently of mimus.

No microphone is available. I2S and Bluetooth headset input remain required.
OV5640 is the first camera; the DSI panel is undecided. Missing hardware must
not remove coverage requirements.
