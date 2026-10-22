# Design: one owner for supply rails and one for power domains

Status: **design**, not implemented; physical behavior stays unqualified
until the board evidence at the end exists. Section numbers refer to
`docs/performance-architecture.md`.

Today the clock registry holds AXP8191 output DCDC2, the GPU resource manager
holds DCDC4, and the rail ledger reaches both through bridges. The PCK600
power-domain sequence exists in the GPU, DE_SYS, NPU, HDMI and DisplayPort
code, and the GPU, NPU, HDMI and DisplayPort each take their own PMIC
snapshot of the same supplies.

## The rail ledger owns fifteen rails and writes two

The ledger keeps a fixed table of the fifteen outputs that production code
acquires and the PMIC provider decodes. DC1SW2 stays an alias of DCDC1.
Other outputs are refused.

| Rails | Consumers | Ledger state |
| --- | --- | --- |
| DCDC3 (CPU-B), DCDC5 (CPU-L, DSU) | CPU clock driver | Holder records, floors, status entry, raise and lower |
| DCDC2, DCDC4, CLDO2, CLDO5 | Domain owner (below) | Holder records only |
| DCDC1, DCDC8, DCDC9, BLDO1, BLDO2, BLDO5, CLDO1, CLDO3, ELDO6 | USB, Type-C, MMC, camera, radio, PWM, PCIe, SPI, ADC, monitor, thermal | Holder records only |

A **holder** record stores a token identifier, owner, rail mask and floors
for DCDC3 and DCDC5. `a733_rail_acquire` keeps its signature, token layout
and PENDING result, so the 26 production call sites outside this package do
not change; its exclusive mask must be zero.

Only DCDC3 and DCDC5 have a **status entry**: state, published voltage,
baseline, highest floor, desired target, the one write in flight and its
settle deadline. When the holder count of either goes from zero to one, the
ledger submits an ordinary provider snapshot through `a733_pmic_submit` and
keeps the result, so the ledger adds no second read path. The first such
snapshot records the **baseline**. Status for a holder is READY when, for
each of its written rails, the output is enabled, the selector decodes,
every floor is at or below the published voltage, no write is in flight and
the settle deadline has passed. Otherwise it is PENDING. A disabled or
undecodable output is refused, because nothing in this design enables a rail.

- **One write per rail.** The desired target is the larger of the baseline
  and the highest floor. A write latches its target when it is queued. A
  floor change during a write updates only the desired target. The PMIC task
  publishes the voltage decoded from the readback, then queues a follow-up
  write if the desired target differs. Status compares floors only with the
  published voltage. A raise to 870 mV that arrives during a lowering from
  870 to 800 mV therefore stays PENDING until the follow-up write publishes
  870 mV, and never reads READY at 800 mV.
- **Settle deadline.** A write sets it to the later of the stored deadline
  and its STOP time plus the ramp from the lowest possible voltage at
  250 µV/µs, so a lowering cannot shorten a raise still ramping. The first
  snapshot sets it no earlier than provider start plus 5,160 µs, the
  full-range ramp and enable delay in the board device tree. Deadlines are
  stored in counter ticks.
- **Release during a write** succeeds; the in-flight write keeps its own
  authority until the PMIC task publishes.

## The DE_SYS provider becomes the domain owner

`drivers/power/a733_de_sys_power.c` already powers domains 1, 9 and 10 for
G2D and the display framebuffer. It becomes the domain owner for domains 1,
4, 5 and 10, renamed `a733_power_domains.c` with header
`include/a733_power_domains.h`. Its per-consumer ops, its `authorize`
callback and its domain-9 path, which has no production caller, are removed.
`checks/power-domains-pck.c` is extended to the new domains. Per domain the
owner keeps a count, a state, the pinned PCK600 delay row and the local
clock and reset words that must read off before power-down. Per consumer row
it keeps a count and a ledger token.

| Domain | Consumer row | Required supply readback |
| --- | --- | --- |
| 1 (DE_SYS) | Display framebuffer; G2D; deinterlace | DCDC2 = 800 mV (supply assumed) |
| 4 (NPU) | NPU startup | DCDC2 = 800 mV |
| 5 (GPU_TOP) | GPU | DCDC4 = 800 mV |
| 10 (VO1) | HDMI | DCDC2 = 800 mV, CLDO2 = 1,800 mV |
| 10 (VO1) | DisplayPort | DCDC2 = 800 mV, CLDO5 = 1,800 mV |

