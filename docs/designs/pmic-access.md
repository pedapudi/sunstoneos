# PMIC register access and service spin timing

Status: **implemented and measured on the board.** Bundle 6 ran the changed
provider four times on port 3-3 on 2026-10-24, read-only. Three runs used a
`DSB SY` pair around every access. The fourth used the barrier placement of
item 6 of section "Change" and qualified it for snapshots. Section "Board result"
gives the steps, the access costs, the provisional empirical bound and what
the measured costs predict. The modeled step times come from the host model
of the power service, `checks/power-domains-service.c`, which sets each bus
flag at a sub-microsecond time, the write's completion time plus the phase's
wire time. [pmic-access-measurements.md](pmic-access-measurements.md)
records the modeled rows against the base tree.

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
The changed provider's step is 779 to 784 µs (median), 124% of wire time.
At the measured access costs the model gives 739 µs, and no timed window
accounts for the remaining 45 µs (section "Board result").

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
   asserts the kernel mutex holder (section 1.1 of the same document).
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

6. **Barrier placement.** The native adapter reads S_TWI0 plainly, writes
   `DATA` plainly and completes each `CNTR` write with one `DSB SY`
   (`a733_mmio_write32_dsb_sy_after`), which makes `phase_at` follow the
   write's completion. Its reads of R_CCU, CCU, GIC and R_PIO at admission
   and startup keep `DSB SY` on both sides. Section 1.2 of the performance
   architecture prescribes this placement. A bundle 6 run on port 3-3
   qualified it for snapshots (section "Board result"), and
   `checks/pmic-tree.py` keeps the placement.

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
check's 40 ns per time read. The board-cost rows use the costs that bundle 6
measured (section "Board result"): 505 ns per register read, 403 ns per
write, 145 ns per time read and 527 ns per provider poll beyond its time read
and flag read.

| Metric | Baseline | Design | Measured by |
| --- | --- | --- | --- |
| Time reads per DCDC2 snapshot; per DCDC3 selector write | 469; 599 | 137; 174 | `checks/a733-pmic-cost.c --limits` |
| Register accesses per snapshot; per selector write | 211; 270 | 200; 255 | `checks/a733-pmic-cost.c --limits` |
| Task checks per register access | 1, plus 1 per poll | 0, plus 1 per poll | `checks/pmic-tree.py` |
| Serial accesses between a flag and the next phase-start write | 27 per transaction | 25 per transaction, about 150 per snapshot | Counted from the phase sequence |
| Snapshot step, 400 kHz, 200 ns per access | 712 µs (113%) | 685 µs (109%) | `checks/power-domains-service.c` |
| Snapshot step, 400 kHz, measured costs | Not rerun | 739 µs (117%) | Same, board-cost table |
| Snapshot step on the board, median; first step | 917 µs (146%); 941 µs | 779 µs (123.7%) and 784 µs (124.4%); 809 µs and 807 µs; 796 µs (126.4%) and 824 µs with the completing-poll window; 795 µs (126.1%) and 819 µs with that window and the item 6 placement | Bundle 6 `STEP` lines of the four runs, port 3-3 |
| Selector write, 400 kHz, measured costs | Not rerun | One step of 950 µs; not measured on the board | Same |
| Snapshot, 400 kHz, flags 10% later than the divisor implies, 200 ns | Two steps, one tick wait | One step of 751 µs for 708 µs of model wire | Same, slow-bus case |
| Tick waits per snapshot and selector write, 100 kHz | 2 and 3 | 2 and 3 | Same |
| BUSY results per other-core caller; from `a733_pmic_live` | At most 1; 0 | At most 1; 0 | Same |

The DCDC3 selector-write acceptance bound remains at most two steps with
one tick wait. A one-step result is not required and has not been measured
on the board. At the poll cost that reproduces the board's snapshot median,
the model splits a selector write into two steps with one tick wait. The
service checks its 1 ms spin budget between polls, so a final poll can
finish slightly later than 1 ms.

