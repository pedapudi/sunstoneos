# Design: one owner for supply rails and one for power domains

Status: **implemented and host-tested** (state word, rail ledger, lock-free
PMIC time reads, CPU-rail floor); **open** for the items under "Open work".
The power-domain owner and the PMIC service task are designed and recorded in
[power-domains.md](power-domains.md). Physical behavior is unqualified until
board evidence exists. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md). Measured
figures and their conditions are in
[power-authority.jsonl](../evidence/power-authority.jsonl).

The AXP8191 outputs have one owner each: the rail ledger
(`drivers/power/a733_rail_leases.c`) for supply tokens, the PMIC provider for
the S_TWI0 bus, and the power-domain owner for PCK600 domains.

## One state word carries readiness and revocation

The power-authority state word (`include/a733_power_authority.h`) is one
64-byte line, embedded in the rail ledger and shared by every object bound to
that ledger. Every bit is set once per boot with `atomic_fetch_or` and never
cleared, so a READY bit cannot overwrite a fault set on another core during
startup. Readers make one acquire load and test a mask; any set fault bit in
the mask refuses the caller.

| Bit | Meaning | Set by |
| --- | --- | --- |
| 0 | PMIC provider READY | provider |
| 1, 2, 3 | Supply verification done; DCDC2 verified; DCDC4 verified | power-domain owner |
| 8, 9, 10 | Provider fault; clock registry fault; rail ledger fault | provider, registry, ledger |
| 11, 12, 13 | Fault of domain 1, 4 and 5 | power-domain owner |

A PMIC transaction depends on the provider, clock and ledger fault bits
(`A733_POWER_PMIC_TRANSFER`).

## The rail ledger owns fifteen rails; the CPU clock driver writes two

The ledger keeps a fixed table of the fifteen outputs that production code
acquires: DCDC1–5, DCDC8, DCDC9, BLDO1, BLDO2, BLDO5, CLDO1, CLDO2, CLDO3,
CLDO5 and ELDO6. DC1SW1 and DC1SW2 are aliases of DCDC1; other outputs are
refused. Exclusive tokens are accepted on all fifteen outputs as pure
exclusion (`checks/a733-radio-platform.c` expects BUSY for an exclusive DCDC8
request).

- **Tokens and pins.** A holder slot stores the token identifier, owner, rail
  masks and one pin count. A counted domain row and a PMIC borrow each raise
  the pin count, and rail release returns BUSY while it is nonzero. A token
  identifier holds its slot index in its low bits and a never-reused
  generation above them, so status compares one slot instead of scanning.
- **Storage.** The ledger is at most 1,632 B (static assertion): 32 holder
  slots of 32 B cover the 31 holders the images create, and 10 borrows of 48 B
  cover eight request slots, the camera's lifetime borrow and one spare.
- **Acquire and poll.** `a733_rail_acquire` returns PENDING and
  `a733_rail_poll` returns READY on its first call with no bus access. The
  poll call remains for its callers.
- **Consumers.** The framebuffer, GPU and NPU acquire their own tokens
  instead of reaching DCDC2 or DCDC4 through the clock registry or the GPU
  manager.

DCDC3 (big cluster) and DCDC5 (little cluster and DSU) accept an exclusive
mask with one holder, the CPU clock driver `drivers/clock/a733_cpu_clock.c`.
The driver records each cluster's boot baseline from its first selector read
and sets the voltage target to the larger of that baseline and the operating
point, through `a733_rail_floor` in the ledger. `a733_pmic_submit_write` with
its expected-selector check is the only writer, so a refused or expired
lowering keeps the higher voltage. The settle wait blocks for whole ticks
until 300 µs before the deadline, then spins on raw counter ticks as the PMIC
phases do.

## PMIC provider and bus waits

- **Time source.** `a733_counter_clock_now` takes the global ticket lock with
  interrupts masked (section 1.7), so the provider never calls it. The native
  adapter's time read returns `ISB; MRS CNTPCT_EL0` divided by the ticks per
  microsecond, the same microseconds as the counter clock, and refuses after a
  fault latched in the counter clock. The provider's `a733_monotonic_advance`
  check faults on a counter that runs backwards. The PCK600 and CPU settle
  spins use the same read. `checks/pmic-tree.py` fails if the time read calls
  the counter clock.
- **Mutex ownership once per bus entry.** The adapter's `enter()` queries the
  kernel mutex holder once and sets a `holding` flag. A FreeRTOS mutex changes
  holder only when its holder gives it, and only the adapter's `leave()`
  gives it on the provider task, so the flag is exact until `leave()`.
- **Revocation inside a transfer.** Before each transaction and before the
  selector-write byte, the provider makes one acquire load of the state word
  (section 1.1). A set fault bit calls `fault()`, which retains the I2C mutex
  with the transaction incomplete, and the task makes no further register
  access. `fault()` only sets a bit, so a fault raised on another core takes
  no lock.