The values are the ones these consumers check today. When a row's count
goes from zero to one, the owner acquires the row's rails and takes one
provider snapshot per rail, which must show both pages 0, the output
enabled, the selector decoded and the voltage in the table. The GPU, NPU,
HDMI and DisplayPort drop their own snapshots. A row whose count is already
above zero relies on the earlier snapshot, because nothing in the OS writes
these four rails; firmware and other S_TWI0 masters are covered only by
board evidence.

**NPU supply.** The NPU runs only at 492 MHz, and the pinned vendor
operating-point table gives 800 mV at 492 MHz for all 26 speed-bin entries.
`checks/npu-platform-reference.py` already asserts this. Domain 4 therefore
uses the fixed DCDC2 value, with no eFuse decode or table in the image.

The GPU resource manager keeps the PCK600 bus-clock gate (`RCCU + 0x1ac`),
because PCIe, video, ISP and USB2 lease it for other domains; the owner holds
one shared lease while any count is above zero. The DSI path keeps its own
lease, because it only reads domains 0, 1 and 9.

- **Power-up:** rails acquired and snapshots checked, PCK lease held, domain
  on, then the consumer ungates its clocks and resets.
- **Power-down:** each consumer stops its bus masters and drains their DMA
  before it gates its clocks and calls release. When the domain count reaches
  zero, the owner reads the local clock and reset words and refuses
  power-down if any is not off. It then turns the domain off and releases
  the PCK lease and rails. A domain found on at boot gets a count of one and
  is never turned off, because inherited traffic has no drain evidence.

## The PMIC task performs every snapshot and write

Every enqueue into a provider slot, from `a733_pmic_submit` or from the
ledger, calls the adapter's `kick`. In the board and OS images, `kick`
notifies the PMIC task on index 0, which it uses for nothing else. The loops
in `apps/board/main.c` and `apps/mimus/native.c` block on the notification,
bounded by 250 ms, only when the poll reports stage READY, no active
transaction and no queued slot. They yield while a transaction is held and
wait one tick on BUSY or during startup, as today. A submit that races the
idle check leaves a pending notification, so the take returns at once. Other
images set `kick` to NULL and keep their one-tick loop.

The board image pins `board-pmic` to the A55 cores 0–5 with
`vTaskCoreAffinitySet` and lowers it to priority 3, below `board-usb0` (4)
and `board-typec` and `board-power` (5). USB0 therefore preempts a spin at
once and never waits for one.

The provider reads chip identity (`0x0e`, `0x0f`) once, in its verify stage,
and refuses READY unless they read 3 and 0. A snapshot is then four
transactions (page `0xff`, enable, selector, page) and carries the admitted
chip and revision, so existing chip checks still pass. A write is six: the
same four, the selector write and a readback. The enable bit must be set,
the selector must equal the ledger's published value, both pages must read 0
and the readback must match. Every transaction stays a single-register
sequence ending in NACK, so control values and the native allow-list do not
change. A fault sets the provider's failed state, marks DCDC3 and DCDC5
UNCERTAIN and returns RETAINED to their holders.

## Waiting uses bounded counter spins at 90 kHz and faster

- **Time source.** The provider, the ledger status and the domain owner read
  `CNTPCT_EL0` directly (ISB, MRS) and keep their own monotonic check with
  `a733_monotonic_advance`. None takes the counter-clock lock. The native
  adapter's init check changes from `locks->now == a733_counter_clock_now`
  to a once-only check that `CNTFRQ_EL0` is 24 MHz. The adapter checks
  `owned()` and the mutex holder once per transaction, when it takes the
  mutex; per access it keeps only the address allow-list and control values.
- **S_TWI0 phases.** At baseline the provider computes the bit time from the
  admitted divisor. If a byte takes at most 100 µs (a rate of at least
  90 kHz), each wait phase spins. It reads only the counter until the
  expected completion (9 bit times for a byte, 1 for START or STOP), then
  reads the control register, about two bus reads per phase. Below 90 kHz
  each wait phase waits one tick, as today. The task holds the provider
  object for one transaction and yields between transactions.
- **Preemption.** If 2 ms pass from the phase start, the provider re-reads
  control and status once. It faults only on a bus error. A clear flag
  returns PENDING, and the request deadline alone decides expiry, as today,
  so a preemption longer than 2 ms never becomes a fault.
- **No TWI interrupt.** Byte interrupts would cost 34 per write, a route in
  every image and GIC masking, while settle on the same path already waits
  up to 1,000 µs. Interrupt 235 stays masked.
- **Rail settle.** Status compares the stored deadline with the counter. The
  CPU clock driver has no production caller, so no waiter is added.
