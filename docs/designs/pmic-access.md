# PMIC register access and service spin timing

Status: **implemented and host-tested; qualified on the board for snapshots
with an earlier barrier placement.** The current barrier placement (a `DSB SY`
after every S_TWI0 `DATA` and `CNTR` store) has host evidence only. A DCDC3
selector write through this provider is open.

The PMIC provider (`drivers/power/a733_pmic.c`) reads the AXP8191 over the
always-on I2C controller S_TWI0. Its worker in five images is the power
service task (`drivers/power/a733_power_domains.c`), which spins on the
counter while a bus phase is on the wire. A DCDC2 or DCDC4 snapshot is 252 bit
times at 400 kHz, 630 µs of wire time. The service completes a snapshot step
in 779 to 796 µs on the board, 124% to 126% of wire time, where the provider
before this design took 917 µs (146%). Run records are in
[pmic-access.jsonl](../evidence/pmic-access.jsonl).

## Design

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
   admission and startup check the deadline once after their read sequences.
   The native adapter divides the counter by its ticks per microsecond with a
   multiply-high by a reciprocal that its first time read computes. This is
   item 5 of section 2 of
   [performance-architecture.md](../performance-architecture.md).
2. **Task check once per poll.** The native adapter's `owned()`, `read32()`
   and `write32()` check the `holding` flag, one acquire load of the I2C
   lock's retained bit and the address allowlist. The task check
   (`a733_task_context_owner_el1`) runs at the entry of every
   `a733_pmic_poll()` and in the adapter's `enter()`, which also asserts the
   kernel mutex holder (section 1.1 of the same document).
3. **STOP wait for the idle status.** After a STOP write, the provider reads
   the control register (`CNTR`, `+0x0c`). Bit 4 (`M_STP`) still set is
   PENDING. Once it is clear, the low byte must be `0x40`, else the provider
   faults. It then reads the status register (`+0x10`); a value other than
   `0xf8` is PENDING. Both waits are bounded by the request deadline.
4. **Line-control read at the ends of a request.** The line-control idle
   check (`+0x20`) runs at admission and after the request's final STOP. A
   mismatch after the final STOP faults. A line that another master holds
   after an intermediate STOP stops the next START, whose wait the request
   deadline bounds.
5. **Spin from the phase-start write.** The service spins until
   `max(phase_at + phase duration, now + bit time / 4)`, in whole
   microseconds, rounded up, where `now` is read after the poll returns.
   `a733_pmic_work()` computes that time from `now`, so the service reads no
   private provider field. The first term times the wire from the write that
   started it, so the provider's work after that write overlaps the bus. The
   second term is the minimum re-spin after a poll that finds the flag still
   clear; it keeps polls at least a quarter bit time apart. The backstop on
   polls per step, `PMIC_POLLS_PER_STEP`, is 256, because with a slow bus a
   selector write needs about 56 polls plus re-spins.
6. **Barrier placement.** The native adapter reads S_TWI0 plainly and
   completes each `DATA` and `CNTR` store with `DSB SY`
   (`a733_mmio_write32_dsb_sy_after`). DATA therefore completes before the
   dependent CNTR trigger, and CNTR completes before `phase_at` and
   bus-status polling. `INT_FLAG`, the expected status and STOP/idle
   establish the bus transaction's completion. Reads of R_CCU, CCU, GIC and
   R_PIO at admission and startup keep `DSB SY` on both sides.
   `checks/pmic-tree.py` keeps the placement. No host check executes the
   native adapter with independently posted stores and bus latency, so the
   DATA-before-CNTR ordering is not covered by a host check.

`phase_at` is a microsecond time truncated from the counter and read after
the write completes, so a spin ends within about ±1 µs of the flag. Polls
that find the flag clear and re-spin are therefore expected.

## Safety properties

- **Ownership.** The provider object's try-lock, the poll's task check and
  the I2C mutex taken by `enter()` on the provider task protect the bus.
  Only `a733_pmic_poll()` reaches `rd()` and `wr()`, and it holds the
  provider object and has checked the task at entry, so a per-access task
  check adds no case. `checks/pmic-tree.py` keeps both facts.
- **Revocation.** One acquire load of the power-authority state word at the
  entry of each poll and before each further phase, including the publish
  phase, and the lock's retained bit on every access. The publish phase
  checks the deadline and not the state word.
