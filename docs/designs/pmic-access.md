# PMIC register access and service spin timing

Status: **implemented and measured on the board.** Bundle 6 ran the changed
provider on port 3-3 on 2026-10-24; section "Board result" gives its steps,
its access costs and the provisional empirical bound. The modeled step times come
from the host model of the power service, `checks/power-domains-service.c`,
which sets each bus flag at a sub-microsecond time, the write's completion
time plus the phase's wire time.
[pmic-access-measurements.md](pmic-access-measurements.md) records the
modeled rows against the base tree.

The PMIC provider (`drivers/power/a733_pmic.c`) reads the AXP8191 over the
always-on I2C controller S_TWI0. In five images its worker is the power
service task (`drivers/power/a733_power_domains.c`), which spins on the
counter while a bus phase is on the wire. On port 3-3 on 2026-10-24, the
read-only board bundle 6 measured the base provider's DCDC2 or DCDC4
snapshot step at 917 µs
(median of 34 steps; 941 µs in the first step). A snapshot is 252 bit times
at 400 kHz, 630 µs of wire time, so the step is 146% of wire time (149.4% in
the first step). The raw log is
`~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/bb6-run/kernel.log`.
The model reproduces the median when a register access costs 1,300 ns. That
fit does not identify the cost of individual operations. The changed
provider's run timed component windows; the model at those costs falls short
of that run's median step, and the gap is unexplained (section "Board
result").

## Change

1. **One time read per completed bus phase.** The provider's `rd()` and
   `wr()` make no time read. `transfer()` reads the time once after each
   completed phase's accesses and checks the request deadline and the
   monotonic clock with it. After a phase that starts a bus event (a START,
   repeated START, address, register index, data byte, receive or STOP
   write), the provider stores that time as `phase_at`. A wait that finds its
   flag or status not yet set ends the poll without a time read: the time
   read before it is at most two reads old, and the next poll reads the time
   at entry before any access. Publication reads the time once, after the
   provider releases the I2C mutex; the state word and the deadline were
   checked after the final STOP or at the entry of the poll. Request
   admission and startup check the deadline once after their read
   sequences. The native adapter divides the counter by its ticks per
   microsecond with a multiply-high by a reciprocal that its first time read
   computes, instead of a `UDIV`. This is item 5 of section 2 of
   [performance-architecture.md](../performance-architecture.md).
2. **Task check once per poll.** The native adapter's `owned()`, `read32()`
   and `write32()` check the `holding` flag, one acquire load of the I2C
   lock's retained bit and the address allowlist. The task check
   (`a733_task_context_owner_el1`) runs where it already runs: at the entry
   of every `a733_pmic_poll()` and in the adapter's `enter()`, which also
   asserts the kernel mutex holder (section 1.1 of the same document). The
   `DSB SY` before and after every access stays.
3. **STOP wait for the idle status.** After a STOP write, the provider reads
   the control register (`CNTR`, `+0x0c`). Bit 4 (`M_STP`) still set is
   PENDING. Once it is clear, the low byte must be `0x40`, else the provider
   faults. It then reads the status register (`+0x10`); a value other than
   `0xf8` is PENDING. Both waits are bounded by the request deadline. The
   bootloader waits for status `0xf8` alone; the provider adds the `M_STP`
   wait.
4. **Line-control read at the ends of a request.** The line-control idle
   check (`+0x20`) runs at admission and after the request's final STOP,
   instead of after every STOP. A mismatch after the final STOP faults, as
   every mismatch does today; every STOP of the board runs so far read idle
   right after `0xf8`. A line that another master holds after an
   intermediate STOP stops the next START, whose wait the request deadline
   bounds.
5. **Spin from the phase-start write.** The service spins until
   `max(phase_at + phase duration, now + bit time / 4)`, in whole
   microseconds, rounded up, where `now` is read after the poll returns.
   `a733_pmic_work()` computes that time from `now`, so the service reads
   no private provider field. The
   first term times the wire from the write that started it, so the
   provider's work after that write overlaps the bus. The second term is the
   minimum re-spin after a poll that finds the flag still clear. It keeps
   polls at least a quarter bit time apart. The backstop on polls per step,
   `PMIC_POLLS_PER_STEP`, rises from 64 to 256, because with a slow bus a
   selector write needs about 56 polls plus re-spins.