- **PCK600 acknowledgement.** No transition interrupt is documented and the
  vendor driver polls. The owner spins up to 50 µs with no lock held and
  interrupts enabled, then returns PENDING; the consumer retries once per
  tick up to 10 ms.

## Memory placement and cache policy

Both objects live in kernel `.bss` (Normal Write-Back, Inner Shareable). No
DMA master reads them, so no cache maintenance applies. Status lines are
separate from mutation lines.

| Ledger region | Size | Written by |
| --- | --- | --- |
| Lock word and free bitmap; fault word | 2 × 64 B | Acquire and release; a fault |
| DCDC3 and DCDC5 status, one line each | 128 B | PMIC task publication |
| DCDC3 and DCDC5 counts, floors, desired target, write slot | 128 B | Acquire, release, floor change |
| Holder slots, 32 B × 40 | 1,280 B | That slot's acquire and release |
| PMIC snapshot borrows, 48 B × 16 | 768 B | Snapshot submit and reap |

The total is 2,432 bytes, down from 20,096. Forty slots cover 36 concurrent
holders (26 acquire sites, seven consumer rows, three CPU floors) plus four
spare. Sixteen borrows cover eight provider slots plus camera and TWI users.
Exhaustion returns BUSY before any change. Registers keep the `a733_mmio.h`
helpers until section 1.2's barriers are qualified.

## Synchronization across cores

Mutations, result publication and domain transitions take one nonblocking
try-lock per object: compare-and-swap with acquire, release store on exit,
BUSY on contention, no spin or nesting. The domain owner drops its lock
before calling the ledger.

Status takes no lock. A token identifier holds the slot index in its low
bits (6 for the ledger's 40 slots, 7 for the clock registry's 128) and a
never-reused generation above them.

- **Writer.** Store an invalid slot word, issue a release fence, write
  owner, mask and floors as relaxed atomics, then release-store the new slot
  word.
- **Reader.** Acquire-load the fault word and the slot word, load the
  fields, issue an acquire fence and reload the slot word. A changed or
  mismatched word returns INVALID. Once index and generation match, status
  is computed from the slot's own mask and floors; the token's other fields
  are not compared.
- **Rail status.** The PMIC task stores a rail's deadline before its state
  word (release); the reader acquire-loads the state first. A reader
  therefore pairs a state with the same or a later deadline, which at worst
  returns PENDING early.

The clock registry adopts this scheme without changing its 48-byte token or
its 73 production status callers. `a733_pmic_live` becomes two acquire
loads.

## Reused and removed mechanisms