**110% is not reachable by the register accesses of this design.** The
target is 693 µs for a snapshot. With every software cost set to zero, the
model charges only the register accesses and a 35 ns counter read per time
read. It then gives 712 µs (113%) with a `DSB SY` pair around each access
and 694 µs (110.2%) with the adapter's placement (item 6 of section
"Change"). Each phase
of a snapshot spins its rounded wire time, 23 µs for a byte and 5 µs for a
condition, which already makes 642 µs of the 693 µs. The 51 µs that remain
cover 42 phases, about 1.2 µs each. The serial register accesses of a
phase that writes `DATA` and `CNTR` take about 1.2 µs with the lighter
placement (two plain reads, a plain write and a completed write) and 1.4 µs
with `DSB SY` pairs. Any route to 110% therefore needs fewer serial
register accesses per snapshot. The S_TWI0 packet engine (the `TWI_DRV`
registers from `+0x200`, which startup requires disabled) sequences the
conditions and bytes of a transaction in hardware. It would leave one
software turnaround per transaction instead of one per phase, six per
snapshot instead of 42. It is not designed or qualified here. Reading the
status register alone to detect completion, under "Left out", removes one
read per phase but not enough to reach 110% at the present software cost.

**Provisional empirical bound.** At 400 kHz and CPU-L at 494 MHz, bundle 6
uses a median limit of 1,268 permille of each step's own measured wire
time. The first-step limit adds 40 µs to that wire-relative limit. Both
are the board's figures plus a margin: the observed median, 779 µs, plus
about 20 µs gives 799 µs, and the first-step limit, 839 µs, lies 30 µs
above the observed first step of 809 µs. Those microsecond values hold at
the nominal 22.5 µs byte, where a snapshot's wire time is 630 µs (798.840 µs
and 838.840 µs exactly); at another measured byte the permille limit
applies to that step's wire time. This is an empirical acceptance limit
set from the first run; no model or cost measurement derives it, and it is
not a worst-case bound. The second run, in another lifetime, applied it and
passed: a median of 1,243 permille and a first step of 807 µs against
840 µs.

The conditions include one service core, the trace observer and write
stamps enabled, and interrupts unmasked during ordinary steps. Production
images omit that instrumentation, but their step times were not measured
here. Bundle 6's evaluator rounds ratios up from the counter ticks and
compares the first step using the counter's 125/3 ns tick directly.
Its upper distribution bin is also 1,268 permille. The first run's image
used an earlier limit of 155%.

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
22.5 µs byte, and spin ends fall on whole microseconds, so a step changes
in whole microseconds per phase (section "Board result").

Re-spins are reported per step and in the summary and do not gate the run:
the service model gives no re-spin at a 23 µs byte and 18 to 24 per
snapshot at 24 µs. Each re-spin costs a quarter bit time and one poll, about
1.5 µs. Neither run of the changed provider had one. Their count sizes the
counter-tick spin resolution left out below.

## Measurement

Committed checks, added with the implementation:

- `checks/power-domains-service.c`: independent read, write, poll and time
  costs. The board-cost rows charge the measured costs with the present and
  the lighter barrier placement, the poll cost fitted to the second run's
  median with both placements, the poll cost matched to the third run's
  completing-poll spans and to the fourth run's, and the register accesses
  alone with both placements; each asserts its snapshot step. `--board-costs` selects only
  those rows. The slow-bus case remains in the default check. Every
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
5. **Windows added in the eighth prepared set**, three at the park and one
   during the snapshots, each with 256 iterations in eight sub-windows of
   32 and IRQs masked unless stated:
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
     18.11.10.9, printed pages 1505 and 1506; the SDA control enable, bit 0,
     is on page 1506). An XADDR write therefore starts no bus phase, acknowledges no
     flag and drives no line. The window runs only when ADDR bits 7:3 are not
     `11110b`, CNTR reads `0x40`, STAT reads `0xf8` and LCR reads both lines
     high with neither forced (`bb_xaddr_admitted()` in
     `apps/board-bundles-power/trace.c`). Afterwards XADDR, ADDR, CNTR, STAT
     and LCR must each read the value read before the window, or the image
     fails without a controlled outcome. A refused precondition skips the window and is recorded.
   - **Service loop between polls:** the work query, the time read and the
     work query with the spin time, as `pmic()` in
     `drivers/power/a733_power_domains.c` runs them.
   - **Whole provider poll with the flag clear:** up to four of the last
     four client snapshots carry this window; it stops at 256 samples,
     normally after the first of them. The service polls a queued snapshot back to
     back, without its spin, and stamps each `a733_pmic_poll()`. A poll that
     read only CNTR in a flag phase, wrote nothing, completed no phase and
     returned PENDING is a whole poll with the flag clear: entry, the provider
     object's try-lock, the time read and the flag read. IRQs stay unmasked as
     in the service, and a sample with a scheduler tick inside is left out and
     counted. 256 samples are grouped into eight sub-windows of 32; 256 samples
     of the stamp pair alone give the per-sample overhead. Those snapshots
     leave the step records. Of the 34 snapshots (two supply verifications and
     32 client snapshots), each normally one service step, the wire-time spin
     step is excluded and the poll window takes one, so the summary normally
     reads `STEPS recorded=32` and `excluded=1`; each further snapshot the
     window takes lowers the recorded count by one.
