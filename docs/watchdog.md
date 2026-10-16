# Independent hardware reset still needs physical validation

The secure CPU timer recovers the tested EL1 software stall, but the UFS probe
did not return to ROM. A peripheral transaction can potentially stall the CPU
before it services the secure interrupt. The exact instruction that stopped the
UFS probe is unknown because its retained result is inaccessible.

Further new peripheral-access sequences require a physically tested hardware
watchdog and ROM return. The failed USB instance is recorded in
`~/.cache/sunstoneos/recovery-required.json`. Host loaders using
`require_rom()` refuse that instance before sending a request, even if sysfs
still contains its ROM descriptor. A new instance must pass normal ROM checks.
The record is retained for diagnosis and does not block a distinct instance.

## Prepared watchdog driver

[drivers/watchdog/a733_watchdog.c](../drivers/watchdog/a733_watchdog.c) programs
the A733 watchdog at `0x02050000`. It uses the keyed configuration and mode
registers, supports the documented discrete timeouts from 1 to 16 seconds, and
refuses an already enabled watchdog. Callers must zero-initialize the context
and provide exclusive ownership.

Arming saves the previous configuration and writes the bootloader's FEL request
value to RTC register `0x07090108`. It selects system reset and the reset-output
setting used by the A733 bootloader to avoid resetting the power controller.
The driver checks the reset configuration before enabling the timer. A failed
configuration readback restores saved state without starting the watchdog.
Disarming disables the watchdog before restoring its configuration and RTC flag.
If disabling fails, the driver retains the FEL request and reports failure.
The RTC flag is volatile boot state; this operation does not program storage.

The RTC flag is handled by boot0. It does not establish recovery independent
of the installed storage bootloader. Hardware register access, timer expiry,
reset scope, and register retention remain untested. Treat DDR as uninitialized after
watchdog reset. The existing warm CPU-reset loader cannot establish this reset
path by reading an old DDR result.

## ROM recovery candidate

Disassembly of the pinned board boot0 identifies the FEL-flag reader at
`0x4a990`. Its caller at `0x5e1ae` clears the flag through `0x4aa08`, then
branches to ROM entry `0x20` through the routine at `0x48ec6`. This path depends
on boot0 being loaded and executing. The boot0 hash and provenance are in
[sources.lock.json](../sources.lock.json) and [sources.md](sources.md).

The pinned A733 ROM contains another route that executes before storage boot:

| ROM address | Observed behavior |
| --- | --- |
| `0x164` | Read `0x070501c0` and compare it with `0xfa50392f` on CPU0 |
| `0xcc` | On a match, load the program counter from `0x070501c4` |
| `0x20` | Forced-FEL entry: initialize RTC access, clear SRAM, select FEL |
| `0x1f0` | Check a separate dispatch flag at `0x070501d4`; low bits `0xefe8` redirect through `0x070501d8` |
| `0x22c` | Branch to the FEL dispatcher at `0x2330` when forced FEL is selected |

The reference bootloader names the block at `0x07050000` CPUIDLE. Setting its
CPU0 redirect entry to `0x20` is a candidate for recovery without executing
boot0. The flag and entry must survive the selected reset. That retention,
register accessibility, and reset entry behavior have not been established.
The separate dispatch flag must also be inactive; it can override this route.

[checks/a733-rom-recovery.py](../checks/a733-rom-recovery.py) executes the actual
ROM prefix in Unicorn and stops at the boot or FEL dispatcher. Normal reset
and normal reset with the bootloader's RTC flag both reach the boot dispatcher.
Explicit entry at `0x20` and CPU0 redirection to that entry reach the FEL
dispatcher. An active alternate dispatch flag overrides CPU0 redirection.

The check uses a Cortex-A15 instruction model and skips the NSACR write at
`0x1c4`, which Unicorn rejects. NSACR controls nonsecure access; its hardware
effects remain outside this check. No subsequent branch in the tested prefix
reads it. The check does not simulate watchdog expiry, register retention,
security permissions, clocks, or USB initialization.

The check passed with decoded ROM SHA-256
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

## Required next validation

After independent board recovery, verify a new ROM instance. Build the isolated
watchdog diagnostic entirely in SRAM, with saved host state and RTC results;
it must not require DDR to report a system reset. Review ownership and preserve
the original RTC values before using them for results.

First establish a reset destination that cannot execute unknown storage code.
Then test watchdog configuration and disarming with register readback, followed
by expiry with CPU timer recovery disabled. Verify elapsed time, a new ROM
instance, automatic access, and the retained result. Reinitialize DDR before
resuming normal probes. Integrate watchdog arming before risky peripheral access
only after the physical reset path passes.

The [pinned sources](sources.md) provide the register references: kernel
`bsp/drivers/watchdog/sunxi_wdt.c` uses the `wdt-v103` layout; bootloader
`arch/arm/mach-sunxi/board_sun60iw2.c` supplies reset-output configuration;
`arch/arm/mach-sunxi/rtc.c` supplies the FEL flag and RTC index.
