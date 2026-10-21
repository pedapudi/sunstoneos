# Native radio platform ownership

The radio platform binding connects the native SDIO controller and card layer to
SDC1 clocks, pins, interrupt masking, Wi-Fi enable control and the shared PMIC
provider. Composed host checks and native compilation pass. Physical operation
remains unqualified.

This document is for platform and driver developers. The binding owns one
uninterrupted module and card lifetime. Firmware loading and network service
borrow the actual enumerated card; their protocol state remains owned by the
existing radio transport.

## SDC1 is separate from storage controllers

The board routes radio SDIO to SDC1 at `0x04021000`, with interrupt INTID 194.
PG0 is clock, PG1 is command, and PG2–PG5 are data 0–3, all using mux function 2.
The binding reserves all six pins during initial one-bit operation.
Its native address filter excludes the SDC0 and SDC2 storage apertures and the
controller's eMMC reset register.

Main CCU base is `0x02002000`. SDC1 module offset `0xd10` contains source
bits 26:24, gate bit 31, and two linear divisors: M in bits 4:0 and N in
bits 12:8. Both divisors equal the encoded field plus one.
SYS_CLK24M divided by 30 supplies an 800 kHz module clock. The existing
controller's SDR divide-by-two path produces a 400 kHz initial card clock.
Local bus gate and reset are bits 0 and 16 at offset `0xd1c`.

The local gate must be off and reset asserted at admission. The binding preserves
unowned fields, programs its divider with the module gate off, enables its bus
and module clocks, holds reset for ten microseconds, and deasserts reset.
That software interval is conservative; it is not a measured hardware limit.
The real SDIO state machine then checks controller version and performs local
reset, FIFO, timing and clock-update preparation before card enumeration.

## Four-bit default speed is an optional request

`a733_radio_platform_four_bit` must be called before the first poll. It adds
the shared PERI0 scope to the clock lease, and every later clock-profile
observation also requires the PERI0 400 MHz branch: its gate, its enable status
and a PLL configuration that yields 400 MHz. After enumeration of a full-speed
card (CCCR capability bit 6 clear), the platform reads CCCR address 7 and writes
four-bit width with read-after-write, keeping the other bits. It then widens the
controller. During that poll only, it holds the platform register lock. Its
module callback stops the SDC1 module gate, selects source 1 (PERI0 400 MHz) with
M=8, and restarts the gate; it verifies each write by readback and rechecks the
clock profile and pin health. The resulting module clock is 50 MHz and the card
clock is 25 MHz. A final CCCR read at the new clock must still report four bits
before the platform becomes READY. Any failure retains the platform, including
the changed module register. A low-speed card stays at one bit and 400 kHz.

No board image requests this profile yet. Physical qualification requires a
coordinator-owned identity read at four bits and 25 MHz that matches the
one-bit result.

Shared storage access depends on STORE AHB gate bit 24 at CCU offset `0x5c0`,
STORE MBUS gate bit 30 at `0x5e0`, and MSI_LITE1 gate/reset bits 0, 16 and 17
at `0x59c`. SD-monitor automatic gating at `0x5c0` bit 29 must be disabled.
The binding observes these inherited prerequisites under stable fabric ownership.
It does not toggle shared storage gates or resets.

AHB, NSI and MBUS selectors and divisors must match the admitted snapshots.
Supported parent profiles are SYS_CLK24M and the source-defined PERI0 outputs.
The binding verifies actual PLL controls, output enable requests, effective
output status and divider relationships. It reserves each selected parent in
addition to the shared fabric. No rate follows from a token alone.

## Rail observations require authenticated board wiring

The schematic and device tree do not supply one consistent radio I/O rail map.
Schematic V1.2 sheet 6 connects DCDC1 to `VCC-3V3-WIFI` and DCDC8 to the global
1.8 V supply. Sheets 9 and 14 connect PG/PM and module VDDIO to that supply.
The board device tree instead names BLDO5 and CLDO1 for PG/PM power.

Physical admission must identify the installed board topology and its VBAT,
PG, PM and module VDDIO supplies. The binding verifies only that admitted map.
DCDC1 must be enabled at a programmed 3.3 V; each admitted I/O supply must be
enabled at a programmed 1.8 V. The selected rail scopes remain stable for the
complete radio lifetime. Register values establish programmed settings, not
measured voltages or electrical connectivity.

The shared PMIC provider owns the always-on I2C controller and PL0/PL1 once.
Radio requests asynchronous snapshots through that provider and retains its
lifetime identity. Each result includes page, chip, revision, enable and selector
observations, copied before the request is reaped. Radio never claims private
ownership of the PMIC pins or executes the early boot blocking reader.

The PMIC address is `0x36`. The AXP8191 profile requires page zero, chip ID 3
and revision zero. DCDC1 uses enable register `0x10` bit 0 and selector `0x12`.
DCDC8 uses `0x10` bit 7 and selector `0x19`. BLDO5 uses `0x21` bit 2 and
selector `0x2e`; CLDO1 uses `0x21` bit 3 and selector `0x2f`.
The binding performs no PMIC voltage or enable writes.

Card operating-condition register (OCR) admission is limited to bits 20 and 21, the ranges bordering 3.3 V.
The existing card worker intersects that mask with the card's response.
The binding does not request signal-voltage switching.

