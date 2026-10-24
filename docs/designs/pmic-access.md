# PMIC register access and service spin timing

Status: **implemented and host-modeled**; no board run has used it. Every
step time below comes from the host model of the power service,
`checks/power-domains-service.c`, which sets each bus flag at a
sub-microsecond time, the write's completion time plus the phase's wire
time. [pmic-access-measurements.md](pmic-access-measurements.md) records the
measured rows against the base tree.

The PMIC provider (`drivers/power/a733_pmic.c`) reads the AXP8191 over the
always-on I2C controller S_TWI0. In five images its worker is the power
service task (`drivers/power/a733_power_domains.c`), which spins on the
counter while a bus phase is on the wire. On port 3-3 on 2026-10-24, the
read-only board bundle 6 measured a DCDC2 or DCDC4 snapshot step at 917 µs
(median of 34 steps; 941 µs in the first step). A snapshot is 252 bit times
at 400 kHz, 630 µs of wire time, so the step is 146% of wire time (149.4% in
the first step). The raw log is
`~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/bb6-run/kernel.log`.
The model reproduces the median when a register access costs 1,300 ns. An
estimated half of that is provider software: each access makes two time
reads, each an `ISB`, a counter read and a 64-bit division, and a task check
that masks IRQs and calls two FreeRTOS functions. The rest is the access with
its two `DSB SY`. The run did not measure that split.

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
check's 40 ns per time read. The board-cost rows charge 100 ns per time read,
an estimate for an `ISB`, a counter read and a reciprocal multiply at
494 MHz. They also charge 200 ns per provider poll, for its task check, the
provider object's try-lock and the per-phase software that no per-access
figure holds. Writes cost as much as reads. The next bundle 6 measures each of
these, and the rows then take the measured values. *X* is the cost of one
provider register read after the change; the measurement section defines it
from the board windows.

| Metric | Baseline | Design | Measured by |
| --- | --- | --- | --- |
| Time reads per DCDC2 snapshot; per DCDC3 selector write | 469; 599 | 137; 174 | `checks/a733-pmic-cost.c --limits` |
| Register accesses per snapshot; per selector write | 211; 270 | 200; 255 | `checks/a733-pmic-cost.c --limits` |
| Task checks per register access | 1, plus 1 per poll | 0, plus 1 per poll | `checks/pmic-tree.py` |
| Serial accesses between a flag and the next phase-start write | 27 per transaction | 25 per transaction, about 150 per snapshot | Counted from the phase sequence |
| Snapshot step, 400 kHz, 200 ns per access | 712 µs (113%) | 685 µs (109%) | `checks/power-domains-service.c` |
| Snapshot step, 400 kHz, 1,300 ns per access, 200 ns per poll, 100 ns per time read | 947 µs (150%); 915 µs (145%) with the fit's 40 ns per time read and no poll cost | 846 µs (134%) | Same, board-cost table |
| Snapshot step, 400 kHz, *X* = 800 ns; 850 ns, same poll and time-read costs | 854 µs; 855 µs | 758 µs (120%); 782 µs (124%) | Same |
| Selector write, 400 kHz, *X* = 800 ns; 850 ns | Two steps, one tick wait | One step of 975 µs; two steps, one tick wait | Same |
| Snapshot, 400 kHz, flags 10% later than the divisor implies, 200 ns | Two steps, one tick wait | One step of 751 µs for 708 µs of model wire | Same, slow-bus case |
| Tick waits per snapshot and selector write, 100 kHz | 2 and 3 | 2 and 3 | Same |
| BUSY results per other-core caller; from `a733_pmic_live` | At most 1; 0 | At most 1; 0 | Same |