`phase_at` is a microsecond time truncated from the counter and read after
the write completes, so a spin ends within about ±1 µs of the flag. Polls
that find the flag clear and re-spin are therefore expected.

## Safety properties

- **Ownership.** The provider object's try-lock, the poll's task check and
  the I2C mutex taken by `enter()` on the provider task are unchanged. Only
  `a733_pmic_poll()` reaches `rd()` and `wr()`, and it holds the provider
  object and has checked the task at entry, so a per-access task check adds
  no case. `checks/pmic-tree.py` keeps both facts (see the last section).
- **Revocation.** Unchanged: one acquire load of the power-authority state
  word at the entry of each poll and before each further phase, including
  the publish phase, and the lock's retained bit on every access. The
  publish phase itself checks the deadline and not the state word.
- **Bounded failure.** Inside `transfer()`, a deadline that passes during a
  phase is detected after at most that phase's accesses, three at most (an
  address phase writes `DATA` and `CNTR`; the final STOP wait reads three
  registers). Admission runs up to eight reads, and startup about 24, before
  their one check. A deadline that passes during admission retains the
  request in the admitting poll, before any transfer write, so a per-tick
  image does not hold the expired request's mutex until its next poll.
  `checks/a733-pmic.c` sweeps the deadline across every access of a snapshot
  and asserts both bounds. A `CNTR` write, including the one that sends the
  selector byte of a DCDC3 or DCDC5 write, can now start after the deadline
  has passed within its phase. The outcome class is unchanged: a fault
  retains the request, the held mutex and the leases, and the programmed
  voltage of a selector write is then uncertain, as it is today after any
  fault that follows the data byte.
- **The 1 ms spin limit.** Unchanged: `pmic()` returns one tick after
  `A733_POWER_PMIC_SPIN_US` of spinning in a step, and `SPIN_READS` still
  bounds each spin if the clock stops.
- **Bus protocol.** Every write, its value and its order are unchanged. Each
  phase still reads the flag, then the status, and checks the status code.
  The STOP wait accepts a slow return to idle as PENDING, so a status that
  reaches `0xf8` one access late no longer latches a permanent fault in
  board-qualified images.

The interrupt, memory and core arrangement does not change. S_TWI0's
interrupt (GIC SPI 203) is unqualified, and a 400 kHz byte is 22.5 µs, far
below a tick, so the service still spins. The provider object, `phase_at`
and the adapter state stay in kernel `.bss` (Normal write-back); S_TWI0 stays
Device-nGnRnE. `phase_at` is written and read only by the provider task.
Clients on other cores still see BUSY only inside a poll, and the model's
bound of one BUSY result per caller holds.

## Register sequences and sources

No register write changes. The A733 User Manual V1.00 describes S_TWI0's
control register in section 18.11.10.4 and its status register in 18.11.10.5,
with the same text as TWI0's (18.11.9). `M_STA` clears automatically after
a START is sent, `M_STP` clears automatically after the STOP, `INT_FLAG` is
write-one-to-clear, and the idle state `0xF8` is the only state that does
not set `INT_FLAG`. `CNTR` reads `0x40`, `BUS_EN` alone, once `M_STP` clears,
on two premises: the STOP write `0x58` cleared `INT_FLAG`, and `M_STA`
cleared after the START. `A_ACK` and `INT_EN` are never written. Only an
error state can set `INT_FLAG` after a STOP, so any other value faults.
Section 18.11.10.5 lists `0xF8` as "no relevant status information,
INT_FLAG=0". The pinned bootloader `b791be842935b27268ae3d00e943a9075495f30a`,
`drivers/i2c/sunxi_i2c.c`, waits after STOP for status `0xf8` alone, with a
timeout (`twi_stop()`, lines 469–483); the provider adds the `M_STP` wait
before it. It reads line control only before a clock
change (line 497) and in its bus-recovery pulses (lines 263–297), never after
a STOP. The provider keeps the line-control check at admission and after the
final STOP, so each request still starts and ends with both lines released.

## Budgets