## Wi-Fi enable preserves Bluetooth controls

PM1 drives Wi-Fi enable and PM0 reads Wi-Fi wake. The guarded wake API samples
PM0 through the actual owned GPIO input and returns its current level.
The bank-qualified pin manager owns both pins. PM1 output requires the manager's authenticated complete PM bank
latch record, which preserves other outputs during always-on DATA writes.
Expected inherited pin functions are input or disabled. The pin manager
establishes PM1 low. The radio binding waits at least ten milliseconds after
validated supplies, then drives PM1 high. The source power helper supplies
that settling interval.
The binding then waits fifty milliseconds before controller preparation, matching
the AIC BSP power-on path before card rescan.

PM2 configuration and output level remain unchanged. The pin manager may read
its latch and preserve that bit while writing the shared PM DATA word.
The schematic connects its net to both the module's Bluetooth reset symbol and
its low-power oscillator symbol through separate resistors.
Physical admission must establish stable inherited behavior for those controls.
Changing PM2, module power or another reset invalidates the borrowed radio
lifetime. Independent Wi-Fi and Bluetooth power writers must be excluded;
the shared module's controls cannot be managed as unrelated devices.

## The borrowed card keeps actual resources owned

Startup masks the SDC1 GIC source, waits for distributor write completion and
requires the source disabled and inactive before configuring its pins or clocks.
Task-polled service keeps that interrupt masked. The core preserves the card's
raw SDIO interrupt indication. No GIC enable, interrupt handler or wake interrupt
service is supplied by this binding.

One designated EL1 FreeRTOS task advances the radio binding and downstream
transport. Its native adapter checks task identity, scheduler state, interrupt
nesting and IRQ masking. Clock, pin and GIC writers share the platform register
mutex. Exclusive SDIO FIFO access does not acquire that mutex per word.
Provider callbacks must be bounded and non-reentrant.

Only completed host preparation and card enumeration permit borrowing the actual
card. A generation-bound token identifies its single downstream owner. Before
each operation, that owner calls the binding's live guard, which checks real
clock and rail ownership, pin ownership and health, and the PMIC provider lifetime.
A fixed lifetime deadline bounds authorization independently of operation
deadlines. A fresh operation deadline is accepted only while the host is READY,
after any prior completion has been consumed. Active or unconsumed work cannot
extend its existing deadline, and the lifetime deadline never extends.

Register and host callbacks check monotonic time around hardware work. A late or
uncertain access retains clock, rail and pin tokens, card state, pending request
and borrowed buffers. A fault inside the common mutex retains that mutex.
Subsequent fault polling performs no MMIO or automatic recovery.
Independent watchdog recovery remains mandatory because a software deadline
cannot interrupt a stalled bus transaction.

The current radio transport has no complete shutdown and disposal protocol.
Accordingly, this binding offers no resource-release API after handoff.
Downstream firmware, runtime and packet objects remain alive through fault and
external recovery. A future shutdown path must first close every borrower and
prove card/controller quiescence before restoring pins or changing module power.

## Validation targets the new composition

The [focused check](../checks/a733-radio-platform.py) composes the real shared
PMIC provider, rail ledger, clock registry, pin manager, SDIO controller and
card worker against modeled registers. AddressSanitizer and UndefinedBehaviorSanitizer
report no errors. The check verifies:

- Actual resource acquisition, copied PMIC results and request reap.
- Ten milliseconds between PM1-low admission and enable, fifty milliseconds
  before module clocks and card access, and ten microseconds before reset release.
- SDC1 preparation and card enumeration, with exclusive controller/module scopes.
- One downstream borrower, PM0 input, forged-token and aliased-output refusal.
- Fresh deadlines after consumed completion and retention after active expiry.
- Wrong observed supply voltage before pin or controller changes, and retained
  common-mutex ownership after an uncertain clock write.

The platform core compiles for native AArch64. Its adapter compiles against the
actual single-core and SMP FreeRTOS configurations. Existing PMIC, SDIO transfer,
firmware upload and radio protocol checks were reused without repetition.
No physical board execution or performance measurement was performed.

The evidence directory is
`~/.cache/sunstoneos/evidence/radio-platform-20261016/initial/`.
Its input manifest preserves 101 source, dependency and compiler identities.
The executed sanitizer binary has SHA-256
`35ad80c227752dc6415e8805efac2b1c70272feead2f655eec071cca7f838542`.
The saved commands, log and native objects record the focused batch.

Source identities and the inspected board design are recorded outside Git in
`~/.cache/sunstoneos/evidence/radio-platform-design-20261016/`.
The primary references are A733 User Manual V1.00 sections 4.1 and 11.3,
the pin-function tables, board schematic V1.2 sheets 6, 9 and 14, and the pinned
board device tree. Published clock, GPIO power sequencing and AXP8191 register
sources corroborate those register contracts.

The default native profile uses one SDIO data line at 400 kHz. The optional
four-bit 25 MHz profile is implemented and covered by the register model only.
High-speed timing, DMA, interrupt service and suspend/resume require separate
implementation and qualification. Board startup must connect the borrowed
card and live guard to the transport worker before firmware loading.
