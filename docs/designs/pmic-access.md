# PMIC register access and service spin timing

Status: **design, not implemented**. Every step time below comes from the
host model of the power service (`checks/power-domains-service.c`) run on an
uncommitted copy with the changes of this design applied
(`~/.cache/sunstoneos/build/pmic-access/final/`). The figures are estimates
until the check cases listed under "Measurement" are committed.

The PMIC provider (`drivers/power/a733_pmic.c`) reads the AXP8191 over the
always-on I2C controller S_TWI0. In five images its worker is the power
service task (`drivers/power/a733_power_domains.c`), which spins on the
counter while a bus phase is on the wire. On port 3-3 on 2026-10-24, the
read-only board bundle 6 measured a DCDC2 or DCDC4 snapshot step at 917 µs
(median of 34 steps; 941 µs in the first step). A snapshot is 252 bit times
at 400 kHz, 630 µs of wire time, so the step is 146% of wire time (149.4% in
the first step) against the power-domain owner's 110% target. The raw log is
`~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/bb6-run/kernel.log`.
The model reproduces the median when a register access costs 1,300 ns. About
half of that is provider software: each access makes two time reads, each an
`ISB`, a counter read and a 64-bit division, and a task check that masks
IRQs and calls two FreeRTOS functions. The rest is the access with its two
`DSB SY`.

## Change

1. **One time read per bus phase.** The provider's `rd()` and `wr()` make no
   time read. `transfer()` reads the time once after each phase's accesses
   and checks the request deadline and the monotonic clock with it. After a
   phase that starts a bus event (a START, repeated START, address, register
   index, data byte, receive or STOP write), the provider stores that time as
   `phase_at`, the time the phase-start write completed. Request admission
   and startup check the deadline once after their read sequences. This is item 5 of section 2
   of [performance-architecture.md](../performance-architecture.md).
2. **Task check once per poll.** The native adapter's `read32()` and
   `write32()` check the `holding` flag, one acquire load of the I2C lock's
   retained bit and the address allowlist. The task check
   (`a733_task_context_owner_el1`) runs where it already runs: at the entry
   of every `a733_pmic_poll()` and in the adapter's `enter()`, which also
   asserts the kernel mutex holder (section 1.1 of the same document). The
   `DSB SY` before and after every access stays.
3. **One control read after STOP.** The STOP wait reads the control register
   (`CNTR`, `+0x0c`) once and applies both of its conditions to that value.
   Bit 4 clear ends the wait, and the low byte must then equal `0x40`: the
   idle check that `bus_idle()` makes today with a second read. The status
   (`+0x10`) and line-control (`+0x20`) idle reads stay.
4. **Spin from the phase-start write.** The service spins until
   `max(phase_at + phase duration, now + bit time / 4)`, in whole
   microseconds, rounded up, where `now` is read after the poll returns. The first term times the wire from the write
   that started it, so the provider's work after that write overlaps the
   bus. The second term is the minimum re-spin after a poll that finds the
   flag still clear, which keeps polls at least a quarter bit time apart.
   The backstop on polls per step, `PMIC_POLLS_PER_STEP`, rises from 64 to
   256: with a slow bus, a selector write needs about 56 polls plus up to
   two re-spins per phase.

## Safety properties

- **Ownership.** The provider object's try-lock, the poll's task check, and
  the I2C mutex taken by `enter()` on the provider task are unchanged. Only
  `a733_pmic_poll()` reaches `rd()` and `wr()`, and it holds the provider
  object and has checked the task at entry, so a per-access task check adds
  no case. `checks/pmic-tree.py` fails if an accessor queries the mutex
  holder; the edit listed at the end also makes it fail if an accessor calls
  the task-context predicate.
- **Revocation.** Unchanged: one acquire load of the power-authority state
  word before each further phase and at publication, and the lock's retained
  bit on every access.
- **Bounded failure.** A deadline that passes inside a phase is detected
  after at most that phase's accesses, three at most (an address phase
  writes `DATA` and `CNTR`; the STOP wait reads three registers). The poll
  entry still checks the deadline before any access. A fault still retains
  the request, the held mutex and the leases.