Percentages are of the bit-time wire time: 630 µs for a snapshot and
812.5 µs for a selector write (325 bit times). The model's bytes take 23 µs,
so its own snapshot wire is 642 µs (102%). Rows at 200 ns per access keep the
check's 40 ns per time read. The board-cost rows use illustrative sums of
component means from bundle 6
(section "Board result"): 506 ns per register read, 402 ns per write,
144 ns per time read and 77 ns per provider poll. These costs omit work
outside the timed windows; the empirical bound does not depend on them.

| Metric | Baseline | Design | Measured by |
| --- | --- | --- | --- |
| Time reads per DCDC2 snapshot; per DCDC3 selector write | 469; 599 | 137; 174 | `checks/a733-pmic-cost.c --limits` |
| Register accesses per snapshot; per selector write | 211; 270 | 200; 255 | `checks/a733-pmic-cost.c --limits` |
| Task checks per register access | 1, plus 1 per poll | 0, plus 1 per poll | `checks/pmic-tree.py` |
| Serial accesses between a flag and the next phase-start write | 27 per transaction | 25 per transaction, about 150 per snapshot | Counted from the phase sequence |
| Snapshot step, 400 kHz, 200 ns per access | 712 µs (113%) | 685 µs (109%) | `checks/power-domains-service.c` |
| Snapshot step, 400 kHz, component means: 506 ns per read, 402 ns per write, 77 ns per poll, 144 ns per time read | Not rerun | 731 µs (116%); incomplete cost model | Same, board-cost table |
| Snapshot step on the board, median; first step | 917 µs (146%); 941 µs | 779 µs (123.7%); 809 µs (128.4%) | Bundle 6 `STEP` lines, port 3-3 |
| Selector write, 400 kHz, component means | Not rerun | One step of 941 µs; incomplete cost model | Same |
| Snapshot, 400 kHz, flags 10% later than the divisor implies, 200 ns | Two steps, one tick wait | One step of 751 µs for 708 µs of model wire | Same, slow-bus case |
| Tick waits per snapshot and selector write, 100 kHz | 2 and 3 | 2 and 3 | Same |
| BUSY results per other-core caller; from `a733_pmic_live` | At most 1; 0 | At most 1; 0 | Same |

The DCDC3 selector-write acceptance bound remains at most two steps with
one tick wait. A one-step result is not required and has not been measured
on the board. The service checks its 1 ms spin budget between polls, so a
final poll can finish slightly later than 1 ms.

**110% remains open.** A bare S_TWI0 read costs about 379 ns, but a write
without a trailing barrier has not been measured; every timed write had
`DSB SY` on both sides. Read and write costs must remain separate: the
model gives 688 µs with reads at 379 ns and writes assumed to cost 1 ns,
and 685 µs with those costs exchanged. With reads at 379 ns and no software
cost, writes of at most 150 ns give 664 to 692 µs, within 110% of the
630 µs wire (693 µs).
The 1 ns values approximate a free operation; they are assumptions, not
board measurements. The model uses zero write cost to mean "use the read
cost", so zero would silently restore the rejected equal-cost assumption.
At 379 ns for both accesses, it gives 694 µs with 22 µs bytes and 712 µs
with 23 µs bytes. Neither establishes a hardware floor or proves that
lighter barriers cannot reach 110%. Two routes remain open: the lighter
barrier placement under "Left out" (plain `DATA` write, one `DSB SY` after
the phase-start `CNTR` write), and mapping S_TWI0 Device-nGnRE instead of
Device-nGnRnE, which lets the interconnect acknowledge a write before it
reaches the controller. Each needs the board windows of section "Board result" and
the qualification of [shared-helpers.md](../shared-helpers.md).

**Provisional empirical bound.** At 400 kHz and CPU-L at 494 MHz, bundle 6
uses a median limit of 1,268 permille of each step's own measured wire
time. The first-step limit adds 40 µs to that wire-relative limit. Both
are the board's figures plus a margin: the observed median, 779 µs, plus
about 20 µs gives 799 µs, and the first-step limit, 839 µs, lies 30 µs
above the observed first step of 809 µs. Those microsecond values hold at
the nominal 22.5 µs byte, where a snapshot's wire time is 630 µs (798.840 µs
and 838.840 µs exactly); at another measured byte the permille limit
applies to that step's wire time. This is an empirical acceptance limit
from one lifetime; no model or cost measurement derives it, and it is not
a worst-case bound.

