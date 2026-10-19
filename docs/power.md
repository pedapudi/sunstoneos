# PMIC register reads work on the inherited I²C bus

The native I²C reader identifies the AXP8191 power controller at address `0x36`
and reads its storage-rail configuration. The physical check passed on
2026-10-16. Regulator writes, cold bus initialization, concurrent clients, and
bus-error recovery remain unqualified.

[drivers/i2c/a733_i2c.c](../drivers/i2c/a733_i2c.c) uses the always-on controller
at `0x07083000` with PL0 and PL1. It requires the inherited clock, pin function,
controller enable, idle status, and released bus lines. Each byte read sends
the register address, a repeated START, a read address, and a final NACK before
STOP. It sends no register-data writes. The transfer deadline is 10 ms, followed
by a separate 10 ms STOP deadline. Arbitration loss suppresses STOP because
another master may own the bus. A failed transfer ends the probe.

The API requires one owner. It has no scheduler mutex, interrupt delivery,
multi-byte transfer, DMA, bus scan, reset sequence, or recovery clock pulses.
Normal close restores the inherited controller control bits. A failed STOP or
arbitration loss prevents close from altering the controller afterward.

## Physical evidence and interpretation

The probe read 17 specified registers in approximately 2.12 ms, using the
declared 24 MHz architectural counter. It returned to ROM with automatic
access. The inherited I²C divisor was `0x28`; the bus frequency was not measured.
The normal register page was selected, chip identity was `0x03`, and revision
was `0x00`, matching the pinned AXP8191 reference driver.

| Setting | Register value | Interpretation from the reference register map |
| --- | --- | --- |
| DCDC enables | `0x10 = 0xff` | DCDC8 enable bit is set |
| DCDC8 voltage | `0x19 = 0xe4` | Seven-bit selector 100 programs 1.8 V |
| LDO enables | `0x22 = 0xe1` | DLDO6 enable bit is set |
| DLDO6 voltage | `0x39 = 0x14` | Selector 20 programs 2.5 V |
| DCDC1 voltage | `0x12 = 0x17` | Selector 23 programs 3.3 V |
| Switched DCDC1 outputs | `0x11 = 0x17` | DC1SW2 is enabled; DC1SW1 is disabled |

The board device tree assigns DCDC8 to UFS VCCQ/VCCQ2 and DLDO6 to UFS VCC.
These are programmed settings, not measured voltages or proof of physical
storage population. Their enable bits remove disabled regulator configuration
as the immediate explanation for UFS failure. No voltage setting was changed.

| Artifact | SHA-256 |
| --- | --- |
| Power probe ELF | `c4680a483028893f726b4406d112529db676ecdda98a122c7b132fccb5638b85` |
| Power probe binary | `1f446f5ed607da455448d908b9951b986e378b033157b509abe9d617a4c4db31` |

Raw evidence is `~/.cache/sunstoneos/evidence/power-read-20261016/`.
The `a733-power-probe` build target contains
[checks/power-probe.c](../checks/power-probe.c). It uses the normal bounded
kernel loader with `--expect 'PASS PMIC identity and rail reads'`.
Do not execute it while the board is awaiting independent recovery.

The expanded probe passes all 19 reads on the second board under
`second-board-20261018-3-3/pmic-rail-probe/runtime/`. DCDC9 is enabled with
selector register `0x1a = 0xc8`; its seven-bit selector is 72, representing
1,240 mV. ELDO6 is enabled with `0x3f = 0x0c`, representing 800 mV. These are
programmed settings, not measured rail voltages. No regulator data was written.
The corrected returning monitor completed with status zero and ROM returned as
`003:093` with automatic USB access. This result covers the normal PMIC workload;
it does not qualify recovery from arbitrary peripheral faults.

References are the [pinned board sources](sources.md): the bootloader's
`include/sunxi_power/pmu_axp8191.h`, `drivers/sunxi_power/pmu_axp8191.c`,
`include/sunxi_i2c.h`, and `drivers/i2c/sunxi_i2c.c`; the kernel's
`ccu-sun60iw2-r.c`, `pinctrl-sun60iw2-r.c`, and board device tree.


## Host USB supply remains unqualified under load

The second board remains powered only by host USB. The user reports that the
first board is now connected to a 5 V / 3 A port, and its host data connection
works. Its boot-CPU cache diagnostic returned successfully. The precise current
supply arrangement and voltage under load remain unverified; recovery must
remove every source powering that board. Connection evidence is under
`first-board-independent-power-20261018/connection-5v3a.json`.

The [board manual V1.0](https://drive.google.com/file/d/1R1MC9erKTztd87GvJ_UFE132x6KgPATe/view)
lists a 5 V / 3 A supply and recommends a high-quality adapter and cable. Its
section 2.12 also cautions that computer USB power can be unstable with additional
peripherals. The pinned local text is
`~/.cache/sunstoneos/sources/board-documents/manual-v1.0.txt`.

The host ports' available current, cable voltage drop and board input voltage
under CPU or accelerator load have not been measured. The ROM descriptor's
300 mA value is a descriptor declaration, not a current measurement or proof
of available supply capacity. PMIC selector reads likewise do not measure rail
stability. Supply limitation is a plausible contributor to startup failures;
current evidence establishes no such cause. Prefer one useful voltage observation
or a controlled comparison with a verified 5 V / 3 A source over repeated stress
runs. Any alternate supply arrangement must preserve USB data and avoid feeding
power back into the host.