Without the per-poll cost the selector write stays in one step up to 850 ns
per access, and with 40 ns per time read up to 950 ns; 200 ns per poll lowers
that limit to 800 ns. Each phase boundary keeps the flag read, the status read and,
except before a repeated START or a receive, the next `DATA` write, then the
`CNTR` write. With `DSB SY` on both sides of every access the step stays near
wire time plus 150 *X*. The model reaches 110% of wire time only at about
300 ns per access, below the owner's fastest measured read interval (0.42 µs
between CCU reads; 0.71 µs median in the region that holds S_TWI0). 110%
therefore needs a lighter barrier placement, which this design leaves out.

**Bound.** The provisional bound of 155% of wire time for a 400 kHz snapshot
step stays until the next bundle 6 has measured the costs. The post-change
bound is then the model's step with the measured read, write, poll and
time-read costs, each taken at the upper end of its measured range, plus
20 µs (3% of the 630 µs wire), applied to the median step. The upper end of
a window's range is its slowest of eight sub-windows of 32 iterations,
divided by 32. The model also charges, per register write, the trace
observer call and the two counter stamps that the bundle adds (about 60 per
snapshot), since those lie inside the measured steps. The model's byte, and
the bundle's own wire time per step, take the measured byte: the lower
bracket of the wire-time spin when it exceeds nine bit times, at most 110%
of nine bit times. A measured byte that is missing or above 110% fails the
bound, and the raw stamps are printed. It fails rather than being capped at
110%: the run measures the provider's cost per step against the wire time
that the divisor sets, and a byte that much longer means the bus itself ran
slower (clock stretching or a wrong divisor). A capped wire time would
charge that bus time to the provider, so such a run needs a diagnosis of
the bus before any bound is set from it. A spin with fewer than two reads
has no read with the flag clear and therefore no lower bracket. Bundle 6
then takes nine bit times, as for a missing sample, and its `WIRE` line
shows `reads=1`; the post-change model treats that sample as missing. Unlike
a missing sample it does not fail the bound in the fifth set's image, which
stays byte-identical while it runs; with nine bit times the verdict can
only be stricter. The first step has its own bound,
40 µs above the median bound: on the board it ran 24 µs above the median.
Both hold at the stated core clock (494 MHz on CPU-L in the measured
lifetime). Re-spins are reported per step and in the summary and do not
gate the measurement run: the service model gives no re-spin at a 23 µs
byte and 18 to 24 per snapshot at 24 µs, with steps at 128% to 130% of wire
time, inside 155%. Each re-spin costs a quarter bit time and one poll, about
1.5 µs. Their measured count sizes the counter-tick spin resolution left out
below.

## Measurement

Committed checks, added with the implementation:

- `checks/power-domains-service.c`: a table of board-cost cases, each with a
  read, a write, a per-poll and a time-read cost: 800 ns and 850 ns (the
  selector-write bracket with 200 ns per poll) and 1,300 ns; and the
  slow-bus case. Every service case runs with sub-microsecond flags.
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

Board measurement on the next bundle 6. A bundle built from this tree runs
the changed provider, so the windows give *X* directly, and one window times
the task check that the earlier adapter made per access:

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
   snapshot, are inside the run's step times, so its steps are compared with
   the model at the measured cost and never with the earlier 917 µs median.
4. **Wire time**: once, with IRQs masked from before a byte-phase `CNTR`
   write, `bb_status_spin()` reads `CNTR` back to back until `INT_FLAG`
   (bit 3) reads set. The record keeps the stamp of the last read with the
   flag clear and of the first with it set, both from the write's return;
   the byte is the lower one. That step is excluded from the step
   statistics and counted as excluded.
5. **Baseline check.** The adapter-read window plus the task-check window,
   two time-read windows and the observer window reconstructs the earlier
   per-access path. It must come within 10% of 1,380 ns: the 1,300 ns fit
   plus the two 40 ns time reads that the model charged on top of it. The
   fit also holds per-phase and per-poll software that no window measures, so
   a reconstruction below that range means the windows miss part of the
   cost, and *X* is then taken from the upper end of its range.