The conditions include one service core, the trace observer and write
stamps enabled, and interrupts unmasked during ordinary steps. Production
images omit that instrumentation, but their step times were not measured
here. Bundle 6's evaluator rounds ratios up from the counter ticks and
compares the first step using the counter's 125/3 ns tick directly.
Its upper distribution bin is also 1,268 permille. The preserved board
image used the earlier 155% limit; a future image uses the revised one.

The bundle's own wire time per step uses the measured byte: the lower bracket of the wire-time spin when it exceeds nine bit
times, at most 110% of nine bit times. A measured byte that is missing or
above 110% fails the bound, and the raw stamps are printed. It fails rather
than being capped at 110%: the run measures the provider's cost per step
against the wire time that the divisor sets, and a byte that much longer
means the bus itself ran slower (clock stretching or a wrong divisor). A
capped wire time would charge that bus time to the provider, so such a run
needs a diagnosis of the bus before any bound is set from it. A spin with
fewer than two reads has no read with the flag clear and therefore no lower
bracket. Bundle 6 then takes nine bit times, as for a missing sample, and
its `WIRE` line shows `reads=1`; the model treats that sample as missing.
Unlike a missing sample it does not fail the bound, because with nine bit
times the verdict can only be stricter. The service spins 23 µs for a
22.5 µs byte. Changing the modeled byte from 22 to 23 µs can change the
step because the completion checks and spin boundaries are quantized; the
694 µs and 712 µs rows above expose that sensitivity.

Re-spins are reported per step and in the summary and do not gate the run:
the service model gives no re-spin at a 23 µs byte and 18 to 24 per
snapshot at 24 µs. Each re-spin costs a quarter bit time and one poll, about
1.5 µs. The changed provider's run had none. Their count sizes the
counter-tick spin resolution left out below.

## Measurement

Committed checks, added with the implementation:

- `checks/power-domains-service.c`: independent read, write, poll and time
  costs; component means; historical calibration sensitivities; separate
  read/write floors; and byte-rounding sensitivity. `--board-costs` selects
  only those rows. The slow-bus case remains in the default check. Every
  service case runs with sub-microsecond flags.
  The 400 kHz case at 200 ns per access keeps its 110% assertion against the
  model's own wire time: the base provider fails it with sub-microsecond
  flags (712 µs for 642 µs), and the changed provider passes (685 µs).
- `checks/a733-pmic.c` model: flags set from the write's completion time in
  nanoseconds; separate write and per-poll costs; a status that reaches
  `0xf8` late or never after a STOP; a control value other than `0x40` once
  `M_STP` clears; a line-control value per STOP; and a count of accesses
  that start at or after a deadline. Its cases assert:
  - a pending then complete request for a late status;
  - a retained request at the deadline for a status that never arrives;
  - a retained fault for the control error;
  - a request that completes with a held line after an intermediate STOP,
    and one retained after the final STOP;
  - the deadline bounds above;
  - for a clock fault set between phases after any write of a snapshot, no
    further bus access;
  - a retained request when the deadline passes between the final STOP and
    publication;
  - a STOP spin that ends two bit times after the time read that follows
    the STOP write.

  Each of the matching provider mutants fails it.
- `checks/a733-pmic-time.c`: the reciprocal division against integer
  division for every divisor up to 4,096 and the powers of two to 2^32.
- `checks/a733-pmic-cost.c --limits`: the counts in the table.

Board measurement, bundle 6. A bundle built from this tree runs the changed
provider. Its windows measure component costs; one times the task check
that the earlier adapter made per access:

1. **Cost windows in the service task** at the park, which is the provider
   task, after the adapter's `enter()`, with the provider idle: on the
   provider's core at its clock. The record names the core (`MPIDR_EL1`
   Aff1); the CPU-L rate comes from bundle 4 in the same lifetime. The
   windows live in `apps/board-bundles-power/power.c`, which the tree rule
   allows to use the adapter's operations.
2. **Windows**, 256 iterations each in eight timed sub-windows of 32; each
   prints its total and its fastest and slowest sub-window:
   - `CNTR` reads with `a733_mmio_read32_dsb_sy_both`;
   - plain `CNTR` reads whose value is discarded, labelled as issue
     throughput;
   - plain `CNTR` reads where each value forms the next read's address (an
     address dependency), labelled as latency;
   - time reads through the adapter's `now`;
   - the adapter's `read` operation;
   - `a733_task_context_owner_el1()`, with IRQs unmasked as the earlier
     adapter called it, and the scheduler ticks inside that window;
   - `bb_twi_observe()` on a scratch record, as a flag poll that reads the
     flag clear: the trace observer that `apps/board-bundles-power/power.c`
     runs on every access.
