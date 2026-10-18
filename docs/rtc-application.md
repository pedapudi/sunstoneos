# Native RTC read and alarm diagnostic

The native application adopts the A733 SoC real-time clock (RTC), reads its
initialized calendar, and arms one non-waking alarm one to ten seconds ahead.
The owner task acknowledges its interrupt, disables the alarm, restores the
previous disabled target, and publishes a copied calendar/result snapshot over
USB. Cancellation follows the same ownership and restoration rules.
The source and linked image pass focused software checks. Physical operation
and retention remain unqualified.

## Sources and board prerequisites

The pinned Orange Pi kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` identifies the controller as
`allwinner,rtc-v201` in `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi`,
lines 962–975: base `0x07090000`, SPI 196 (GIC interrupt 228).
The matching `bsp/drivers/rtc/rtc-sunxi.c` uses TypeA one-based calendar days,
absolute day/hour/minute/second alarm targets, and write-one-to-clear status.
Its source identities are recorded in `apps/rtc/sources.json`. The native
calendar and register operations adapt that GPL-2.0-or-later implementation.

The A733 User Manual V1.00, section 9.1.7, pages 391–398, defines the calendar,
alarm and clock register fields. Its PDF SHA256 is
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
Section 9.1.4.3 describes a seconds counter, and section 9.1.5.3 says writing zero
clears alarm pending. Those descriptions contradict the register tables and
matching driver; the application follows the tables and driver. It treats day 1
as 1970-01-01 and refuses day 0 rather than repairing the inherited calendar.

Manual section 9.1.7.1, pages 391–393, defines clock control source bits 1/0,
crystal gate 4, calendar-write busy bits 8/7, oscillator gain 3/2 and automatic
switch controls 15/14. Section9.1.7.2 defines external-source status 0, automatic
switch pending 1 and oscillator failure 2. The diagnostic requires the external
32768 Hz source, no busy/fault/pending state, and stable source/gain/switch fields.
It never changes those fields.

The board schematic V1.2, SHA256
`4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0`,
shows Y2 `32.768K-12pF-20ppm`, C151/C158 12 pF and R11 10 MΩ on sheet 8
“08_CORE1”, connected to U2G X32KIN W37 and X32KOUT V37. Sheet 6
“06_POWER_AXP318” connects U1A M15 VRTC through RTCLDO-PMU to VCC-RTC;
L15 BKUPBAT is marked unconnected. This supports a powered-retention hypothesis,
not retention after removal of every supply. The schematic does not identify
the fitted board revision. The pinned zero3w-named DTS instead uses an
orangepi-4-pro compatible and HYM8563 on s_twi2. That external-chip variant
remains unresolved; this application does not assume it is fitted.

## Sealed ownership and startup writes

The ordinary board hook authenticates the full session against the copied boot
contract. The RTC hook copies and authenticates its own 768-byte session before
any RTC mutation. Four evidence hashes bind initial ownership, clock/calendar,
electrical conditions and manual recovery to the target and exact image.
Nonzero hashes alone do not prove those facts.

Before cached startup masks all SPIs, the primary EL3 hook requires interrupt 228
disabled, inactive and unpending. Under admitted exclusive RTC/R_CCU ownership
and exclusion of other cores and remote writers, it sets only bits 16/0 of
R_CCU `0x0701020c`, preserving the other bits and checking exact readback.
The matching kernel `ccu-sun60iw2-r.c` and manual section 4.2.5.22 define these
as reset release and bus gate. No scheduler mutex exists at this stage.
If reset was inherited asserted, release cannot prove preservation of an earlier
calendar. Invalid calendar state subsequently refuses the operation.

After bus enable, alarm enable, IRQ enable, pending and wake output must all be
disabled. An exposed pending event causes retained refusal without an
acknowledgement. After GIC affinity routing is enabled and before secondary
release, the hook configures only interrupt 228 as masked, level-triggered,
nonsecure Group 1, priority 0xe0, routed to core 0. Partial failure retains all
ownership; it does not gate the bus, assert reset, clear inherited events or
restore uncertain state.

Task access holds existing clock stability leases and the shared register mutex.
The interrupt handler only masks 228, checks session lifetime and publishes an
atomic notification. The owner waits for distributor mask propagation and an
inactive interrupt before RTC access. It acknowledges only its own pending bit
with an exact write 1. Cancellation may race its own alarm; an observed pending
event wins and is reported as an alarm. Saved targets are restored only after
confirmed RTC disable/acknowledgement. Copied publication additionally requires
GIC quiescence and an unexpired, live session. Ownership lasts until manual
whole-board power removal even after success.

## Remaining RTC modes and physical evidence

This diagnostic does not set time, initialize or calibrate the oscillator,
change the DCXO or LDO, or write general-purpose/boot words. Clock-source
initialization, set-time, alarm wake from low power, and retention qualification
remain required. No authoritative hardware reset-cause field has been identified;
software boot markers are not decoded as reset cause. Warm-reset retention and
complete supply-removal retention require separate physical evidence.

## Frozen software evidence and launch

The affected supervised image and source/dependency records are retained under
`~/.cache/sunstoneos/evidence/rtc-native-20261018/`. Its ELF SHA256 is
`bd1043d087bdcc1293cb298439f441643d4b493cbbd39fe4fa2b690604a87ca8`.
The generated build ID is
`8e4b39b653dbfc8371d24124b35036675bffbd7e900191b84ecbc5634eb947dd`.
The evidence records 174 compiled repository inputs, 417 dependency inputs and
2,496 pre-run input hashes. Those hashes still match after execution.

The new checks cover TypeA calendar/leap-day/midnight and alarm ownership,
partial boot failures, complete session sealing and publication refusal,
core-0 IRQ notification, deferred immediate-interrupt enable/readback,
private copied output and revocation, protected packaging, launcher admission,
and host framing/capture/RTC authority. The driver executes over explicit MMIO
callbacks. Linked native instructions execute over modeled hardware and cache
publication boundaries. Host frames are synthetic and carry the snapshot copied
by the native instruction fixture; native USB serialization is source/link-only
coverage. No physical timing, oscillator accuracy or retention result is implied.

`apps/rtc/prepare.py` creates a protected bundle from the exact image manifest,
a session template and reviewed target-specific physical admission records.
The software model template in the evidence directory is nondeployable.
`tools/check-a733-rtc-supervised.py` requires a coordinator-owned queue claim,
the sealed bundle and manual recovery admission. Request byte 0 observes a
result; byte 1 requests cancellation. The host tool's `--cancel` option sends
that request once. A successful 64-byte header carries session identity and a
SHA256 digest followed by the immutable 88-byte calendar/alarm snapshot.
Pending or failed replies expose no snapshot. Raw refused frames remain durable.
After any attempted run, ownership remains retained until whole-board power
removal; a USB cable reconnect alone does not establish that recovery.
