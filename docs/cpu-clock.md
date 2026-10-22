# CPU cluster clocks and voltages use fixed operating points

Status: implemented and host-modeled. The driver follows the board thermal
policy, reads its permission from a grant record that an OS session can
carry, and times work in cycles of the cluster that ran it. No image calls the
driver yet, and no image carries a grant. Nothing in it has run on the board.
Any physical use needs the user's approval for each clock or voltage change.

The A733 has two CPU clusters and a shared DynamIQ unit:

- **CPU-L**, the six Cortex-A55 cores (cpu0–cpu5). The board device tree
  supplies them from AXP8191 output DCDC5.
- **CPU-B**, the two Cortex-A76 cores (cpu6–cpu7), supplied from DCDC3.
- **DSU**, the DynamIQ Shared Unit with the L3 cache. It also uses DCDC5,
  so every DCDC5 voltage must cover both CPU-L and the DSU.

Boot0 leaves CPU-L at 494 MHz: the 26 MHz oscillator times N=38, divided by
2. The recognition timings in [mimus.md](mimus.md) were measured at that
configured rate. The pinned operating-point table permits CPU-L up to
1794 MHz and CPU-B up to 2002 MHz, depending on the chip's speed bin. These
rate ratios are upper bounds on speedup; work limited by memory or the DSU
clock gains less.

The implementation consists of [the API](../include/a733_cpu_clock.h) and
[the driver](../drivers/clock/a733_cpu_clock.c). It uses the shared
[clock registry](clock-leases.md), [rail ledger](power.md) and
[PMIC provider](pmic-runtime.md).

## Registers and rate decoding

Each domain has a PLL word and a word that holds its clock source selector
(the mux) and post-divider P. The CPU PLL block starts at `0x08870000`.

| Domain | PLL word | Mux and P word | Driver may write |
| --- | --- | --- | --- |
| CPU-L | `0x08871000` | `0x0887101c` | yes |
| CPU-B | `0x08872000` | `0x0887201c` | yes |
| DSU | `0x08873000` | `0x0887301c` | no |

In the PLL word, bits 31 to 27 are enable, LDO enable, lock enable, lock and
output enable; bit 26 requests an update; N is bits 15:8. In the mux word,
bits 26:24 select the source and bits 17:16 hold P. Sources 0 to 2 are the
DCXO oscillator and source 3 is the domain's own PLL. The configured rate on
source 3 is `DCXO × N ÷ 2^P`. P is a power-of-two exponent because the
pinned `ccu_nkmp.c` computes the rate with `1 << p` and programs P with
`ilog2`. Sources 4 and 5 (PERI0 2X and PLL_CPU_BACK) are not decoded.

`a733_cpu_clock_report` reads the oscillator, the speed-bin word and all
three domains, and decodes their configured rates. It writes nothing, so any
image with read access to these words can use it to name the clock in a
timing record. A decoded rate is a configured value. The cycle timing below
measures the rate independently. The mimus recording path still prints only
CPU-L and omits the clock line from native images; it can call this report
and the cycle timing instead.

## Operating points come from the 26 MHz table columns

The driver holds the `opp-microvolt-26m-vf*` rows of the pinned
`sun60iw2p1-cpu-vf.dtsi` (version V0.9) for CPU-L, CPU-B and the DSU. The
board's oscillator runs at 26 MHz, and the vendor drivers select these
columns for that oscillator. The 24 MHz columns do not apply. Each row lists
one voltage per speed bin, and a zero voltage marks a point that the bin does
not support. For example, the 1794 MHz CPU-L point needs 1000 mV on bin
vf0000 and is unsupported on vf0100.

The speed bin comes from the SID eFuse word at `0x0300624c`, as in the
vendor `sunxi_get_soc_dvfs`. The code is bits 23:16 when they are nonzero and
bits 15:8 otherwise. The table's `vf_mapping_table` maps the code to a
column:

| Code | Column | Driver behavior |
| --- | --- | --- |
| `0x00` | vf0000 | Uses the envelope described below |
| `0x01`–`0x06` | vf0100–vf0600 | Uses that column |
| `0x14`, `0x15` | vf0401, vf0501 | Uses that column |
| `0x11`, `0x12` | vf0101, vf0201 | Refused: the tables have no such column |
| any other | none | Refused |

An unprogrammed fuse also reads code 0, so code 0 cannot distinguish a
vf0000 chip from a missing bin. For code 0 the driver uses an envelope: a
row is admitted only when every column supports it, at the highest voltage
among the columns. On CPU-L this admits 416, 1014, 1196 and 1404 MHz at 800,
800, 850 and 950 mV.