3. **Writes**, timed inside the snapshots' own transfers: the `pmic_write`
   wrapper stamps the counter around the adapter's `write`, sums the ticks
   and keeps the fastest and slowest write, with no extra bus write. Those
   two stamps and the observer call per write, about 60 per
   snapshot, are inside the run's step times. The changed instrumentation
   limits direct comparison with the earlier 917 µs median.
4. **Wire time**: once, with IRQs masked from before a byte-phase `CNTR`
   write, `bb_status_spin()` reads `CNTR` back to back until `INT_FLAG`
   (bit 3) reads set. The record keeps the stamp of the last read with the
   flag clear and of the first with it set, both from the write's return;
   the byte is the lower one. That step is excluded from the step
   statistics and counted as excluded.
5. **Baseline reconstruction, rejected.** The original procedure added
   adapter-read, task-check, two time-read and observer windows, then fitted
   the difference from the base provider's 1,380 ns per-access cost. This
   cannot isolate the missing cost or transfer it to the changed provider.
   Section "Board result" retains the figures and explains their limits.

The step records count re-spins: control-register polls beyond the second
of a flag phase or of a STOP wait. A STOP wait lasts until `M_STP` reads
clear and the status register reads `0xf8`.

After the change, bundle 6 makes 32 snapshots of DCDC2 and DCDC4. Pass: the
median and first steps within their bounds set as above; a measured byte
within 110% of nine bit times; snapshot bytes unchanged (`0xff9e`); no
fault. Re-spins are reported. A DCDC3 selector write needs a separate
bundle with a PMIC write, which needs the user's approval; acceptance
allows two steps and one tick wait.

**Requalification.** The provider and adapter change affects every image
that links them. The board-qualified one is the native USB channel image
(`rank-9-usb-v4/`, rank 9 of the board-bundle table, four supply snapshots).
It was rebuilt and rerun with its qualifying pass conditions on port 3-3 on
2026-10-24 and passed (record `set5-r9-v6-usb/`), independent of the
selector-write run. The returning thermal image of rank 6 does not link
the provider: its board-run ELF has no `a733_pmic_` symbol, and each of the
seven units it compiles from the tree has the same instructions before and
after this change. The returning ADC image (`apps/thermal-returning/adc.c`)
links the provider and is not board-qualified. Bundle 7
(`apps/board-bundles-power/bundle7.c`), which runs the service and the
provider, either runs from its existing freeze on the current provider or is
refrozen only after the rebuilt bundle 6 passes. The spin change otherwise
affects only images that run the power service: GPU, accelerator, GPU
display, HDMI, the DisplayPort and G2D board image, and the integrated OS
image built from it. None of them is board-qualified.

## Board result

Conditions: port 3-3, 2026-10-24, bundle 6 of the fifth prepared set (step
3), built from the changed provider; read-only. The service ran on core 0.
Bundle 4 in the same lifetime measured CPU-L at 493,989,502 Hz. S_TWI0 ran
at 400 kHz (divisor `0x28`, bit time 2,500 ns). The raw log is
`~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/set5-bb6-run/kernel.log`.

**Steps.** 33 steps were recorded and one, the wire-time spin, was excluded.
Each recorded step completed one snapshot: 12 conditions, 24 bytes and
6 STOPs, 630 µs of wire time.

| Metric | Board |
| --- | --- |
| Median step | 779 µs (18,703 counter ticks), 123.7% of wire time |
| Steps 2 to 33 | 777 to 788 µs |
| First step | 809 µs, 128.4% |
| Re-spins | 0 |
| Steps containing an interrupt | 24 of 33; their median step is about 1 µs longer than that of the other 9 |
| Byte | Last read with the flag clear 22.4 µs after the write returned, first with it set 23.0 µs; the byte is the nominal 22.5 µs |
| Snapshots | All `0xff9e`; no fault |
| Image's own verdict | Pass against the provisional 155% |