- **`a733_pmic_live` takes no lock.** It compares the caller's lifetime with
  the provider's start time and makes one acquire load, so it never returns
  BUSY.
- **Bus rate.** The provider adopts the bootloader's S_TWI0 divisor and
  admits any rate from 10 to 400 kHz (`route()` in
  `drivers/power/a733_pmic.c`). Admission rereads the clock route and divisor
  (four or five reads), because `checks/a733-clock-reference-pmic.c` requires
  drift detection there.
- **Barriers.** The adapter issues `DSB SY` on its register accesses; the
  placement is described in [pmic-access.md](pmic-access.md).
- **Phase spin.** Phase spin timing, the 1 ms spin limit and the provider's
  bounded-failure rules are in [pmic-access.md](pmic-access.md) and
  [power-domains.md](power-domains.md).
- **One S_TWI0 interrupt owner.** Only the provider names the S_TWI0
  interrupt, GIC-600 INTID 235 (`twi@7083000`, SPI 203 in
  `board-kernel/sun60iw2p1.dtsi`), and `checks/pmic-*` enforces this. The
  interrupt is unqualified, so the service spins.

## Checks

- `checks/rail-leases.c`: acquire, poll and release in production order;
  READY on the first poll for all fifteen rails; a full table returns BUSY.
- `checks/a733-pmic.c`: submit before READY returns BUSY; a fault raised on
  another core during startup stays latched after READY; a registry fault
  during a write faults the provider before the selector-write byte; a fault
  during a spin stops all later TWI access.
- `checks/a733-pmic-polls.c`, `checks/power-domains-pck.c` and
  `checks/power-domains-service.c`: bus, wake and domain behaviour, described
  in [power-domains.md](power-domains.md).
- `checks/cpu-clock-model.c`: the CPU voltage target never falls below the
  baseline.
- `checks/pmic-tree.py`, `checks/rail-leases-cost.py` and
  `checks/a733-pmic-cost.c`: tree rules and cost budgets.

## Qualified figures

All figures are host-modeled; none comes from the board. Rows compare the
merge base with the implementation (record `power-authority-host-measurements`).

| Figure | Value | Budget |
| --- | --- | --- |
| Rail status, one-rail token | 37 instructions, 3 lines (base 222 to 888) | at most 60, 3 lines |
| Clock token status | 45 instructions (base 235 to 592) | at most 50 |
| Acquire, poll and release | 335 instructions (base 1,643 to 3,565) | at most 450 |
| Authority check per PMIC transaction | 6 instructions, one acquire load | at most 6 |
| Ledger storage | 1,600 B (base 20,096 B) | at most 1,632 B |
| Counter-clock lock acquisitions per snapshot | 0 (base 590) | at most 2 |

## Limitations and open work

1. The adapter keeps `DSB SY` before and after each register access; the
   lighter placement is described in [pmic-access.md](pmic-access.md).
   `docs/shared-helpers.md` requires board qualification for a weaker barrier.
2. Setup reads per snapshot are 25 against a target of 21, because admission
   rereads the clock route and divisor.
3. Requesters poll `a733_pmic_result` once per tick; the notification-based
   wake of the original service-task proposal does not exist. It needs a
   second task-notification index and a scheduled-core constant from the
   primary agent.
4. Domain 10 (VO1) has no owner entry: HDMI and DisplayPort power it
   themselves, and CLDO2 and CLDO5 are not verified. The design for these
   rows is [power-domains.md](power-domains.md), section "Left out".
5. HDMI's `validate()` has no state-word check yet: it should load the state
   word and the GPU resource manager's failed latch, and the register
   accessors should drop their per-access try-locks. Likewise `live()` in
   `a733_mmc_platform.c`, `healthy()` in `a733_twi_native.c` and
   `leases_live()` in `a733_camera_native.c` should become one admission
   check plus the state-word load (section 2.4).
6. The TWI packet engine (`TWI_DRV` registers, DMA request 47, one completion
   per transaction on INTID 235) replaces the byte spin when the measured CPU
   voltage-change rate exceeds 10 per second or the engine is qualified on the
   board. It is not designed in detail.
7. The CPU clock driver's core placement mask for its settle spin and a
   ledger-held floor record are preconditions for its production callers.
8. Board evidence: S_TWI0 SCL rate, DCDC3 and DCDC5 readbacks and spins,
   unchanged DCDC2–5 selectors after PSCI CPU_ON with a bus capture showing no
   other S_TWI0 master, PCK600 times, USB0 and audio latency with PMIC
   requests running, PMU cycles for rail and clock status on an A55 and an
   A76, and a schematic source for the DE_SYS, G2D and deinterlace supply
   (record `power-authority-board-evidence-required`).
