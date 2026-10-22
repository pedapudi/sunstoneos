# Independent hardware reset still needs physical validation

The secure CPU timer recovers the tested EL1 software stall, but the UFS probe
did not return to ROM. A peripheral transaction can potentially stall the CPU
before it services the secure interrupt. The exact instruction that stopped the
UFS probe is unknown because its retained result is inaccessible.

Watchdog reset qualification is deferred. Supervised RAM-only peripheral tests
may proceed under the [hardware access rules](../AGENTS.md#hardware-access),
with a reviewed manual-reconnection plan and their actual admission requirements.
Intentional watchdog expiry and unattended fault tests still require a qualified
reset destination and recovery path. The failed USB instance is recorded in
`~/.cache/sunstoneos/recovery-required.json`. Host loaders using
`require_rom()` refuse that instance before sending a request, even if sysfs
still contains its ROM descriptor. A new instance must pass normal ROM checks.
The record is retained for diagnosis and does not block a distinct instance.

## Launchers arm the watchdog for runtime images

Every runtime image that runs under the watchdog finds WDT0 already armed by
the code that admitted it: an SRAM launcher loaded over FEL, or the stage-2
slot loader. A FEL launcher writes the RTC FEL request `0x5aa5a55a` to
`0x07090108`, the CPU0 ROM redirect flag `0xfa50392f` to `0x070501c0` and the
redirect entry `0x20` to `0x070501c4`. It then writes output `2` and
configuration `1` (system reset) under key `0x16aa0000`, and mode `0x41`, which
enables the four-second interval. Restart word `0x14af` starts the count. It
reads each value back, records the arming time in the contract and never
disarms the timer.

Each of five files writes these register values with its own literals:

- [platform/a733-smp/launch.c](../platform/a733-smp/launch.c), the SMP
  diagnostic launcher;
- [platform/a733-smp-observe/launch.c](../platform/a733-smp-observe/launch.c),
  which must stay textually equal to the SMP launcher apart from its request
  magic;
- [platform/a733-storage-launch/launch.c](../platform/a733-storage-launch/launch.c),
  the storage-maintenance and native mimus launcher;
- [platform/a733-device-launch/launch.c](../platform/a733-device-launch/launch.c),
  the extended device-memory launcher;
- [boot/a733/stage2/loader.c](../boot/a733/stage2/loader.c), the stage-2 slot
  loader, which omits the FEL and redirect writes.

The maintenance reload verifier in
[platform/a733/maintenance_watchdog.c](../platform/a733/maintenance_watchdog.c)
and the board contract validators for the armed and resident lifetimes require
mode `0x41`, configuration `1` and output `2`. A different interval therefore
needs the launchers, the reload verifier, the contract validators and their
host checks changed together. A missed site makes the verifier or the contract
admission refuse, which stops the image and leaves the armed watchdog to reset
the board.

The launchers do not share a header. The host tools that build the storage
and device launchers copy only each launcher's own files into an isolated
directory, and a host check compares the SMP launcher as text with the observe
launcher.

## Save-and-restore driver for the expiry diagnostic

[drivers/watchdog/a733_watchdog.c](../drivers/watchdog/a733_watchdog.c) arms
WDT0 only in the [protected expiry diagnostic](watchdog-expiry.md); no runtime
image links it, and `a733_watchdog_feed` has no caller outside the host check.
Unlike the launchers, it keeps the other mode bits, can disarm, and leaves the
ROM redirect registers untouched.

The driver programs the A733 watchdog at `0x02050000`. It uses the keyed
configuration and mode registers, supports the documented discrete timeouts
from 1 to 16 seconds, and refuses an already enabled watchdog. Callers must
zero-initialize the context and provide exclusive ownership.

Arming saves the previous configuration and writes the bootloader's FEL request
value to RTC register `0x07090108`. It selects system reset and the reset-pulse
duration setting used by the A733 bootloader. The manual defines that setting
as pulse duration; it does not establish power-controller or redirect retention.
The driver checks the reset configuration before enabling the timer. A failed
configuration readback restores saved state without starting the watchdog.
Disarming disables the watchdog before restoring its configuration and RTC flag.
If disabling fails, the driver retains the FEL request and reports failure.
The RTC flag is volatile boot state; this operation does not program storage.

The RTC flag is handled by boot0. It does not establish recovery independent
of the installed storage bootloader. Register reads now pass on the physical
board. Reset-policy programming and restoration also pass the configuration
trial below. Timer enabling, expiry, reset scope, and register retention
remain untested. Treat DDR as uninitialized after
watchdog reset. The existing warm CPU-reset loader cannot establish this reset
path by reading an old DDR result.

The separate [protected expiry diagnostic](watchdog-expiry.md) implements
explicit enable/disarm and expiry phases. Its instruction-model and host-protocol
checks pass; both physical requests remain pending in the board queue. The
physical BOOT/GND connection must be confirmed before either phase.
The [reset-evidence investigation](watchdog-reset-evidence.md) distinguishes
watchdog interrupt status, software reset, and ROM-written progress records.
None currently supplies a qualified retained watchdog-cause result.

## ROM recovery candidate

Disassembly of the pinned board boot0 identifies the FEL-flag reader at
`0x4a990`. Its caller at `0x5e1ae` clears the flag through `0x4aa08`, then
branches to ROM entry `0x20` through the routine at `0x48ec6`. This path depends
on boot0 being loaded and executing. The boot0 hash and provenance are in
[sources.lock.json](../sources.lock.json) and [sources.md](sources.md).

The captured board ROM contains another route that executes before storage boot:

| ROM address | Observed behavior |
| --- | --- |
| `0x164` | Read `0x070501c0` and compare it with `0xfa50392f` on CPU0 |
| `0xcc` | On a match, load the program counter from `0x070501c4` |
| `0x20` | Forced-FEL entry: initialize RTC access, clear SRAM, select FEL |
| `0x1f0` | Check a separate dispatch flag at `0x070501d4`; low bits `0xefe8` redirect through `0x070501d8` |
| `0x22c` | Branch to the FEL dispatcher at `0x2514` when forced FEL is selected |

The reference bootloader names the block at `0x07050000` CPUIDLE. Setting its
CPU0 redirect entry to `0x20` is a candidate for recovery without executing
boot0. The flag and entry must survive the selected reset. Reads, writes, and
restoration pass; retention and reset entry remain untested.
The separate dispatch flag must also be inactive; it can override this route.

[checks/a733-rom-recovery.py](../checks/a733-rom-recovery.py) accepts pinned
hashes for the published ROM and the captured board ROM. It executes each
selected ROM prefix in Unicorn and stops at the boot or FEL dispatcher. Normal reset
and normal reset with the bootloader's RTC flag both reach the boot dispatcher.
Explicit entry at `0x20` and CPU0 redirection to that entry reach the FEL
dispatcher. An active alternate dispatch flag overrides CPU0 redirection.

The check uses a Cortex-A15 instruction model and skips the NSACR write at
`0x1c4`, which Unicorn rejects. NSACR controls nonsecure access; its hardware
effects remain outside this check. No subsequent branch in the tested prefix
reads it. The check does not simulate watchdog expiry, register retention,
security permissions, clocks, or USB initialization.

The earlier published-ROM check passed with decoded ROM SHA-256
`3a2a7d289951f61095afdabe7813b067295223ce588c422d64200fc7e4431eb1`.
Raw output is in
`~/.cache/sunstoneos/evidence/rom-recovery-20261016/control-flow.json`.

```sh
~/.cache/sunstoneos/venv/bin/python checks/a733-rom-recovery.py \
  ~/.cache/sunstoneos/sources/brom/fixs-brom.bin
```

An eventual hardware loader must compare the relevant ROM bytes before using
this route, refuse an existing redirect owner or alternate dispatch, and save
the original register values on the host. It must write and verify the entry
before enabling the redirect flag. Restoration after reconnect must disable
the redirect before restoring its entry. A watchdog expiry test also needs a
verified way to prevent unintended storage boot if the redirect is lost.

## Captured board ROM and reset ownership

The reconnected board answered the A733 identity request on USB instance
`003:064`, with automatic access on port `3-1`. A direct FEL read at address
zero returned rejection status `0x01`. The tool consumed the error status and
its USB transaction response; this was not a transport timeout or disappearance.

The FEL tool's `memmove` command executes a CPU copy through SRAM and can read
ROM. The capture saved and restored the helper workspace at `0x71000` and the
destination at `0x52000`, with matching readback after restoration. It first
copied 1,024 bytes, then captured the remaining bytes through `0x10fff` after
the prefix comparison found a different ROM revision. No reset was issued.

The 69,632-byte physical capture has SHA-256
`a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa`.
Thirteen words in its first 1,024 bytes differ from the published reference.
The reset and redirect instructions through `0x228` match. The instruction at
`0x22c` branches to `0x2514`; the published ROM branches to `0x2330`.
Earlier five-word checks of forced-FEL entry remain valid but did not establish
complete ROM identity. Use the captured image for further board-specific analysis.

A read-only reset-register snapshot found the CPU redirect flag, CPU0 entry,
alternate flag, and alternate entry all zero. Watchdog configuration was `1`,
mode was `0` (disabled), and reset-output configuration was `0x1f`. The
bootloader FEL flag was zero. These values establish the observed ownership
state; a loader must check again before modifying registers.

The physical ROM's boot-input routine at `0x10134` samples bit 8 of
`0x03000024` four times. Four low samples select FEL before the normal storage
path, even without a CPU redirect. Any high sample continues toward storage
boot. The live register value was `0x101`, so the sampled bit was high.
The manufacturer documents BOOT and GND test pads connected to this input.
[Board wiring and physical recovery](board-wiring.md) pins the schematic and
manual, identifies the pads, and gives a bounded input check. Assertion on the
attached board and behavior across watchdog reset remain unqualified.

The user confirmed automatic FEL entry after disconnecting and reattaching USB,
with a microSD card inserted and no button held. This establishes the observed
power-cycle outcome. It does not establish watchdog-reset behavior or prove
that the card cannot boot under a different initialization state. Preserve the
card and its contents while establishing a protected reset destination.

One focused instruction-model run passed nine cases on the physical capture:
the five reset/redirect cases above, low boot input, high boot input, one mixed
sample, and low boot input with the secure indicator set. The secure case leaves
the separate security-policy bits clear; it does not cover every secure-boot
configuration. The model executes the inspected boot-input code and preceding
initialization without substituting function calls. NSACR remains the sole
skipped instruction. MMIO values are synthetic, so these results do not prove
electrical behavior, register retention, or physical watchdog recovery.

Raw captures, transaction logs, register values, the focused model result, and
input hashes reside under
`~/.cache/sunstoneos/evidence/rom-copy-20261016-084055/`.
The prior direct-read rejection is recorded separately under
`~/.cache/sunstoneos/evidence/reconnected-rom-20261016-083552/`.
The unchanged watchdog, kernel, application, and NPU suites were not repeated.

## Focused host evidence

The driver compiled for AArch64. The host register model passed checks for
invalid timeout refusal, existing-owner refusal, feed behavior, restoration,
failed FEL-flag writes, failed configuration writes, and failed disarming.
Ignored reset-configuration and reset-output writes both leave the timer disabled.
These checks establish software behavior only; they do not qualify hardware.

```sh
clang -std=c11 -O2 -Wall -Wextra -Werror -Iinclude -Iplatform/a733 \
  checks/watchdog.c -o ~/.cache/sunstoneos/build/watchdog-check
~/.cache/sunstoneos/build/watchdog-check
clang --target=aarch64-none-elf -march=armv8-a -ffreestanding -O2 \
  -Wall -Wextra -Werror -Iinclude -Iplatform/a733 \
  -c drivers/watchdog/a733_watchdog.c \
  -o ~/.cache/sunstoneos/build/watchdog-a733.o
```

`checks/usb-disconnect.py` also passed the new failed-instance refusal case and
acceptance of a distinct instance with the expected ROM descriptor. This host
check contacts no USB device.

## Deferred physical qualification

The SRAM configuration trial below passed on its recorded board instance.
The [protected expiry diagnostic](watchdog-expiry.md) is implemented, but its
physical qualification is deferred. Retain its code and evidence for future
qualification; it is not the next development task.

First establish a reset destination that cannot execute unknown storage code.
The documented BOOT-to-GND connection provides a candidate independent of
CPU redirect retention; physically verify its assertion before using it.
Then test timer enabling and disarming, followed by expiry with CPU timer
recovery disabled. Verify elapsed time, a new ROM
instance, automatic access, and the retained result. Reinitialize DDR before
resuming normal probes. Integrate watchdog arming before risky peripheral access
only after the physical reset path passes.

The [pinned sources](sources.md) provide the register references: kernel
`bsp/drivers/watchdog/sunxi_wdt.c` uses the `wdt-v103` layout; bootloader
`arch/arm/mach-sunxi/board_sun60iw2.c` supplies reset-output configuration;
`arch/arm/mach-sunxi/rtc.c` supplies the FEL flag and RTC index.

## SRAM configuration and restoration pass on hardware

The [ROM-return helper](../drivers/watchdog/a733_rom_return.c) prepares CPU0
redirection to ROM entry `0x20`. It refuses an active watchdog or an existing
redirect/alternate-dispatch owner. It verifies the entry before activating the
flag. Restoration refuses an active watchdog, clears and verifies the flag,
then restores the original entry. Failed restoration retains the context.
Callers need exclusive ownership, a zero-initialized context, and a verified
ROM identity. This helper does not establish reset retention.

The [SRAM diagnostic](../checks/a733-watchdog-configuration.c) exercises this
helper and the keyed watchdog configuration/output registers. It never writes
the watchdog enable or feed register. The entry wrapper preserves the FEL
caller registers, stack pointer, and CPSR, using a private 4 KiB SRAM stack.
The linked image is 832 bytes with no static data, BSS, or undefined symbols.
Its SHA-256 is
`01a47f9bd1cc3c61353c988f0b34a9168d3311db591ba63ddd9b538009dd09eb`.
The helper also builds for AArch64: 264 text bytes, no data or BSS.

The [compiled-image check](../checks/watchdog-configuration.py) passes twelve
cases covering restoration, invalid requests, existing owners, ignored writes,
failed restoration, and a watchdog becoming active before restoration. It
checks caller state, private-stack guards, redirect write ordering, and the
absence of writes to watchdog enable/feed registers. An initial wrapper restored
the return address through PC but left LR changed; the final wrapper restores
LR explicitly. Only the affected wrapper and focused model were rebuilt or
rerun for that correction.

One [physical configuration run](../tools/check-watchdog-configuration.py)
passed on USB instance `003:064`. The runner pins the image, FEL tool, and
physical ROM capture; it compares the reset prefix before execution. It saves
registers and SRAM, checks the uploaded image and request, reads the diagnostic
result, and compares restored registers and SRAM. The trial set the redirect
to `0x20` and reset output to `2`, then restored the original zero redirect and
output `0x1f`. Watchdog configuration remained `1`, and mode remained `0`.
The alternate dispatch and bootloader RTC flag were unchanged. No reset,
storage access, or performance measurement occurred.

Raw build commands, image, model result, transaction log, backups, readbacks,
and source hashes are under
`~/.cache/sunstoneos/evidence/watchdog-configuration-20261016-085706/`.
These results do not qualify timer enabling, feeding, expiry, reset retention,
or autonomous recovery from a peripheral bus stall.

Build the isolated diagnostic from the worktree:

```sh
probe_build="$HOME/.cache/sunstoneos/build/watchdog-configuration"
mkdir -p "$probe_build"
probe_flags=(--target=arm-none-eabi -march=armv8-a -marm -mfloat-abi=soft
  -ffreestanding -fno-builtin -fno-stack-protector -fno-unwind-tables
  -fno-asynchronous-unwind-tables -Os -g -Wall -Wextra -Werror
  -Iinclude -Iplatform/a733)
clang "${probe_flags[@]}" -c checks/a733-watchdog-configuration.S -o "$probe_build/entry.o"
clang "${probe_flags[@]}" -c checks/a733-watchdog-configuration.c -o "$probe_build/probe.o"
clang "${probe_flags[@]}" -c drivers/watchdog/a733_rom_return.c -o "$probe_build/return.o"
"$HOME/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld" \
  -T checks/a733-watchdog-configuration.ld "$probe_build/entry.o" \
  "$probe_build/probe.o" "$probe_build/return.o" -o "$probe_build/probe.elf"
/usr/lib/llvm-21/bin/llvm-objcopy -O binary "$probe_build/probe.elf" "$probe_build/probe.bin"
```

The physical runner is intentionally pinned to the checked image. A changed
image requires review and focused validation before updating its pin. Reuse
the completed run while its relevant inputs remain unchanged:

```sh
python3 tools/check-watchdog-configuration.py \
  --fel "$HOME/.cache/sunstoneos/build/sunxi-a733/sunxi-fel" \
  --rom "$HOME/.cache/sunstoneos/evidence/rom-copy-20261016-084055/physical-rom.bin"
```