- **The 1 ms spin limit.** Unchanged: `pmic()` returns one tick after
  `A733_POWER_PMIC_SPIN_US` of spinning in a step, and `SPIN_READS` still
  bounds each spin if the clock stops.
- **Bus protocol.** Every write, its value and its order are unchanged. Each
  phase still reads the flag, then the status, and checks the status code.
  The merged STOP read applies the same two conditions to one value.

The interrupt, memory and core arrangement does not change. S_TWI0's
interrupt (GIC SPI 203) is unqualified, and a 400 kHz byte is 22.5 µs, far
below a tick, so the service still spins. The provider object, `phase_at`
and the adapter state stay in kernel `.bss` (Normal write-back); S_TWI0 stays
Device-nGnRnE. `phase_at` is written and read only by the provider task.
Clients on other cores still see BUSY only inside a poll; the minimum
re-spin keeps the polls of a late phase apart, and the model's bound of one
BUSY result per caller still holds.

## Register sequences and sources

No new register sequence. The field meanings come from the A733 User Manual
1.00, sections 18.11.8 to 18.11.10, and the transaction order from the
pinned bootloader `b791be842935b27268ae3d00e943a9075495f30a`,
`drivers/i2c/sunxi_i2c.c`, as cited in [pmic-runtime.md](../pmic-runtime.md).
The bootloader's `twi_stop()` (lines 469–483) waits only for status `0xf8`.
No source states that `CNTR` reads `0x40` in the same read where bit 4
clears; that is **unproven**. If it does not hold, the merged read retains a
fault instead of passing, and the board run below checks it over 32
snapshots.

## Budgets

Percentages are of the bit-time wire time: 630 µs for a snapshot and
812.5 µs for a selector write (325 bit times). The model's bytes take 23 µs,
so its own snapshot wire is 642 µs (102%). The design model charges 100 ns
per time read, except in the 200 ns row, which keeps the check's 40 ns.
*X* is the measured cost of one provider register access after the change,
which the next bundle 6 supplies.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Time reads per DCDC2 snapshot; per DCDC3 selector write | 469; 599 | 180; 229 | `checks/a733-pmic-cost.c --limits` |
| Register accesses per snapshot; per selector write | 211; 270 | 205; 262 | `checks/a733-pmic-cost.c --limits` |
| Task checks per register access | 1, plus 1 per poll | 0, plus 1 per poll | `checks/pmic-tree.py` |
| Instructions on the provider read path | About 220 (disassembled, bundle 6 image) | At most 60: two `DSB SY`, one acquire load, no FreeRTOS call | New `checks/a733-pmic-native-path.py`, disassembly of the AArch64 objects |
| Snapshot step, 400 kHz, 200 ns per access | 687 µs (109%) | 644 µs (102%) | `checks/power-domains-service.c` |
| Snapshot step, 400 kHz, 1,300 ns per access | 915 µs (145%) | 850 µs (135%) | Same, board-cost case |
| Snapshot step, 400 kHz, at *X* | 917 µs on the board | 731 µs (116%) at 600 ns; 762–767 µs (121–122%) at 700–875 ns; 782 µs (124%) at 900 ns | Same, cases at 600, 700, 850 and 900 ns; bundle 6 |
| Selector write, 400 kHz | Two steps, one tick wait (model, 1,300 ns) | One step, no tick wait, while *X* ≤ 850 ns (982 µs, 121%); two steps at 900 ns | Same; a board write run |
| Snapshot, 400 kHz, flags 10% later than the divisor implies | Two steps, one tick wait | One step of 710 µs for 708 µs of model wire | Same, new slow-bus case |
| Tick waits per snapshot and selector write, 100 kHz | 2 and 3 | 2 and 3 | Same |
| BUSY results per other-core caller; from `a733_pmic_live` | At most 1; 0 | At most 1; 0 | Same |

Each phase boundary keeps four serial accesses (flag read, status read, the
next `DATA` and `CNTR` writes), 42 boundaries per snapshot. With a `DSB SY`
on both sides of every access, the step therefore stays near
wire + 168 *X*. The model reaches 110% (693 µs) only at about 400 ns per
access, below the owner's fastest measured read interval (0.42 µs between
CCU reads; 0.71 µs median in the region that holds S_TWI0). The 110% target
therefore needs a lighter barrier placement; this design reaches about 121%.
The proposed bound for the power-domain owner is a snapshot step of at most
125% of wire time, and a selector write in one step when *X* ≤ 850 ns.