6. **Completing polls in the recorded steps**, a diagnostic build option.
   `checks/board-bundles-apps.py --complete-window` compiles bundle 6 with
   `BB6_COMPLETE_WINDOW=1`; the default image omits the window, and its
   `TWI_COMPLETE` lines print `window=0` with empty records. The window's
   stamp lengthened each snapshot step by about 12 µs in the third and
   fourth runs (section "Board result"). With the window, the bundle's
   wrapper of the poll's task check stamps the counter at the entry of
   every `a733_pmic_poll()`; that is one counter read per poll, inside the
   step times. A poll that completes a bus
   phase (a flag or a STOP wait, as the trace observer counts them) ends its
   span at the stamp after its first phase-start `CNTR` write returns. The
   spans of a recorded step join five records by that write: a byte whose
   poll wrote `DATA` first (`0x48` for an address or register index), a
   receive (`0x48` with no `DATA` write in the poll), a repeated START, a
   START after a STOP wait, and a STOP (`0x58`). Each record keeps the count, the summed ticks and the fastest
   and slowest span, printed as `TWI_COMPLETE` lines. The step that holds the
   wire-time spin and the poll window's snapshot are left out.

The step records count re-spins: control-register polls beyond the second
of a flag phase or of a STOP wait. A STOP wait lasts until `M_STP` reads
clear and the status register reads `0xf8`.

After the change, bundle 6 makes 32 snapshots of DCDC2 and DCDC4. Pass: the
median and first steps within their bounds set as above; a measured byte
within 110% of nine bit times; snapshot bytes unchanged (`0xff9e`); no
fault. Re-spins are reported. A DCDC3 selector write needs a separate
bundle with a PMIC write. The user approved it on 2026-10-24, and it runs
only on a board where ranks 7 and 8 have passed and the B1 record of
[cpu-a76-dvfs.md](cpu-a76-dvfs.md) exists, with an approval record bound to
that board, image and session ([board-bundles-next.md](board-bundles-next.md),
section "Bundles that wait"). The B1 record's DCDC3 reading gives the
selector that the write must find. Acceptance allows two steps and one tick
wait.

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

**Qualification of the barrier placement.** Bundle 6 built from this tree
links the adapter with the placement of item 6 under "Change", and its
windows time that placement directly. Its run on port 3-3 met bundle 6's
pass conditions with the same 1,268-permille median limit and first-step
allowance, and its snapshots, `XADDR` window and bus-idle reads match the
run with `DSB SY` pairs (section "Board result"). That qualifies the
placement for snapshots. The native USB image of rank 9 needs a rerun on
this adapter before it relies on it, and a DCDC3 selector write, under its
own gate, covers the write of the selector byte.

## Board result

Three runs used port 3-3 on 2026-10-24 and the read-only bundle 6 built from
the changed provider, each with a `DSB SY` pair around every S_TWI0 access. The service ran on core 0, S_TWI0 at 400 kHz (divisor
`0x28`, bit time 2,500 ns), with the trace observer and write stamps active.
CPU-L ran at its boot rate of 494 MHz; bundle 4 measured 493,989,502 Hz in
the first run's lifetime, and the second and third runs' lifetime did not
measure it. Raw logs:

- first run, step 3 of the fifth prepared set:
  `~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/set5-bb6-run/kernel.log`;
- second run, with the windows added in the eighth set:
  `~/.cache/sunstoneos/evidence/board-session-20261024-8/port-3-3/s7-bb6-run/kernel.log`,
  from the frozen bundle
  `~/.cache/sunstoneos/evidence/board-bundles-20261024-8/port-3-3/pmic-power-domain-reads/`;
- third run, with the completing-poll window, in the same lifetime:
  `~/.cache/sunstoneos/evidence/board-session-20261024-8/port-3-3/s10-bb6-dsb-run/kernel.log`,
  from the frozen bundle
  `~/.cache/sunstoneos/evidence/board-bundles-20261024-10/port-3-3/step-1-dsb/pmic-power-domain-reads/`.

**Steps.** Each recorded step completed one snapshot: 12 conditions,
24 bytes and 6 STOPs, 630 µs of wire time.

| Metric | First run | Second run | Third run, with the completing-poll window |
| --- | --- | --- | --- |
| Steps recorded | 33; the wire-time spin step excluded | 32; the wire-time spin step excluded, one snapshot taken by the poll window | 32; as the second run |
| Median step | 779 µs (18,703 counter ticks), 123.7% | 784 µs (18,815.5 ticks), 124.4%; 1,243 permille of the measured wire time | 796.4 µs (19,114.5 ticks), 126.4%; 1,265 permille |
| Steps after the first | 777 to 788 µs | 779 to 793 µs | 795 to 805 µs |
| Steps above 1,268 permille | Not evaluated | 1 of 32 | 2 of 32 |
| First step | 809 µs, 128.4% | 807 µs (19,376 ticks), limit 840 µs | 823.6 µs, limit 838.8 µs |
| Re-spins | 0 | 0 | 0 |
| Steps containing an interrupt | 24 of 33 | 24 of 32 | 24 of 32 |
| Byte | 22.5 µs | 22.541 µs | 22.5 µs |
| Snapshots | All `0xff9e`; no fault | All `0xff9e`; no fault | All `0xff9e`; no fault |
| Image's own verdict | Pass against 155% | Pass against 1,268 permille | Pass against 1,268 permille |

**Access costs.** Each window ran 256 iterations in eight sub-windows of 32,
timed by the 24 MHz counter (41.67 ns per tick), with IRQs masked except in
the task-check and poll windows; neither held a scheduler tick. Values are
the second run's, with the first run's in parentheses.

| Window | Average per iteration | Sub-windows, per iteration |
| --- | --- | --- |
| `CNTR` read with `DSB SY` before and after | 418.5 ns (418.8 ns) | 416.7 to 420.6 ns |
| Plain `CNTR` read, value discarded | 377.9 ns (377.9 ns) | 376.3 to 378.9 ns |
| Plain `CNTR` read, address-dependent | 379.1 ns (378.7 ns) | 377.6 to 380.2 ns |
| Adapter time read | 144.4 ns (143.6 ns) | 141.9 to 151.0 ns |
| Adapter register read | 462.2 ns (462.4 ns) | 460.9 to 470.1 ns |
| Task check | 77.0 ns (76.5 ns) | 75.5 to 83.3 ns |
| Trace observer | 42.5 ns (43.5 ns) | 40.4 to 52.1 ns |
| Plain `XADDR` write, one `DSB SY` per 32 | 200.7 ns; 197.6 ns less the empty window | 186.2 to 204.4 ns |
| Empty window, one `DSB SY` per 32 | 3.1 ns; 99 ns per sub-window | 2 to 5 ticks per sub-window |
| Service loop between polls | 274.6 ns | 272.1 to 286.5 ns |
| Whole provider poll with the flag clear | 1,245.4 ns; 1,175.8 ns less its stamp pair | 1,225.3 to 1,263.0 ns |
| Stamp pair of a poll sample | 69.7 ns | 69.0 to 70.3 ns |