- **Reused:** the ledger API and retention, PMIC transactions and snapshots
  (also for the ledger's own reads), the DE_SYS PCK600 code and delay row,
  clock scopes, the PCK gate lease, the CPU grant.
- **Removed:** clock scope `A733_POWER_DCDC2`; the GPU RAIL resource; ledger
  bridges, OPENING and exclusive mode; `a733_pmic_consumer.c`; public
  `a733_pmic_submit_write`; four duplicate PCK sequences; the domain-9 path;
  chip identity reads per request; `owned()` and counter-clock reads per
  register access. The provider's per-phase clock and borrow queries become
  fault-word loads; the provider never releases its S_TWI0 lease and alone
  ends its borrows, so coverage is unchanged.
- **Changed outside this package:** GPU resource and platform files, NPU
  platform, G2D, deinterlace, display framebuffer, `hdmi_native.c` (its
  snapshot check moves to the owner), DisplayPort, the board and OS PMIC
  loops and task creation, and checks that build a ledger. The new header
  `include/a733_power_domains.h` is outside the owned header patterns and
  needs the coordinator's approval. No reserved CPU code changes.

## Performance budgets

Instruction and cache-line counts come from a step added to
`checks/rail-leases.py`, the existing runner for the rail checks. The step
builds the status and acquire functions with clang `-O2 -mcpu=cortex-a55`
and runs them in Unicorn with 42 clock and 16 rail leases, then 126 of each.
No existing component check counts instructions, so the runner gains the
step instead of a new file. Bus, spin and latency figures come from counters
added to `checks/a733-pmic-polls.c`. Contention cases go into
`checks/rail-leases.c` and `checks/clock-counter-contention.c`. A read
transaction takes about 95 µs and a write 70 µs at 400 kHz, four times that
at 100 kHz.

The instruction, line and storage baselines below come from an earlier
measurement that was not committed. The implementation re-measures them on
the parent commit with the extended runner and replaces them before it
reports any target as met.

| Operation | Baseline | Target |
| --- | --- | --- |
| Rail status, one-rail token | 536 / 1,554 instructions; 60 / 233 lines | ≤ 40 instructions including the counter read; ≤ 3 lines; +8 per further written rail |
| Clock token status | 243 / 600 instructions | ≤ 30 instructions, 2 lines |
| Acquire, status and release | 4,026 / 7,773 instructions | ≤ 400 instructions, no bus access |
| Status during mutation | Lock; BUSY under contention | No BUSY or false READY with 4 readers, 1 writer and sanitizers; cases: raise then lowering, lowering then raise, disabled DCDC4 at a high selector, undecodable selector |
| Counter-clock lock acquisitions per PMIC register access | 2 | 0; the contention check counts the counter line |
| Authority check per PMIC transaction | 818 / 2,179 instructions per phase | 2 acquire loads, ≤ 10 instructions |
| Bus reads per wait phase | 1 per poll, repeated after each yield | ≤ 3 at 400 and 100 kHz |
| Ledger storage | 20,096 B | ≤ 2,432 B (static assertion) |
| Snapshot, submit to completion | Up to one tick to start, then bus time | No tick wait; ≤ 0.45 ms at 400 kHz, ≤ 1.6 ms at 100 kHz |
| Startup to PMIC READY | Re-measured on the parent commit | No more ticks than the parent commit |
| CPU-B 800→870 mV, request to readback | About 60 polls, 60 ms (model) | ≤ 0.65 ms at 400 kHz, ≤ 2.4 ms at 100 kHz |
| Same raise, request to READY | None | ≤ 0.85 ms at 400 kHz (280 µs settle); ≤ 2.4 ms at 100 kHz |
| Core time spun per write | Not applicable | ≤ 0.6 ms at 400 kHz, ≤ 2.3 ms at 100 kHz, 0 below 90 kHz; per transaction ≤ 0.1 ms and ≤ 0.4 ms |
| Delay added to a USB0 burst | Not applicable | 0, because USB0 runs at a higher priority |
| Phase spin preempted past 2 ms | Not applicable | PENDING, no fault (model case) |
| Domain power-on (1, 4, 5, 10) | GPU: 27 steps, one per poll | Snapshots, 2 PCK reads, write, readback, status in the 50 µs spin; 10 delay-row accesses once per boot |
| Domain power-down | GPU: 8 steps, one per poll | Local word reads, 2 PCK reads, write, readback, status |
| Power-authority calls per NPU job | 0 | 0 |

## Failure classes (section 1.8)

- **Refused or BUSY before any access:** nothing is retained.
- **Snapshot mismatch at domain power-up** (disabled output, wrong voltage,
  nonzero page): the row's rails are released and the consumer gets an
  error, because a read changes no state.
- **Expiry before a domain write:** that row's ledger records are released.
- **Unknown state:** a bus fault, a readback mismatch or expiry during a
  rail write retains the provider and the DCDC3 and DCDC5 holders. A PCK600
  status other than the requested on or off after the domain write,
  including a deny, or expiry after that write, retains the domain, its
  holders and the PCK lease. The PCK model covers the deny case.

## Not included, with reasons

- **DCDC2 and DCDC4 writes:** no NPU point at 492 MHz needs more than
  800 mV, no grant covers DCDC2 and no GPU operating-point table exists.
- **Lowering below the baseline:** it needs every consumer of each rail.
- **Ledger snapshots for holder-only rails:** their consumers keep their own
  snapshots. Moving them would change 26 files outside this package and
  save no read.
- **A waiter for CPU frequency changes:** no production caller exists.
- **CPU cluster and core domains** (reserved code) and **domains 0, 2, 3
  and 6–9** (current owners stay).
- **The TWI_DRV packet engine** at S_TWI0 + `0x200`: one interrupt per
  transaction, so spun core time falls from about 0.55 ms to 60 µs per write
  at the same bus time. It is needed when a governor raises more than 10
  times per second or a measured phase spin exceeds its budget.

## Board evidence that would confirm the design

- The S_TWI0 divisor of the attached board and the rate it decodes to.
- DCDC3 or DCDC5 readbacks after a raise and a lowering, with
  request-to-readback time and the longest phase spin.
- PMIC page 0, idle S_TWI0, and unchanged DCDC2, DCDC3, DCDC4 and DCDC5
  selectors after PSCI CPU_ON, CPU_OFF and suspend, and a boot-to-idle bus
  capture showing no other S_TWI0 master.
- PCK600 status and time to ON and OFF for domains 1, 4, 5 and 10 across
  repeated cycles.
- PMU cycles for rail and clock status on an A55 and an A76, idle and during
  a mutation on the other cluster.
- A schematic source for the DE_SYS, G2D and deinterlace supply.
