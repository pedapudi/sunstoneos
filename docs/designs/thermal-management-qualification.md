# Thermal transitions and E902 management need separate ownership proofs

Status: reference analysis and proposed qualification sequence. This document
authorizes no voltage, power-domain, sleep or board operation. It is intended
for the coordinator and the processor lifecycle owners.

## Reuse finite acquisition evidence before testing protection

The hardware matrix already records physical acquisition and normal return
from the finite thermal image. Its earlier statement that acquisition remains
unqualified is stale. That result establishes a finite observation; calibrated
accuracy, interrupt delivery, thermal transitions and sustained behavior remain
open. The returning CPU image can also capture its existing thermal window
without starting USB, as described in [the capture design](cpu-return-thermal.md).

The current driver implements all five sensors, factory calibration, alarm and
critical status, deferred interrupt handling and stale-sample protection.
[The thermal reference](../thermal.md) pins the five-channel layout and IRQ71.
The OS CPU limit can lower the critical trip, and cannot raise the normal
110 °C critical threshold. The existing qualification limit is 70 °C.
These are implemented mechanisms; physical transitions remain unqualified.

Before another thermal image, compare the current driver's complete startup,
threshold, IRQ acknowledgement and configured-disable sequences against the
pinned Linux source and manual sections 7.1.6 and 7.1.8. Carry the effective
26 MHz oscillator and normalized reference route through that comparison.
Reuse valid arithmetic and calibration checks; no repeated acquisition run is
needed merely to refresh a commit ID.

## Exercise protection with a reviewed lower trip

The first new case should trigger IRQ71 with a hardware alarm threshold below
a previously observed cool temperature. It should mask in the handler, wake
the owner task, collect and acknowledge status, and rearm only after the
interrupt is inactive. The physical result must distinguish alarm, critical
and data-ready causes. It must not require heating the chip to its normal
critical temperature.

A separate CPU case should lower the session critical limit below its observed
temperature and demonstrate bounded submission stop, worker termination,
secondary OFF proofs and normal return. The trip and session must be reviewed
against the actual admission predicate before building. No unchecked numeric
trip value is proposed here.

Each image needs the existing host-armed WDT0 and RTC breadcrumb path. A
software timeout or successful reset is not proof that an accelerator stopped.
The GPU and NPU owners must supply their accepted drain/OFF conditions before
thermal policy can release any resources. A test that returns with retained
uncertainty is a failed protection case.

## Qualify existing top points and transitions after lifecycle acceptance

Both saved speed-bin words select vendor bin 4. The source-backed points are
already implemented in [performance modes](../performance-modes.md):

| Block | Default high point | Optional low point |
| --- | --- | --- |
| CPU-L | 1794 MHz, 1000 mV | 1196 MHz, 800 mV |
| CPU-B | 2002 MHz, 1050 mV | 1404 MHz, 800 mV |
| DSU | 1274 MHz, 1000 mV | 910 MHz, 800 mV |
| GPU | 1008 MHz, 960 mV | 600 MHz, 800 mV |
| NPU | 852 MHz, 800 mV | 492 MHz, 800 mV |

After each processor's repeated start/job/stop path passes, run the existing
high/low/high mode sequence with fixed inputs and exact output parity. Capture
selected clock words, programmed voltages, thermal rows, completion and drain
for each interval. Then compare fixed CPU, GPU and NPU jobs individually and
concurrently. Concurrency requires the shared clock/rail registry and each
owner's accepted fault behavior; CPU fallback cannot satisfy either accelerator.

Retain the existing operation deadlines until the owners establish tighter
measured bounds. Before implementation, a design panel must set measurable
limits for thermal notification-to-stop, processor drain, voltage settling and
normal return. The same three reviewers must review the resulting code.
Host counters compare MMIO, wakeups, copies and wait time before and after.
Unexplained regressions block integration.

## Starting the vendor E902 firmware conflicts with the runtime provider

Current SunstoneOS excludes the E902 as a remote writer. There is no native
mailbox owner or system suspend implementation. The
[SCP analysis](../vendor-firmware/scp-firmware.md) derives the vendor sequence
from firmware SHA-256 `07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749`
and records offsets in the existing data inventories. The manual's sections
5.2 and 6.1 describe the E902 and four eight-word mailbox channels.

Vendor BL31 copies firmware into SRAM A2, writes its start address, enables the
RISC-V bus/reset/start controls and releases its clock. Firmware cold startup
then changes APBS0, initializes S_TWI0 and PL0/PL1, uses S_TIMER0/1 and writes
RTC word `0x0709010c`. That RTC word is the returning monitor's TIME breadcrumb.
The PMIC task already owns the bus and pins and refuses the firmware's APBS0
route. NPU workloads may own overlapping SRAM. Starting this firmware inside
the current runtime would violate each of those ownership contracts.

Vendor message channel 3 uses a header, payload count and payload words.
Loopback type `0x61` has at most eleven payload words. A native owner must
validate the complete count before copying, serialize a complete frame across
the eight-word FIFO, and retain partial frames after timeout. FIFO emptiness
does not prove that the remote core stopped. Use interrupt-driven delivery
with a bounded task queue; the vendor's polling loop is not the runtime target.

Before implementation, select a source-available compatible management
firmware or a reviewed adaptation of the vendor startup and ownership policy.
The existing opaque firmware remains a source reference; no replacement is
justified solely by its opacity. The demonstrated bus, clock, breadcrumb and
SRAM conflicts justify adapting its operating-system interfaces before reuse.
Specify who owns each resource during active runtime, sleep and recovery.
Do not introduce a second PMIC writer or weaken remote-writer exclusion.

## Suspend and resume require a design panel before coding

The vendor suspend path saves clocks and security state, quiesces domains,
puts DRAM into retention, changes shared buses and disables selected rails.
Resume restores rails and clocks, powers every PCK domain, reinitializes DRAM,
restores security and starts CPU0 before a mailbox handshake. Several waits
are unbounded, and some auto-gating settings are not restored. The sequence
cannot be copied as a bounded SunstoneOS transition.

The first management milestone is owned interrupt-driven mailbox loopback
with the remote core's writable memory excluded from application allocations.
The next is remote-core idle/wake without system rail or DRAM transitions.
System suspend follows only after the three-reviewer design panel accepts
per-stage deadlines, failure retention, wake-source ownership and independent
watchdog/recovery behavior while CPUX or DRAM is unavailable.

For suspend qualification, reserve a RAM range, fill it with a deterministic
pattern, publish it through the accepted cache path, and compare it after wake.
Record the exact image hash, Git revision, wake source and retained breadcrumbs.
Verify all saved clock/security state and processor ownership before resuming
application jobs. Preserve both microSD cards; these cases need no storage
writes or absent peripherals.