A PLL program for a point uses P=0 and N = rate ÷ 26 MHz at or above
676 MHz, and P=1 below it. These are the values that the pinned driver's
search chooses, within its 26 MHz range of N = 26–84 for CPU-L and 26–86 for
CPU-B.

## A session must grant each change

`a733_cpu_clock_request` refuses every transition unless the configuration
carries a grant with a nonzero session, the cluster's bit, a maximum rate at
or above the target, and an unused change. Each admitted request consumes one
change, whatever its outcome. No current image or session supplies a grant.
A supervised session that grants one must record the user's approval for
that specific change.

An OS session carries the grant as a 96-byte little-endian record, which
`a733_cpu_clock_grant_load` checks and converts into the driver's
configuration:

| Bytes | Field |
| --- | --- |
| 0..23 | Magic `0x4153373343505531`, version 1, size 96 |
| 24..31 | Owner of the driver's clock and rail leases; nonzero |
| 32..47 | Epoch and generation of the board session that carries the record |
| 48..51 | Cluster bits: 1 for CPU-L, 2 for CPU-B |
| 52..59 | Highest admitted operating point of CPU-L and of CPU-B, in MHz |
| 60..63 | Number of admitted changes |
| 64..95 | SHA-256 of the reviewed record of the user's approval |

A record that grants nothing has zero cluster bits, and its epoch,
generation, maxima, change count and approval hash are also zero. The driver
then refuses every request. A record that grants changes must name the
carrying session's nonzero epoch and generation, so it cannot admit changes in
another session. Each admitted cluster's maximum must be a row of that
cluster's table, the other cluster's maximum must be zero, the change count
must be 1 to 64, and the approval hash must be nonzero. The epoch becomes the
grant's session. The hash identifies the approval record; preparing the
session must check that record, because the driver cannot. A record that
fails any rule leaves the configuration unchanged.

## The thermal policy selects operating points

`a733_cpu_clock_init` binds the driver to the board's
[thermal policy](thermal.md), which must already be initialized. The driver
reads the policy's level and the channels that have had a valid reading;
both are published for any task. A request needs a valid reading from the
cluster's own sensor, CPUL for CPU-L and CPUB for CPU-B, in addition to the
grant. The policy reports NORMAL before its first reading, so this rule keeps
an unmeasured cluster at its present rate. With a reading, NORMAL admits
granted points; THROTTLE admits only points at or below 1014 MHz, which needs
800 mV in every bin; CRITICAL refuses every transition. The driver keeps
CRITICAL once it has read it.

While idle, the owner's poll applies the level:

- Under THROTTLE, the driver lowers a cluster that it raised above 1014 MHz
  back to 1014 MHz. The lowering needs no sensor reading, because a channel
  without readings is itself a reason to throttle. It consumes no change and
  keeps the voltage. If it ends REFUSED or EXPIRED, the driver does not start
  it again in the same THROTTLE episode.
- At NORMAL, the driver raises a cluster that THROTTLE lowered back to the
  point it ran before, once. The raise needs the cluster's sensor reading and
  consumes a change, as a request does. Without a remaining change, the
  cluster stays at 1014 MHz. A NORMAL level also ends the THROTTLE episode,
  so a failed lowering is tried again when THROTTLE returns.
- An owner request for a cluster cancels any pending raise of that cluster.

The policy's hysteresis is 2 °C for the CPU channels and none for the GPU, so
a GPU near 90 °C can alternate the level at successive readings. Each return
to NORMAL then consumes a change, so the grant's change count bounds the
number of raises. Inherited rates above 1014 MHz that this driver did not set
are not lowered.

The first physical trial should be CPU-L at 1196 MHz. That point needs
800 mV on every bin except vf0100, and 850 mV on vf0100 or under the code 0
envelope.

## A transition orders voltage and frequency

One owner task calls `a733_cpu_clock_poll`. Each call performs one bounded
step and never waits in a loop:

1. Acquire the cluster's clock scope exclusively. CPU-L also holds the DSU
   scope shared, because its voltage depends on the DSU rate.
2. Acquire the cluster's rail exclusively in the rail ledger.
3. Read and check the inherited state: a 26 MHz oscillator that is enabled
   and stable, a mapped speed bin, the cluster PLL enabled and locked with no
   update pending, the mux on that PLL, and N within range. The cluster's
   pattern and frequency-modulation enables, described below, must be clear.
   For CPU-L, the DSU rate must decode and have a supported DSU point.