The 2,040 S_TWI0 writes inside the snapshots took 17,618 ticks, 359.9 ns on
average (358.1 ns), from 8 ticks (333 ns) to 10 ticks (417 ns). The poll
window took its 256 samples from the 398 polls of one snapshot. XADDR, ADDR,
CNTR, STAT and LCR read the same values after the write window as before
it: `XADDR` `0x0`, `CNTR` `0x40`, `STAT` `0xf8`, `LCR` `0x3a`. The
wire-time spin read `CNTR` 40 times in 554 ticks, 577 ns per iteration. Each
iteration is an adapter read without the observer (462 ns at idle), a raw
counter stamp (about 35 ns, half the stamp pair) and the spin's own record
keeping. The window does not separate the record keeping, so it bounds a
`CNTR` read while a byte is on the wire at about 80 ns above the idle read
and does not show whether it costs more.

The windows divide the costs as follows.

- **Reads.** The `DSB SY` pair adds 41 ns to a read. Issue throughput and
  latency agree, so a plain read waits for its completion either way.
- **Writes.** A plain write costs 198 ns when 32 run back to back before one
  `DSB SY`. The adapter's write with a `DSB SY` pair costs about 325 ns in
  the transfers: the 359.9 ns bracket less one counter read, half the stamp
  pair. Of the 325 ns, about 44 ns are the adapter's checks (its read less
  the fenced read) and about 20 ns the leading `DSB SY`, which leaves about
  261 ns from the store to its completion. One completed write therefore
  takes 198 to 261 ns.
- **Polls.** The whole flag-clear poll, 1,176 ns, holds the task check
  (77 ns), the time read (144 ns), the flag read with its observer call
  (505 ns) and 449 ns of other provider software: entry, the try-lock,
  the deadline and revocation checks, the phase dispatch and the release.
- **Service loop.** Its time read takes 144 ns of the 275 ns, and the two
  work queries the other 131 ns.

**Model at the measured costs.** The board-cost table of
`checks/power-domains-service.c` charges 505 ns per read (adapter read and
observer), 403 ns per write (write bracket and observer), 145 ns per time
read and 527 ns per poll (task check and the 449 ns remainder). It gives a
739 µs snapshot and a 950 µs selector write in one step. The snapshot is
45 µs below the second run's median and 40 µs below the first run's.
Charging the task check alone as the poll cost gives 731 µs, so the whole
poll accounts for 8 µs of the difference. It replaces the 299 ns per-poll
remainder of an earlier fit, which also charged 1,026 ns per write, with a
measured 449 ns. It does not explain the remaining 45 µs.

**Step times move in whole microseconds per phase.** The provider stores
`phase_at` in whole microseconds, the service spins until
`phase_at` plus the rounded-up phase time, and the spin ends at the first
time read at or after that whole microsecond. The next spin starts from the
time read after the next phase-start write. Each phase therefore costs its
spin, 23 µs for a byte and 5 µs for a condition, plus the whole microseconds
spanned by its serial work. That work runs from the end of the spin through
the work query, the poll entry, the flag and status reads and the
phase-start writes to that time read. In the model at the measured costs
that work spans 2 µs in each of the 42 phases. A poll cost of 1,500 ns, about
970 ns per phase more than the measured costs, brings the model to 783 µs,
within 1 µs of the board's median. The board therefore spends about 41 µs
per step on work that the model does not charge. The candidates are:

- the two further phase passes, revocation loads and time-read bookkeeping
  of a completing poll, which a flag-clear poll does once;
- the write wrapper's second counter stamp, atomic exchange and observer
  call, about 70 ns per write outside the bracket;
- `DSB SY` waiting for the provider-state stores that precede each access
  inside a poll, which the isolated windows do not make;
- per-step work outside the phases, such as the admission's two lock
  waits.

**Completing polls, measured.** The third run carries the completing-poll
window (item 6 under "Measurement"). Each span runs from the stamp at a
completing poll's entry to the return of its phase-start `CNTR` write. The
model column charges the measured costs above plus the window's own stamp,
87 ns, by which the whole flag-clear poll grew between the second and third
runs (1,175.8 ns to 1,262.9 ns).