- **Bounded failure.** Inside `transfer()`, a deadline that passes during a
  phase is detected after at most that phase's accesses, three at most (an
  address phase writes `DATA` and `CNTR`; the final STOP wait reads three
  registers). Admission runs up to eight reads, and startup about 24, before
  their one check. A deadline that passes during admission retains the
  request in the admitting poll, before any transfer write. A `CNTR` write,
  including the one that sends the selector byte of a DCDC3 or DCDC5 write,
  can start after the deadline has passed within its phase. A fault retains
  the request, the held mutex and the leases, and the programmed voltage of a
  selector write is then uncertain, as after any fault that follows the data
  byte. `checks/a733-pmic.c` sweeps the deadline across every access of a
  snapshot and asserts both bounds.
- **The 1 ms spin limit.** `pmic()` returns one tick after
  `A733_POWER_PMIC_SPIN_US` of spinning in a step, and `SPIN_READS` bounds
  each spin if the clock stops. The service checks the spin budget between
  polls, so a final poll can finish slightly later than 1 ms.
- **Bus protocol.** Each phase reads the flag, then the status, and checks
  the status code. The STOP wait accepts a slow return to idle as PENDING, so
  a status that reaches `0xf8` one access late does not latch a permanent
  fault.

S_TWI0's interrupt (GIC SPI 203) is unqualified, and a 400 kHz byte is
22.5 µs, far below a tick, so the service spins. The provider object,
`phase_at` and the adapter state are in kernel `.bss` (Normal write-back);
S_TWI0 is Device-nGnRnE. `phase_at` is written and read only by the provider
task. Clients on other cores see BUSY only inside a poll, and the model's
bound of one BUSY result per caller holds.

## Register sequences and sources

The A733 User Manual V1.00 describes S_TWI0's control register in section
18.11.10.4 and its status register in 18.11.10.5, with the same text as
TWI0's (18.11.9). `M_STA` clears automatically after a START is sent,
`M_STP` clears automatically after the STOP, `INT_FLAG` is write-one-to-clear,
and the idle state `0xF8` is the only state that does not set `INT_FLAG`.
`CNTR` reads `0x40`, `BUS_EN` alone, once `M_STP` clears, on two premises: the
STOP write `0x58` cleared `INT_FLAG`, and `M_STA` cleared after the START.
`A_ACK` and `INT_EN` are never written. Only an error state can set
`INT_FLAG` after a STOP, so any other value faults. Section 18.11.10.5 lists
`0xF8` as "no relevant status information, INT_FLAG=0".

The pinned bootloader `b791be842935b27268ae3d00e943a9075495f30a`,
`drivers/i2c/sunxi_i2c.c`, waits after STOP for status `0xf8` alone, with a
timeout (`twi_stop()`, lines 469–483); the provider adds the `M_STP` wait
before it. The bootloader reads line control only before a clock change (line
497) and in its bus-recovery pulses (lines 263–297), never after a STOP. The
provider checks line control at admission and after the final STOP, so each
request starts and ends with both lines released.

## Qualified figures

Percentages are of the bit-time wire time: 630 µs for a snapshot and
812.5 µs for a selector write (325 bit times). The model's bytes take 23 µs,
so its own snapshot wire time is 642 µs.

| Figure | Value | Evidence |
| --- | --- | --- |
| Snapshot step, board, median; first step | 779 to 796 µs (124% to 126%); 807 to 824 µs | `pmic-access-bb6-run1` to `-run4` |
| Provider before this design, board | 917 µs median (146%), first step 941 µs | `pmic-access-bb6-base-provider` |
| Time reads per snapshot; per selector write | 137; 174 | `pmic-access-host-budgets` |
| Register accesses per snapshot; per selector write | 200; 255 | `pmic-access-host-budgets` |
| Modeled snapshot step at 200 ns per access | 685 µs (109% of 642 µs) | `pmic-access-host-budgets` |
| Modeled snapshot step at the measured access costs | 739 µs | `pmic-access-model-at-measured-costs` |
| Provisional acceptance bound | median at most 1,268 permille of measured wire time; first step plus 40 µs | `pmic-access-bound-selection` |
| Access costs on the board | read with `DSB SY` pair 418.5 ns; adapter time read 144.4 ns; whole flag-clear poll 1,176 ns | `pmic-access-bb6-run2` |
| Rank 9 native USB image on this provider | passed, four supply snapshots | `pmic-access-requalification` |