**Access costs.** Each window ran 256 iterations in eight sub-windows of 32,
timed by the 24 MHz counter (41.7 ns per tick), with IRQs masked except in
the task-check window, which held no scheduler tick.

| Window | Average per iteration | Slowest sub-window per iteration |
| --- | --- | --- |
| `CNTR` read with `DSB SY` before and after | 418.8 ns | 419.3 ns |
| Plain `CNTR` read, value discarded | 377.9 ns | 378.9 ns |
| Plain `CNTR` read, address-dependent | 378.7 ns | 380.2 ns |
| Adapter time read | 143.6 ns | 149.7 ns |
| Adapter register read | 462.4 ns | 470.1 ns |
| Task check | 76.5 ns | 80.7 ns |
| Trace observer | 43.5 ns | 58.6 ns |

The 2,040 S_TWI0 writes of the snapshots took 17,533 ticks, 358.1 ns on
average, 8 ticks (333 ns) at the fastest and 16 ticks (667 ns) at the
slowest. Issue throughput and latency agree, so a read waits for its
completion either way, and the barriers add about 41 ns.

**Illustrative model costs.** Use the mean for every measured component,
rounded up after summation:

- read: adapter read plus the observer window, 462.4 + 43.5 = 506 ns;
- write: in-transfer write bracket plus the observer window,
  358.1 + 43.5 = 402 ns;
- time read: 144 ns;
- poll: the task-check window alone, 77 ns.

These are sums of measured component means, not measurements of complete
callbacks. The write bracket already contains counter-read overhead; adding
two adapter time reads would double-charge part of it. The outer wrapper,
bookkeeping and full provider poll are not timed. The observer window
measures a flag-read path, so its cost for a write is only an approximation.
At these inputs the model gives a 731 µs snapshot and a 941 µs selector
write. Neither independently predicts the board's median or a worst case.

**Rejected calibration, retained for comparison.** The earlier calculation
used slowest sub-window costs for reads, the slowest single write among
2,040 writes, and two additional 150 ns time reads per write. Its inputs
were 529 ns per read, 1,026 ns per write and 150 ns per time read. They mix
statistics and overcharge counter stamps, so they do not define the bound.

At 81 ns per poll that model gives 756 µs, below the 779 µs board median.
Its baseline reconstruction was 909 ns against a fitted 1,380 ns access.
Spreading the difference over the base provider's 211 accesses and 44
polls gives 2,340 ns per poll, including the task check. This gives 850 µs
on the changed provider and 941 µs on the base provider, above their
respective 779 µs and 917 µs board medians. The remainder method therefore
fails for both providers.

At 380 ns per poll the earlier model gives 779 µs. The added 299 ns is a
fit to the observed median and measures nothing. The 23 µs gap between
the model at 81 ns per poll (756 µs) and the board's median is
unexplained. The model's step does not rise steadily with the per-poll
cost: it is 757 µs at 250 and 260 ns, 762 µs at 300 ns and 759 µs at
310 ns. At 380 ns a selector write takes one 1,001 µs step; at 386 ns
(384 ns with 22 µs bytes) it splits into two steps with one tick wait,
which the selector-write bound of at most two steps with one tick wait
still admits. The host check prints the 380 ns row without asserting its
step time, so the model is not tied to the board's median; the other
sensitivity rows remain as historical model inputs.

**Bound selection.** The observed median is 18,703 counter ticks,
779.292 µs, or 123.7% of wire time. The preserved image printed 1,236
permille because its evaluator rounded down; the revised evaluator prints
1,237. Adding 20 µs and rounding down to a whole permille gives the
chosen 1,268-permille limit (798.840 µs at nominal wire time), about
19.55 µs above the observed median. The first-step allowance adds 40 µs.
The model is not used to claim an independently measured margin. Reuse
this provisional limit only under the stated bundle conditions; changed
clock rates, instrumentation or bus timing require a new assessment.

**Bundle 6 windows of the seventh set: implemented, not run.** The
bundle 6 image of `board-bundles-20261024-7/` adds three windows at the park
and one during the snapshots. Each uses the same 256 iterations in eight
sub-windows of 32 as the other windows, with IRQs masked unless stated.

- **Counter overhead:** the window loop with no body and a `DSB SY` at the
  end of each sub-window, the structure of the write window.