| Kind | Spans | Mean measured span | Model, measured costs and stamp | Measured less model |
| --- | --- | --- | --- | --- |
| Byte (`DATA` then `CNTR`) | 575 | 3.10 µs | 2.72 µs | 0.38 µs |
| Receive (`CNTR` alone) | 192 | 2.52 µs | 2.32 µs | 0.20 µs |
| Repeated START | 192 | 2.49 µs | 2.32 µs | 0.17 µs |
| START after a STOP wait | 160 | 2.56 µs | 2.32 µs | 0.25 µs |
| STOP, after the `DATA` read | 192 | 3.29 µs | 2.97 µs | 0.32 µs |

The counts are those of 32 recorded steps: 18 byte phases, 6 receives,
6 repeated STARTs, 5 STARTs after a STOP wait and 6 STOPs per snapshot,
less one byte. The first START of a request follows admission and completes
no phase, so it is not a span.

The difference grows with the accesses a poll makes after its first read:
0.17 to 0.25 µs for the polls with two reads and one write, 0.32 µs with a
third read, and 0.38 µs with a second write. Inside a poll each fenced
access therefore costs more than the isolated windows, where no
provider-state stores precede the leading `DSB SY`, and each write also
carries the wrapper's second counter stamp, atomic exchange and observer
call. The flag-clear poll, with one read and no write, holds none of that
difference.

Per step, the spans total 118.2 µs against the model's 106.2 µs with the
stamp. With the poll cost set to 919 ns, so that the model's span total
matches the measured one at this run's 505 ns per read and 395 ns per write,
the model gives 758 µs. That is 38 µs below the run's 796 µs median. The
completing polls therefore account for about 12 µs of the per-step
difference between model and board, and about 38 µs lies outside them. That
remainder is step work other than completing polls: the admission with its
two lock waits, the first START, the final STOP's line-control read and the
publication that releases the I2C mutex. No window times that work yet.

**The window's own cost.** The stamp at every poll entry adds 87 ns to each
phase's serial work. The third run's median, 796.4 µs (1,265 permille), is
12.5 µs above the second run's, and 10 µs above the 786 to 787 µs predicted
with 2 to 3 µs for the window. The 87 ns moves most phases across a whole
microsecond, so the window costs about 12 µs per step although its stamps
spend about 3.6 µs.

**Barrier placement of item 6, measured.** The first three runs used a
`DSB SY` pair around every access. The placement of item 6 of section
"Change" reads S_TWI0 plainly, writes `DATA` plainly and completes the
`CNTR` write with one `DSB SY`. A fourth run, in the third run's lifetime
and with the same completing-poll window, used it. Its raw log is
`~/.cache/sunstoneos/evidence/board-session-20261024-8/port-3-3/s10-bb6-light-run/kernel.log`,
from the frozen bundle
`~/.cache/sunstoneos/evidence/board-bundles-20261024-10/port-3-3/step-2-light/pmic-power-domain-reads/`.

| Metric | Third run, `DSB SY` pairs | Fourth run, item 6 placement |
| --- | --- | --- |
| Median step | 796.4 µs (19,114.5 ticks), 1,265 permille | 794.6 µs (19,070 ticks), 1,262 permille |
| Steps after the first | 794.5 to 804.6 µs | 793.5 to 801.4 µs |
| Steps above 1,268 permille | 2 of 32 | 2 of 32 |
| First step | 823.6 µs, limit 838.8 µs | 819.3 µs, limit 838.8 µs |
| Adapter read window | 461.1 ns | 421.4 ns |
| Write bracket in the transfers | 351.2 ns | 277.7 ns |
| Snapshots; `TWI_WRITE` lines | All `0xff9e`; XADDR unchanged | The same values |

| Completing poll | Third run | Fourth run | Saving per poll | Saving per step |
| --- | --- | --- | --- | --- |
| Byte | 3.10 µs | 2.76 µs | 0.33 µs | 6.0 µs |
| Receive | 2.52 µs | 2.43 µs | 0.08 µs | 0.5 µs |
| Repeated START | 2.49 µs | 2.44 µs | 0.05 µs | 0.3 µs |
| START after a STOP wait | 2.56 µs | 2.53 µs | 0.03 µs | 0.2 µs |
| STOP | 3.29 µs | 3.21 µs | 0.08 µs | 0.5 µs |