The 1,268-permille limit is an empirical acceptance limit set from the first
board run; no model or cost measurement derives it, and it is not a
worst-case bound. It applies under bundle 6's instrumentation (one service
core, trace observer and write stamps, CPU-L at 494 MHz), and a changed
clock, instrumentation or bus timing needs a new assessment. The power-domain
design and runtime documents use it. Production images omit that
instrumentation, and their step times are not measured.

110% of wire time (693 µs) is not reachable by the register accesses of this
design: with every software cost set to zero the model gives 712 µs (113%)
with `DSB SY` pairs and 694 µs with plain `DATA` stores. This is because the rounded
wire spin already takes 642 µs and 42 phases share the remaining 51 µs. A
route to 110% needs fewer serial register accesses per snapshot.

## Checks and instrumentation

- `checks/power-domains-service.c`: independent read, write, poll and time
  costs, with board-cost rows (`--board-costs`) that assert modeled snapshot
  steps. The 400 kHz case at 200 ns per access asserts 110% of the model's own
  wire time.
- `checks/a733-pmic.c`: flags set from the write's completion time; late or
  absent `0xf8` after STOP; a control value other than `0x40`; a line-control
  value per STOP; a count of accesses that start at or after a deadline. Each
  matching provider mutant fails it.
- `checks/a733-pmic-time.c`: the reciprocal division against integer division
  for every divisor up to 4,096 and the powers of two to 2^32.
- `checks/pmic-tree.py`: the adapter boundary. Only the provider, native
  adapter and bundle power layer use the adapter's operations; consumers use
  `a733_pmic_native_bind()`, `a733_pmic_now()` and `a733_pmic_outside()`.
- Bundle 6 (`apps/board-bundles-power/bundle6.c`, `power.c`, `trace.c`) makes
  32 snapshots of DCDC2 and DCDC4 with access-cost windows, write stamps, a
  wire-time spin and re-spin records, and applies the 1,268-permille limit.
  `checks/board-bundles-apps.py --complete-window` adds a diagnostic
  completing-poll window that lengthens each step by about 12 µs; the default
  image omits it. The method is in record `pmic-access-bundle6-method`.
- `apps/usb-returning/build.py --base-monitor` links the base image's monitor
  object into the rank 9 image; see `pmic-access-requalification`.

## Limitations and open work

1. Run a DCDC3 selector write on the board; acceptance is at most two steps
   with one tick wait. It needs a bundle with a PMIC write, ranks 7 and 8
   passed, the B1 record of [cpu-a76-dvfs.md](cpu-a76-dvfs.md) and an approval
   record bound to the board, image and session ([board-bundles-next.md](board-bundles-next.md),
   section "Bundles that wait").
2. Rerun the rank 9 native USB image on the current adapter placement.
   Bundle 6 qualified the plain-`DATA` placement and the `DSB SY` pair
   placement for snapshots, not the current one.
3. About 38 µs of each board step is work outside the completing polls (the
   admission's two lock waits, the first START, the final STOP's line-control
   read, publication). No window times it.
4. Not designed and not qualified:
   - A Device-nGnRE mapping of S_TWI0, which lets the interconnect
     acknowledge a write before it reaches the controller; it needs the
     board qualification of [shared-helpers.md](../shared-helpers.md).
   - The S_TWI0 packet engine (`TWI_DRV` registers from `+0x200`, which
     startup requires disabled), the one route to 110%. Its registers,
     interrupt and failure handling have no design or board evidence.
   - Reading the status register alone to detect completion, which removes
     one serial read per phase (up to about 16 µs per snapshot). The manual
     does not state the status value while a phase is on the wire, and the
     bootloader reads the flag first.
   - Counter-tick spin resolution; the quarter-bit re-spin covers a spin that
     ends before the flag.
5. The bundle 7 freeze (`apps/board-bundles-power/bundle7.c`) runs on the
   provider as frozen or is refrozen after a passing bundle 6. The
   returning ADC image links the provider and is not board-qualified; the
   GPU, accelerator, GPU display, HDMI, DisplayPort and G2D board images and
   the integrated OS image run the service and are not board-qualified.