- **Plain writes:** 256 plain writes of the extended slave address register
  `XADDR` (S_TWI0 `+0x04`, `0x07083004`) with the value it read, and one
  `DSB SY` after each sub-window before its end stamp. A733 User Manual
  V1.00 section 18.11.10.2 with 18.11.10.1 (printed pages 1498 and 1499) gives
  XADDR one use: the second byte of a 10-bit slave address, compared only
  after the first byte matched `11110b` and ADDR bits 2:1. Section
  18.11.10.4 (printed pages 1499 to 1501) states that the controller does not
  respond as a slave unless `CNTR.A_ACK` is set, which the provider never
  writes. A START or STOP needs `M_STA` or `M_STP`, `INT_FLAG` is a CNTR
  bit, and the pins follow the line-control register `LCR` (section
  18.11.10.9). An XADDR write therefore starts no bus phase, acknowledges no
  flag and drives no line. The window runs only when ADDR bits 7:3 are not
  `11110b`, CNTR reads `0x40`, STAT reads `0xf8` and LCR reads both lines
  high with neither forced (`bb_xaddr_admitted()` in
  `apps/board-bundles-power/trace.c`). Afterwards XADDR, ADDR and the three
  idle reads must be unchanged, or the image fails without a controlled
  outcome. A refused precondition skips the window and is recorded.
- **Service loop between polls:** the work query, the time read and the
  work query with the spin time, as `pmic()` in
  `drivers/power/a733_power_domains.c` runs them.
- **Whole provider poll with the flag clear:** the last four client
  snapshots carry this window. The service polls a queued snapshot back to
  back, without its spin, and stamps each `a733_pmic_poll()`. A poll that
  read only CNTR in a flag phase, wrote nothing, completed no phase and
  returned PENDING is a whole poll with the flag clear: entry, the provider
  object's try-lock, the time read and the flag read. IRQs stay unmasked as
  in the service, and a sample with a scheduler tick inside is left out and
  counted. 256 samples are grouped into eight sub-windows of 32; 256 samples
  of the stamp pair alone give the per-sample overhead. Those snapshots
  leave the step records.

The windows give no authority for a PMIC rail write, and production barrier
placement remains a separate reviewed change.

## Left out

- **Lighter barriers and Device-nGnRE.** The measured read pair adds about
  41 ns; the effect on writes and the achievable step time remain open, and
  so does a Device-nGnRE mapping of S_TWI0. They need the board qualification of
  [shared-helpers.md](../shared-helpers.md). The intended placement, per
  section 1.2 of the performance architecture: the flag and
  status reads of a phase plain, the `DATA` write then the `CNTR` write
  plain, and one `DSB SY` after the phase-start `CNTR` write, which makes
  `phase_at` follow the write's completion. Reads of R_CCU, CCU, GIC and
  R_PIO at admission and startup keep their barriers.
- **Reading status alone to detect completion.** It would remove one serial
  read per phase. The manual defines `0xF8` with `INT_FLAG` clear but does
  not state the status value while a phase is on the wire, and the
  bootloader reads the flag first.
- **Removing the flag read right after each phase-start write.** It runs
  while the bus is busy, so it costs no step time, and it feeds the bundle 6
  record.
- **Counter-tick spin resolution.** The quarter-bit re-spin covers a spin
  that ends before the flag.
- **The first step's admission** (eight reads and two lock waits) is
  unchanged.

## Integration state

The power-domain design and runtime documents use the provisional
1,268-permille median limit and the first-step allowance of another 40 µs.
The `pmic-tree` source check enforces the adapter boundary: only the
provider, native adapter and bundle power layer may use its operations.
Consumers use `a733_pmic_native_bind()`, `a733_pmic_now()` and
`a733_pmic_outside()`.

The bundle power layer already includes the access-cost windows, write
stamps, wire-time spin and re-spin records. It uses the `pmic_bus_ops`
member and the current power-domain check signature. The bundle builder
admits the changed provider objects through its explicit overlay.

The trace evaluator, distribution labels, host fixtures, controlled-outcome
helper and bundle preparation row use 1,268 permille. The seventh set's
bundle 6 image applies it and has not run. The fifth and sixth sets'
preserved images and runbooks retain their original 1,550-permille
acceptance limit; their existing passes do not establish execution of the
revised evaluator.