*X* is defined conservatively as the adapter-read window plus the observer
window per access, which is what the bundle's steps pay; production images
pay the adapter read alone. The write cost comes from the in-transfer write
timing. The per-poll cost is the task-check window plus the remainder of the
baseline check: 1,380 ns minus the reconstruction, spread over the 211
accesses of a snapshot and charged per poll at the 44 polls of a snapshot.
The step records count re-spins: control-register polls beyond the second
of a flag phase or of a STOP wait. A STOP wait lasts until `M_STP` reads
clear and the status register reads `0xf8`.

After the change, bundle 6 makes 32 snapshots of DCDC2 and DCDC4. Pass: the
median and first steps within their bounds set as above; a measured byte
within 110% of nine bit times; snapshot bytes unchanged (`0xff9e`); no
fault. Re-spins are reported. A DCDC3 selector write in
one step needs a separate bundle with a PMIC write, which needs the user's
approval.

**Requalification.** The provider and adapter change affects every image
that links them. The board-qualified one is the native USB channel image
(`rank-9-usb-v4/`, rank 9 of the board-bundle table, four supply snapshots).
It is rebuilt and rerun with its qualifying pass conditions, independent of
the selector-write run. The returning thermal image of rank 6 does not link
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

## Left out

- **Lighter barriers**, the only route to 110%. They need the board
  qualification of [shared-helpers.md](../shared-helpers.md). The intended
  placement, per section 1.2 of the performance architecture: the flag and
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

## Edits outside the owned files

- `docs/designs/power-domains.md` and `docs/power-domains.md`, which another
  package edits: the spin timed from the provider's phase-start time with a
  quarter-bit minimum re-spin; 256 polls per step; the bound procedure above,
  with 155% kept until the access-cost run.
- `checks/pmic-tree.py`: fail if `owned()`, `read32()` or `write32()` in
  `drivers/power/a733_pmic_native.c` call `current(` or any
  `a733_task_context_` function. Fail when a source other than the
  provider, its adapter and `apps/board-bundles-power/power.c` and
  `power.h` names `pmic_bus_ops`, `a733_pmic_ops` or `a733_pmic_init`.
  `pmic_bus_ops` is the member of `struct a733_pmic_native` and
  `struct a733_pmic` that holds the adapter operations; no other code uses
  the name. Other sources use three helpers instead:
  `a733_pmic_native_bind()` passes the native adapter to
  `a733_pmic_init()`, and `a733_pmic_now()` and `a733_pmic_outside()` make
  the two consumer queries. A name built by token pasting, or a call through
  a pointer cast to the adapter's first member, is not caught. Planted uses
  in `apps/thermal-returning/adc.c` and `apps/npu-returning/main.c` fail it.
- `apps/board-bundles-power/bundle6.c`, `power.c`, `power.h`, `trace.c` and
  `trace.h`, for the next refreeze: the windows at the park, write timing,
  wire-time spin, re-spin counts and core record of the measurement plan, in
  place of the client's `access_cost()`. The same edit passes the third argument of
  `a733_power_domain_check()`, which `bundle6.c` omits on the current tree.
  `power.c` also names the adapter member `pmic_bus_ops`, which replaced
  `operations`; the frozen bundle 6 image keeps the old name until this
  refreeze.
- `docs/designs/board-bundles-next.md`: the bundle 6 row's measurements and
  bound procedure.
- `checks/board-bundles-apps-model.c`, in the patch: the step records'
  re-spin field and a re-spin case of the TWI observer.
- `checks/board-bundles-apps.py` and `tools/prepare-board-bundles.py`, not in
  the patch, at the next refreeze: bundles 6 and 7 reuse rank-9-usb-v4's
  provider objects and require each to recompile byte-identical from the
  tree. The changed provider, adapter and consumer, and the layout of
  `struct a733_pmic`, make those objects new units of the next bundle 6. The
  bundle 6 row text of `tools/prepare-board-bundles.py` still describes the
  client's earlier cost windows.
