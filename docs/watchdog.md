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
Disarming disables the watchdog before restoring its configuration and RTC flag.
If disabling fails, the driver retains the FEL request and reports failure.
The RTC flag is volatile boot state; this operation does not program storage.

Hardware register access, timer expiry, reset scope, RTC retention, and ROM
handling of the FEL request remain untested. Treat DDR as uninitialized after
watchdog reset. The existing warm CPU-reset loader cannot establish this reset
path by reading an old DDR result.

## Focused host evidence

The driver compiled for AArch64. The host register model passed checks for
invalid timeout refusal, existing-owner refusal, feed behavior, restoration,
failed FEL-flag writes, failed configuration writes, and failed disarming.
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

After independent board recovery, verify ROM identity and reinitialize DDR.
First test watchdog configuration and disarming with register readback. Then
test expiry with CPU timer recovery disabled in an isolated test image. Verify
elapsed time, a new ROM instance, automatic access, and RTC evidence without
assuming DDR retention. Integrate watchdog arming before risky peripheral access
only after that physical reset path passes.

The [pinned sources](sources.md) provide the register references: kernel
`bsp/drivers/watchdog/sunxi_wdt.c` uses the `wdt-v103` layout; bootloader
`arch/arm/mach-sunxi/board_sun60iw2.c` supplies reset-output configuration;
`arch/arm/mach-sunxi/rtc.c` supplies the FEL flag and RTC index.
