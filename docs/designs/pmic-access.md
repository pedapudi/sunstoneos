# PMIC register access and service spin timing

Status: **design, not implemented**. Every step time below comes from the
host model of the power service (`checks/power-domains-service.c`) run on an
uncommitted copy with this design applied
(`~/.cache/sunstoneos/build/pmic-access/v2/`). The model sets each bus flag
at a sub-microsecond time, the write's completion time plus the phase's
wire time. The figures are estimates until the check cases listed under
"Measurement" are committed.

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

1. **One time read per bus phase.** The provider's `rd()` and `wr()` make no
   time read. `transfer()` reads the time once after each phase's accesses
   and checks the request deadline and the monotonic clock with it. After a
   phase that starts a bus event (a START, repeated START, address, register
   index, data byte, receive or STOP write), the provider stores that time as
   `phase_at`. Request admission and startup check the deadline once after
   their read sequences. This is item 5 of section 2 of
   [performance-architecture.md](../performance-architecture.md).
2. **Task check once per poll.** The native adapter's `owned()`, `read32()`
   and `write32()` check the `holding` flag, one acquire load of the I2C
   lock's retained bit and the address allowlist. The task check
   (`a733_task_context_owner_el1`) runs where it already runs: at the entry
   of every `a733_pmic_poll()` and in the adapter's `enter()`, which also
   asserts the kernel mutex holder (section 1.1 of the same document). The
   `DSB SY` before and after every access stays.
3. **STOP wait as the bootloader does it.** After a STOP write, the provider
   reads the control register (`CNTR`, `+0x0c`). Bit 4 (`M_STP`) still set is
   PENDING. Once it is clear, the low byte must be `0x40`, else the provider
   faults. It then reads the status register (`+0x10`); a value other than
   `0xf8` is PENDING. Both waits are bounded by the request deadline.
4. **Line-control read at the ends of a request.** The line-control idle
   check (`+0x20`) runs at admission and after the request's final STOP,
   instead of after every STOP.
5. **Spin from the phase-start write.** The service spins until
   `max(phase_at + phase duration, now + bit time / 4)`, in whole
   microseconds, rounded up, where `now` is read after the poll returns. The
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
  word before each further phase and at publication, and the lock's retained
  bit on every access.
- **Bounded failure.** Inside `transfer()`, a deadline that passes during a
  phase is detected after at most that phase's accesses, three at most (an
  address phase writes `DATA` and `CNTR`; the final STOP wait reads three
  registers). Admission runs up to eight reads, and startup about 24, before
  their one check. A `CNTR` write, including the one that sends the
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

No register write changes. The A733 User Manual V1.00, section 18.11.9.4,
states that `M_STP` clears automatically and that the idle state `0xF8` is
the only state that does not set `INT_FLAG`. With `A_ACK` and `INT_EN`
never written, `CNTR` therefore reads `BUS_EN` alone, `0x40`, once `M_STP`
clears. Section 18.11.9.5 lists `0xF8` as "no relevant status information,
INT_FLAG=0". The pinned bootloader `b791be842935b27268ae3d00e943a9075495f30a`,
`drivers/i2c/sunxi_i2c.c`, waits after STOP for status `0xf8` with a timeout
(`twi_stop()`, lines 469–483). It reads line control only before a clock
change (line 497) and in its bus-recovery pulses (lines 263–297), never after
a STOP. The provider keeps the line-control check at admission and after the
final STOP, so each request still starts and ends with both lines released.

## Budgets

Percentages are of the bit-time wire time: 630 µs for a snapshot and
812.5 µs for a selector write (325 bit times). The model's bytes take 23 µs,
so its own snapshot wire is 642 µs (102%). Rows at 200 ns per access keep the
check's 40 ns per time read. The board-cost rows charge 100 ns per time read,
an estimate for an `ISB`, a counter read and a division at 494 MHz, which
the next bundle 6 measures. *X* is the cost of one provider register access
after the change.

| Metric | Baseline | Design | Measured by |
| --- | --- | --- | --- |
| Time reads per DCDC2 snapshot; per DCDC3 selector write | 469; 599 | 180; 229 | `checks/a733-pmic-cost.c --limits` |
| Register accesses per snapshot; per selector write | 211; 270 | 200; 255 | `checks/a733-pmic-cost.c --limits` |
| Task checks per register access | 1, plus 1 per poll | 0, plus 1 per poll | `checks/pmic-tree.py` |
| Serial accesses between a flag and the next phase-start write | 27 per transaction | 25 per transaction, about 150 per snapshot | Counted from the phase sequence |
| Snapshot step, 400 kHz, 200 ns per access | 712 µs (113%) | 685 µs (109%) | `checks/power-domains-service.c` |
| Snapshot step, 400 kHz, 1,300 ns per access | 915 µs (145%) | 845 µs (134%) | Same, board-cost case |
| Snapshot step, 400 kHz, *X* = 850 ns; 900 ns | Not applicable; the current path fits 1,300 ns | 758 µs (120%); 782 µs (124%) | Same, board-cost cases |
| Selector write, 400 kHz, *X* = 850 ns; 900 ns | Two steps, one tick wait | One step of 975 µs; two steps, one tick wait | Same |
| Snapshot, 400 kHz, flags 10% later than the divisor implies, 200 ns | Two steps, one tick wait | One step of 751 µs for 708 µs of model wire | Same, slow-bus case |
| Tick waits per snapshot and selector write, 100 kHz | 2 and 3 | 2 and 3 | Same |
| BUSY results per other-core caller; from `a733_pmic_live` | At most 1; 0 | At most 1; 0 | Same |