## Measurement

Committed checks, added with the implementation:

- `checks/power-domains-service.c`: the board-cost case at 1,300 ns with the
  new spin; cases at 600, 700, 850 and 900 ns, which reproduce the figures
  above; and a slow-bus case. They replace the review estimates of 856 µs at
  1,300 ns and 776 µs at 900 ns, which came from a copy of the model that
  timed the spin from the phase start and changed nothing else.
- `checks/a733-pmic.c` model: flags set at sub-microsecond times, from the
  write time with its fractional nanoseconds.
- `checks/a733-pmic-cost.c --limits`: the counts in the table.

Board re-measurement, in order:

1. **Access cost, next bundle 6.** Its `TWI_COST` line times 256 `CNTR`
   reads with `a733_mmio_read32_dsb_sy_both`, 256 plain reads and 256 time
   reads. The barrier window gives the completed-read latency. The plain
   window discards each value, so it measures how fast Device loads issue;
   it must consume each value or be labelled as throughput. Writes are
   assumed to cost the same as reads: no S_TWI0 register is shown to be
   safe to write outside a transaction, because `DATA` and `CNTR` writes
   act on the bus. *X* is the barrier-window cost plus the counted lean path
   at the measured core clock (494 MHz on CPU-L in that lifetime); the
   model then predicts the step.
2. **Steps after the change, bundle 6 rebuilt.** 32 snapshots of DCDC2 and
   DCDC4. Pass: every step at most the model's prediction at *X* plus 3%,
   and at most 125%; `polls` at most 2 in every step (no re-spin) or the
   re-spins reported; snapshot bytes unchanged (`0xff9e`); no fault.
3. **Selector write.** One DCDC3 write in one step, from a bundle with a PMIC
   write, which needs the user's approval.
4. **Requalification.** The provider and adapter change affects every image
   that links them. The board-qualified ones are the native USB channel
   image (`rank-9-usb-v4/`, rank 9 of the board-bundle table, four supply
   snapshots) and the returning thermal image (rank 6, a CLDO3 snapshot,
   `apps/thermal-returning/adc.c`). Both are rebuilt and rerun with the same
   pass conditions as their qualifying runs. The spin change affects only
   images that run the power service: GPU, accelerator, GPU display, HDMI,
   the DisplayPort and G2D board image, and the integrated OS image built
   from it. None of them is board-qualified.

## Left out

- **Lighter barriers** (section 1.2): the only route to 110%, and they need
  the board qualification of [shared-helpers.md](../shared-helpers.md).
  Bundle 6's barrier-free window, once it consumes each value, gives the
  expected gain.
- **Reading status alone to detect completion.** It would remove one serial
  read per phase, but no pinned source shows that status reads `0xf8`
  whenever the flag is clear, and the bootloader reads the flag first.
- **Removing the flag read right after each phase-start write.** It runs
  while the bus is busy, so it costs no step time. It also feeds the bundle 6
  record.
- **Counter-tick spin resolution.** The microsecond clock places the spin end
  within ±0.5 µs of the flag; the quarter-bit re-spin covers late flags.
- **The first step's admission** (eight reads and two lock waits), 24 µs
  above the median on the board, is unchanged.

## Edits outside the owned files

- `docs/designs/power-domains.md` and `docs/power-domains.md`, which another
  package is editing: the spin is timed from the provider's phase-start time
  with a quarter-bit minimum re-spin; 256 polls per step; the provisional
  155% bound becomes 125%, with 110% conditional on a qualified barrier
  placement; the selector-write board evidence above.
- `checks/pmic-tree.py`: fail if `read32()` or `write32()` call
  `a733_task_context_owner_el1`.
- `apps/board-bundles-power/bundle6.c` and `trace.c`, for the next refreeze.
  Consume each plain read in `access_cost()` with an empty `asm` that takes
  the value as input, or name the window as throughput in the `TWI_COST`
  line; state in the bundle's documentation that writes are assumed to cost
  the same as reads.
- `docs/designs/board-bundles-next.md`: the bundle 6 row's bound.