The fourth run passed bundle 6's conditions, and its snapshots, `TWI_WRITE`
lines and bus-idle reads match the third run's, so the placement is
**qualified for snapshots**. A byte's completing poll, which makes two reads
and two writes, saved 0.33 µs. The isolated windows predicted 0.19 to
0.21 µs for those accesses (41 ns per read, 104 to 124 ns for the write
pair). The 0.12 to 0.15 µs beyond that fits the leading `DSB SY` of each
fenced access waiting for the poll's own provider-state stores, which the
windows do not make. The completing polls saved 7.4 µs per step in all.

The median fell by 1.9 µs (3 permille), against the 8 µs that the model
predicted and the 7.4 µs that the completing polls saved. Each phase costs
its spin plus the whole microseconds that its serial work spans. With
`DSB SY` pairs, a byte phase's serial work is about 3.3 µs: its 3.10 µs
completing poll and about 0.2 µs before the poll and after its write. With
the item 6 placement it is about 3.0 µs, still at or just above 3 µs in most
byte phases, so few phases lost a whole microsecond. The model, with its
poll cost matched to each run's span total, gives 758 µs and 748 µs, a
10 µs difference, because its phases sit at other fractions of a
microsecond.

For the 110% route this means that a per-phase saving shortens the step
only in whole microseconds per phase. Another 0.1 to 0.2 µs per byte phase
would lower the 18 byte phases of a snapshot by 1 µs each, about 18 µs, and
a saving that leaves a phase above its boundary gives nothing. Reaching
693 µs from the fourth run's 795 µs, or about 783 µs without the window,
needs about 90 µs. That is about 2 µs from each of the 42 phases, beyond
what the register accesses leave, or the per-transaction turnaround of the
S_TWI0 packet engine, together with the about 38 µs of step work outside the
completing polls. The barrier placement does not change that route.

**Bound selection.** The first run's median is 18,703 counter ticks,
779.292 µs, or 123.7% of wire time. Its image printed 1,236 permille because
its evaluator rounded down; the revised evaluator prints 1,237. Adding
20 µs and rounding down to a whole permille gives the chosen 1,268-permille
limit (798.840 µs at nominal wire time), about 19.55 µs above that median.
The first-step allowance adds 40 µs. The model is not used to claim an
independently measured margin. Reuse this provisional limit only under the
stated bundle conditions; changed clock rates, instrumentation or bus
timing require a new assessment. The second run's median, 784 µs, is 16 µs
below the limit at its measured byte, 800 µs. The third run, with the
completing-poll window, passed at 1,265 permille, 3 permille (about 2 µs)
below the limit, and 2 of its 32 steps exceeded 1,268 permille; the second
run had 1.

**The completing-poll window in later runs.** With the window present, the
median lies within 3 to 6 permille of the limit. The default bundle 6 image
therefore omits the window, and `checks/board-bundles-apps.py
--complete-window` builds it in for diagnosis. The limit stays at 1,268
permille: it describes the provider under bundle 6's standing
instrumentation, and raising it to absorb a diagnostic window would loosen
it for every later run without a change in the provider.

The windows give no authority for a PMIC rail write.

## Left out

- **A Device-nGnRE mapping of S_TWI0**, which lets the interconnect
  acknowledge a write before it reaches the controller. No window measures
  it, and it needs the board qualification of
  [shared-helpers.md](../shared-helpers.md).
- **The S_TWI0 packet engine.** It would sequence a transaction's
  conditions and bytes in hardware, the one route to 110% that section
  "Budgets" identifies. Its registers, interrupt and failure handling have
  no design or board evidence here.
- **Reading status alone to detect completion.** It would remove one serial
  read per phase, up to about 16 µs per snapshot. The manual defines `0xF8` with `INT_FLAG` clear but does
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
helper and bundle preparation row use 1,268 permille. The eighth set's
bundle 6 image applies it and passed on port 3-3. The fifth and sixth sets'
preserved images and runbooks retain their original 1,550-permille
acceptance limit.