With 40 ns per time read the selector write stays in one step up to 950 ns
per access. Each phase boundary keeps the flag read, the status read and,
except before a repeated START or a receive, the next `DATA` write, then the
`CNTR` write. With `DSB SY` on both sides of every access the step stays near
wire time plus 150 *X*. The model reaches 110% of wire time only at about
300 ns per access, below the owner's fastest measured read interval (0.42 µs
between CCU reads; 0.71 µs median in the region that holds S_TWI0). 110%
therefore needs a lighter barrier placement, which this design leaves out.

**Bound.** The provisional bound of 155% of wire time for a 400 kHz snapshot
step stays until the next bundle 6 has measured *X*. The post-change bound is
then the model's step at the measured *X* plus a margin, applied to the median
step, with a separate bound for the first step, which ran 24 µs above the
median on the board. The model's step is taken as its maximum over *X*
within the measurement's uncertainty, at the stated core clock (494 MHz on
CPU-L in the measured lifetime).

## Measurement

Committed checks, added with the implementation:

- `checks/power-domains-service.c`: a table of board-cost cases, 850 ns and
  900 ns (the selector-write bracket) and 1,300 ns, at 100 ns per time read;
  and the slow-bus case. Every service case runs with sub-microsecond flags.
  These cases replace the review estimates of 856 µs at 1,300 ns and 776 µs
  at 900 ns, which came from a model copy that timed the spin from the phase
  start and changed nothing else.
- `checks/a733-pmic.c` model: flags set from the write's completion time in
  nanoseconds.
- `checks/a733-pmic-cost.c --limits`: the counts in the table.

Board measurement on the next bundle 6, which still runs the current
provider:

1. **Run the cost windows in the service task**, on the provider's core at
   its clock, with the I2C mutex held between transfers; record the core
   number and CPU-L rate. If they stay in the client task, record both
   tasks' cores and clocks.
2. **Windows**, 256 iterations each:
   - `CNTR` reads with `a733_mmio_read32_dsb_sy_both`;
   - plain `CNTR` reads whose value is discarded, labelled as issue
     throughput;
   - plain `CNTR` reads where each value forms the next read's address (an
     address dependency), labelled as latency;
   - time reads through the adapter's `now`;
   - the adapter's `read` operation, the current path;
   - `bb_twi_observe()` on a scratch record: the trace observer that
     `apps/board-bundles-power/power.c` lines 25–37 runs on every access.
3. **Writes**, timed inside the snapshots' own transfers: the `pmic_write`
   wrapper stamps the counter around the adapter's `write` and sums the
   ticks, with no extra bus write.
4. **Wire time**: once, after a byte-phase `CNTR` write, `bb_status_spin()`
   reads `CNTR` back to back until `INT_FLAG` sets. That step is excluded
   from the step statistics.
5. **Baseline check.** The adapter-read window plus two time reads plus the
   observer must reproduce the 1,300 ns fit within 10%; the barrier window
   then gives the bus part of *X*.

After the change, bundle 6 is rebuilt and makes 32 snapshots of DCDC2 and
DCDC4. Pass: the median and first steps within their bounds set as above;
snapshot bytes unchanged (`0xff9e`); no fault; re-spins reported. A DCDC3
selector write in one step needs a separate bundle with a PMIC write, which
needs the user's approval.

**Requalification.** The provider and adapter change affects every image
that links them. The board-qualified ones are the native USB channel image
(`rank-9-usb-v4/`, rank 9 of the board-bundle table, four supply snapshots)
and the returning thermal image (rank 6, a CLDO3 snapshot,
`apps/thermal-returning/adc.c`). Both are rebuilt and rerun with their
qualifying pass conditions, independent of the selector-write run. Bundle 7
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
  `a733_task_context_` function; fail if any source other than
  `drivers/power/a733_pmic.c` and the forwarders in
  `apps/board-bundles-power/power.c` calls the adapter's `read` or `write`
  operation.
- `apps/board-bundles-power/bundle6.c`, `power.c`, `trace.c` and `trace.h`,
  for the next refreeze: the windows, write timing, wire-time spin and core
  and clock record of the measurement plan, in place of the current
  `access_cost()`, whose plain window discards each value.
- `docs/designs/board-bundles-next.md`: the bundle 6 row's measurements and
  bound procedure.
