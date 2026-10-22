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

Raising CPU clocks and voltages through the [CPU clock driver](cpu-clock.md)
increases the board's current draw. Its physical use therefore also depends on
this unmeasured supply under load.

## One state word carries power readiness and revocation

The power-authority state word is one 64-byte line embedded in the
[rail ledger](rail-leases.md) and defined in
[a733_power_authority.h](../include/a733_power_authority.h). Its bits are PMIC
provider READY, supply verification done, one verified bit each for DCDC2,
DCDC4, CLDO2 and CLDO5, and fault bits for the provider, the clock registry, the
rail ledger and power domains 1, 4, 5 and 10. Every bit is set once per boot
with an atomic OR and never cleared, so READY cannot overwrite a fault that
another core set during startup. A reader makes one acquire load and tests a
mask. The PMIC provider checks it before every bus phase, and
`A733_POWER_HDMI` is the mask an HDMI or DisplayPort access needs.

## One owner powers domains 1, 4, 5 and 10

[a733_power_domains.c](../drivers/power/a733_power_domains.c), with the API in
[a733_power_authority_domains.h](../include/a733_power_authority_domains.h),
owns PCK600 domains 1 (DE_SYS), 4 (NPU), 5 (GPU_TOP) and 10 (VO1). Domains 0,
2, 3 and 6–9 and the CPU cluster and core domains keep their current owners.
Status: implemented and host-modeled. No image calls it yet; the consumers
listed below still run their own PCK600 sequences.

| Domain | Consumers | Supply token and verified bits | Domain-level reads |
| --- | --- | --- | --- |
| 1 (DE_SYS) | Framebuffer, G2D, deinterlace, DSI scanout | DCDC2 | MBUS gates CCU `0x5c0` bit 5, `0x5e0` bit 11 |
| 4 (NPU) | NPU startup | DCDC2 | MBUS gates `0x5c0` bit 6, `0x5e0` bit 18 |
| 5 (GPU_TOP) | GPU | DCDC4 | MBUS gates `0x5c0` bit 7, `0x5e0` bit 16 |
| 10 (VO1) | HDMI, DisplayPort | DCDC2, and CLDO2 or CLDO5 | None beyond the shared reads |

Every domain also requires the PCK600 bus clock (R_CCU `0x1ac` bit 0) and
agreement between its command and status fields.

- **Rows.** A consumer requests a row with a rail token that holds the
  domain's supplies. The owner pins that token in the ledger while the row is
  counted. The request makes one acquire load of the state word and no PMIC
  read; it returns PENDING before supply verification is done and REFUSED when
  a required verified bit is clear.
- **Read-only check.** `a733_power_domain_check` runs the domain-level reads
  and the adoption check and writes nothing. A consumer calls it before its
  first reset or clock write.
- **Adoption and transitions.** Command and status both OFF give OFF; both ON
  give ON, and domain 5 also needs the GPU clock gate (CCU `0x0b20` bit 31) and
  resets (`0x0b24` bits 0 and 16) clear. Anything else is FAILED. Commands are
  written only from OFF or ON: the pinned delay row, then the command, then one
  `DSB SY`, because the acknowledgement wait is timed from the command reaching
  the controller. A requester then spins on the status with the owner lock
  released for at most 50 µs of raw counter ticks and returns AGAIN if the
  acknowledgement has not arrived. The 50 µs bound is provisional. Any
  requester's later call continues the wait; 10 ms after the command the domain
  becomes FAILED.
- **Retention.** An unexpected status or a passed deadline retains the domain,
  its rows, their tokens and the PCK lease, and sets the domain's fault bit. A
  failed supply or domain-level read leaves the row uncounted and writes no
  command.
- **Power-off only for domain 5.** Its one bus master is the GPU. When the last
  row is released, after the GPU has drained its DMA, the owner rereads the GPU
  gate and resets; any set bit refuses the power-off and keeps the row.
  Domains 1, 4 and 10 have several masters and stay on.

The per-consumer display provider
[a733_de_sys_power.c](../drivers/power/a733_de_sys_power.c) remains for the
framebuffer and G2D, now for domains 1 and 10 only.

## Supplies are verified once per boot

The task that polls the PMIC provider calls `a733_power_domains_verify`. It waits until the
provider start plus 5,160 µs, the full-range ramp and enable delay in the
device tree, then snapshots DCDC2, DCDC4, CLDO2 and CLDO5 under one shared rail
token: four snapshots of six transactions, 24 in all, about 2.7 ms at
400 kHz. Each output that is enabled at its expected setting (800 mV for DCDC2
and DCDC4, 1,800 mV for CLDO2 and CLDO5) sets its verified bit, and then the
done bit is set.

This relies on no agent writing those outputs after boot. The provider admits
selector writes only to DCDC3 and DCDC5, and the OS issues no PSCI CPU_OFF,
CPU_SUSPEND or SYSTEM_SUSPEND call; `checks/pmic-tree.py` fails if either
changes or if a source outside the provider names the S_TWI0 interrupt
(INTID 235). Firmware writes and a second S_TWI0 master are excluded only by
the board evidence below.

The NPU runs only at 492 MHz, where the pinned vendor operating-point table
gives 800 mV for every speed bin, so its supply selection is the verified DCDC2
setting: `a733_power_npu_supply_uv` returns 800,000 once DCDC2 is verified and
0 otherwise. Nothing writes DCDC2 or DCDC4.

## Consumers still to move to the owner

The following changes belong to the consumers' packages:

- The GPU platform and resource manager request a domain-5 row and use the
  DCDC4 verified bit instead of `a733_gpu_power.c`'s PCK600 steps and the DCDC4
  sample through `a733_pmic_consumer.c`.
- The NPU platform requests the domain-4 row after its resets, rate and
  clocks, with one `DSB SY` after its last CCU write, and keeps its 2,000 µs
  post-ON settle. The NPU startup's direct S_TWI0 snapshot path goes when the
  NPU startup and clock-handoff models bind the PMIC provider.
- The framebuffer, G2D, deinterlace and DSI scanout request domain-1 rows; HDMI
  and DisplayPort request domain-10 rows, and HDMI's `validate()` loads the
  state word with `A733_POWER_HDMI` in place of its per-access try-locks.
- The board and OS images and the application images run one PMIC service
  task that calls `a733_power_domains_verify` and wakes requesters, replacing
  their tick loops. It needs a second task-notification index from the
  scheduler configuration.

## Board evidence that would confirm the power authority

- The S_TWI0 SCL rate for divisor `0x28`.
- Unchanged DCDC2 to DCDC5 selectors and PMIC page 0 after PSCI CPU_ON, and a
  bus capture showing no other S_TWI0 master.
- DCDC3 and DCDC5 readbacks after a raise and a lowering, with
  request-to-readback times.
- PCK600 time to ON for domains 1, 4, 5 and 10 and to OFF for domain 5, which
  replaces the provisional 50 µs spin bound.
- A schematic source for the DE_SYS, G2D and deinterlace supply.