4. Read the rail through a PMIC snapshot. The output must be enabled and its
   selector must decode.
5. Compute the target voltage: the target point's voltage, and for CPU-L at
   least the voltage of the lowest DSU point at or above the current DSU
   rate.
6. When raising the rate, write a higher voltage first, then wait for the
   ramp of `|ΔV| ÷ 250 µV/µs`, the board device tree's ramp delay. No write
   is made when the present voltage is already enough.
7. Change the frequency. The remaining time before the caller's deadline
   must cover both PLL waits and a 10 ms margin for the owner polls that
   follow lock, 90 ms in total; otherwise the transition ends EXPIRED here,
   before any PLL or mux write. Then park the mux on the DCXO, write N, set
   the update bit, wait up to 10 ms for it to clear and up to 70 ms for lock
   (the pinned driver's limits), write P, and select the PLL again. Every mux
   and P write is read back, and the final words must decode to the target
   rate.
8. When lowering the rate, write the lower voltage after the frequency
   change and wait for the same ramp.
9. Release both leases.

The pinned Linux driver changes N in place with linear frequency modulation
(LFM) enabled. At probe it sets these bits in each CPU PLL's control words:

- bits 29 and 30 of the pattern word PAT0 (`PLL + 0x4`), a triangular
  pattern;
- bit 31 of PAT1 (`PLL + 0x8`), `lfm_en`;
- bits 31 and 8 of the LFM word (`PLL + 0x18`), a 1.125 MHz/µs ramp.

With any of these enabled, N alone may not define the rate, and lock may be
reported before the PLL reaches its final rate. A lowering could then
reselect the PLL while it still runs fast and lower the voltage under it.
This driver reads the three words for the cluster and refuses the transition
when PAT0 bit 29 or 30, PAT1 bit 31 or LFM bit 31 is set. It never writes
them. Their values after boot0 are unknown, so the first physical trial
shows whether this check admits the board's inherited state. Parking on the
oscillator keeps the cluster on a stable clock while the PLL relocks. A
transition to the present rate completes without writes.

## Failure handling keeps ownership after any write

Refusals found during steps 1 to 4 release both leases and return REFUSED.
This includes a snapshot that shows the rail disabled or an undecodable
selector. A failed register read or a PMIC provider fault returns RETAINED
even before any write, because the access path itself is then uncertain.

A deadline reached before the PLL is touched returns EXPIRED with both leases
released. This includes a PMIC request that expired before it reached the bus,
and a remaining deadline too short for the PLL waits in step 7. At that point
the hardware holds at most a confirmed higher voltage, which covers the
present rate. A lowering whose voltage write has completed still finishes its
ramp wait. Any error, timeout, readback mismatch or rejected PMIC write after
the first write returns RETAINED. A deadline reached during the PLL update,
lock or unpark steps also returns RETAINED; step 7 admits the PLL change only
when the deadline covers those steps, so this requires the owner task to be
delayed by more than the 10 ms margin. After RETAINED the driver keeps the
clock and rail leases, marks both managers retained and makes no further
access. A PLL timeout leaves the cluster parked on the 26 MHz oscillator.

## Cycle timing names the clock that ran the work

The configured rate does not show what clock a timed piece of work actually
ran at. `a733_cpu_cycles_sample` reads the core's PMU cycle counter
(`PMCCNTR_EL0`) and the architectural counter (`CNTPCT_EL0`) together with
IRQs masked, along with the core identity in `MIDR_EL1` and `MPIDR_EL1`.
`a733_cpu_cycles_enable` first configures the calling core's cycle counter:
it sets `PMCR_EL0.E` and `LC`, clears `D` so that every cycle counts, writes
`PMCCFILTR_EL0` as zero so that EL0, EL1 and EL3 count, and enables the cycle
counter. It never resets a counter. A sample refuses a cycle counter that is
not configured this way. Both functions run only at EL1.

`a733_cpu_clock_timing` takes two samples and returns the core, its cluster,
the elapsed cycles and counter ticks, the measured rate
`cycles × counter rate ÷ ticks`, and, given a clock report, the configured
rate of that cluster. Both samples must name the same core. MPIDR Aff1 is the
core number; cores 0 to 5 must identify as Cortex-A55 (CPU-L) and cores 6
and 7 as Cortex-A76 (CPU-B). The interval must advance the counter, by at
most `2^64 ÷ counter rate` ticks, about 8.9 hours at 24 MHz.

The cycle counter belongs to one core, so the caller must keep the timed task
on one core between its samples. The counter stops while the core waits for
an interrupt. The measured rate therefore equals the clock only while the
core stayed busy, and is otherwise a lower bound. A recognition report can
state the core, its configured rate, its measured rate and the cycles that
recognition took.

Whether boot firmware lets EL1 access the PMU has not been tested. If
`MDCR_EL3.TPM` or `MDCR_EL2.TPM` traps these registers, the first access
takes an exception to that level. A first trial should therefore enable and
sample once, in a supervised session, before timing recognition.

## Validation

`checks/cpu-clock-check.py OUTPUT` is a host check. It compares the
driver's tables, bin map, eFuse location, PLL fields, wait limits, N and P
choices, PMIC profiles and board rail assignments with the pinned sources.
It also confirms that 1014 MHz needs 800 mV in every column and that the
driver's sensor channels match the thermal driver's channel order. It then
builds `checks/cpu-clock-model.c` with AddressSanitizer and
UndefinedBehaviorSanitizer and compiles the changed units for AArch64.

The model runs the actual driver, PMIC provider, rail ledger, clock registry
and thermal policy against modeled CPU PLL words, modeled temperature samples
and a modeled rail that ramps at 250 µV/µs. At every mux write and every voltage write it requires the rail's
present voltage to cover each domain's applied rate. The model also rejects a
PLL write while that cluster's mux selects the PLL. Its cases cover:

- decoding of the recorded 494 MHz configuration and the bin map;
- refusal without a grant, before any reading, with only the other
  cluster's sensor read, above the granted point and for points outside the
  table, all with no register access;
- raises with and without a voltage write, lowering with the voltage written
  last, and CPU-B on DCDC3;
- a DSU rate that keeps DCDC5 at 900 mV when CPU-L is lowered;
- the envelope for code 0;
- THROTTLE refusal and automatic lowering; the raise back at NORMAL, which
  consumes a change and writes no voltage; a lowering after a stale channel
  without any CPU reading; no raise without a remaining change; a raise
  refused before any write when THROTTLE returns first; terminal CRITICAL;
  and no retry of a refused throttle lowering until the next THROTTLE
  episode, which lowers the cluster;
- refusals for a 24 MHz oscillator, a PERI0 parent, an unlocked PLL, an
  undecodable DSU rate, a disabled DCDC5 snapshot and each pattern or LFM
  enable, all with no register or PMIC write;
- clean expiry while another owner holds the clock scope, while the
  snapshot is still queued, during the ramp wait of a raise, and after a
  raise's voltage write when 3 ms remain for a PLL that locks in 5 ms, with
  the PLL and mux untouched;
- a PLL that locks in 60 ms within a 95 ms deadline, which completes;
- retention after an update timeout, a lock timeout, a PMIC write whose
  readback differs and a selector changed after the snapshot;
- grant records that grant nothing or grant both clusters, one refusal for
  each field rule, and a loaded grant that admits its point through the
  driver;
- cycle timing on a Cortex-A55 and a Cortex-A76 core, a fractional rate, a
  missing report, and refusals for two cores, a mismatched or changed
  identity, a nonzero Aff0, a counter that does not advance or goes back,
  cycles that go back, a zero counter rate and intervals too long to scale.

The check then runs the AArch64 object of the native cycle code under
Unicorn, with each system register it reads or writes modeled. Enabling sets
the expected bits, keeps another enabled counter and never writes a counter
reset; it is refused at EL0 and EL2 and when the PMCR write does not take
effect. A sample reads both counters after masking IRQs and restores DAIF
right after them. It is refused, without writing its output, with `E` clear,
`D` set, the cycle counter disabled, EL1 filtered or at EL0, and it makes no
access for a null output.

In separate manual runs, each of these altered drivers failed the model:

- one that changes frequency before voltage, and one that skips the ramp
  wait;
- one without the step 7 deadline check, one that retains on a disabled
  snapshot and one that ignores the LFM enable;
- one that ignores the sensor reading, one that admits a raise at THROTTLE
  and one that drops the CRITICAL latch;
- one whose raise at NORMAL consumes no change, one that retries a refused
  throttle lowering within the same episode and one that keeps a failed
  lowering across episodes;
- one that ignores the grant's epoch, one that ignores its approval hash and
  one that accepts timing samples from two cores.

Altered native cycle code failed the instruction model when it restored DAIF
after all reads, ignored the filter, wrote a counter reset or kept `D` set.
The check passed on 2026-10-21.
These are software results only. Physical lock time, settling, rail
accuracy, frequency, PMU access and thermal behavior remain unmeasured, and
the clock transitions have no native adapter.
