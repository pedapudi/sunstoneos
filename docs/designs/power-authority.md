# Design: one owner for supply rails and one for power domains

Status: **design**, not implemented; physical behavior stays unqualified
until the board evidence at the end exists. Section numbers refer to
`docs/performance-architecture.md`.

Today the clock registry holds AXP8191 output DCDC2, the GPU resource manager
holds DCDC4, and the rail ledger reaches both through bridges. Five drivers
repeat the PCK600 power-domain sequence, and six read the same supplies from
the PMIC.

## The rail ledger owns fifteen rails and writes two

The ledger keeps a fixed table of the fifteen outputs that production code
acquires: DCDC1–5, DCDC8, DCDC9, BLDO1, BLDO2, BLDO5, CLDO1, CLDO2, CLDO3,
CLDO5 and ELDO6. DC1SW2 stays an alias of DCDC1; other outputs are refused.
A **holder slot** stores the token identifier, owner, rail masks and one
**pin count**. With no free slot, acquire returns EXHAUSTED.

`a733_rail_acquire` keeps its signature, token layout and PENDING result,
which callers such as `a733_pcie.c` require. For the thirteen rails other
than DCDC3 and DCDC5, the first poll returns READY with no bus access. The
framebuffer, GPU and NPU acquire their own tokens instead of reaching DCDC2
or DCDC4 through the clock registry, the GPU manager or
`a733_pmic_consumer.c`. The NPU startup's direct S_TWI0 read path is
removed; `npu-convolution`, the only image that runs NPU startup, already
binds the PMIC provider.

**DCDC3 and DCDC5 are written** by the CPU clock driver, which holds DCDC5
for the little cluster and the DSU and DCDC3 for the big cluster. Only these
two rails accept an exclusive mask, and each admits one holder. The driver
has no production caller today, so the floor mechanism takes its smallest
locked form:

- **Rail record.** One 32-byte record per rail holds the baseline, the
  published voltage, the floor and the settle deadline. Only the holder's
  `a733_rail_publish` writes it, under the ledger lock; the PMIC task never
  writes the ledger.
- **Baseline.** No earlier than provider start plus 5,160 µs (the
  full-range ramp and enable delay in the device tree), the driver submits
  one snapshot, outside the ledger lock because submit borrows the token
  through that lock. It reaps the result, which ends the borrow, and
  publishes it. A BUSY submit writes no request ID and is retried on the
  driver's next step.
- **Floor change.** The target is the larger of the baseline and the floor.
  The driver submits `a733_pmic_submit_write` with the published selector as
  the expected value, reaps the result and publishes the floor with the
  readback. Publication sets the deadline to STOP time plus |new − old
  voltage| at 250 µV/µs. A refused or expired request changes nothing, so a
  failed lowering keeps the higher floor.
- **UNCERTAIN** is a per-rail mask in the ledger fault word. The provider's
  fault path sets it for DCDC3 and DCDC5 with an atomic OR, and no store
  clears it. Status reports it, and publication is refused while it is set.
- **Before the PLL or mux write** the driver makes one acquire load each of
  the ledger fault word and the clock registry's failed word, and writes
  nothing if either is set. It lowers frequency before the floor, and
  releasing its token requires a zero floor. Its own selector writes and
  ramp waits are removed.
- **Settle.** The driver blocks once for the whole ticks until 300 µs before
  the deadline, then spins on the counter with no lock held. The window
  covers the 800→870 mV step, which needs at most 280 µs.

## The DE_SYS provider becomes the domain owner

`drivers/power/a733_de_sys_power.c`, which powers domains 1 and 10 for G2D
and the framebuffer today, becomes the owner of domains 1, 4, 5 and 10,
renamed `a733_power_domains.c`. Its per-consumer ops, `authorize` callback
and domain-9 path, which has no production caller, are removed.

| Domain | Consumer rows | Supply each row requires |
| --- | --- | --- |
| 1 (DE_SYS) | Framebuffer, G2D, deinterlace, DSI scanout | DCDC2 = 800 mV |
| 4 (NPU) | NPU startup | DCDC2 = 800 mV |
| 5 (GPU_TOP) | GPU | DCDC4 = 800 mV |
| 10 (VO1) | HDMI; DisplayPort | DCDC2 = 800 mV and CLDO2 or CLDO5 = 1,800 mV |

The NPU runs only at 492 MHz, where the pinned vendor operating-point table
gives 800 mV for all 26 speed bins (`checks/npu-platform-reference.py`).

- **Request.** A consumer passes its rail token, checked once. When a row's
  count goes from zero to one, the owner submits one read sequence under
  that token outside its own lock, and the requester's next step reaps it.
  The read waits until provider start plus 5,160 µs, so a ramp left by the
  bootloader cannot pass; until then the request is PENDING. Nothing in the
  OS writes DCDC2, DCDC4, CLDO2 or CLDO5, so a counted row relies on the
  earlier read.
- **Pinning.** A counted row and a PMIC borrow each raise the slot's pin
  count, which replaces the borrow count. Rail release returns BUSY while it
  is nonzero.
- **Preconditions.** Before the ON command the owner checks a per-domain
  table of CCU gate and reset bits, the union of today's consumer reads, and
  writes nothing if an entry fails. It replaces the GPU's CHECK rows for
  `0x05c0`, `0x05e0`, `0x0b20`, `0x0b24` and R_CCU `0x01ac`, and the
  matching reads in `hdmi_native.c`, `apps/displayport/native.c`, the
  framebuffer, G2D and deinterlace. The bit table lives in
  `checks/power-domains-pck.c`. Consumers keep their assert-and-wait writes
  and exclusion leases. Power-on also requires the shared PCK600 bus-clock
  lease (`RCCU + 0x1ac`).
- **Power-off only for domain 5,** whose one bus master is the GPU. At count
  zero, after the GPU drains its DMA, the owner rereads `0x0b20` bit 31 and
  `0x0b24` bits 0 and 16, replacing the GPU's final off CHECK rows. If any is
  set, release returns RETAINED. The MBUS gates `0x05c0` bit 7 and `0x05e0`
  bit 16 stay set across OFF, as today. Domains 1, 4 and 10 have several
  masters each and are never turned off.
- **Boot adoption.** Command and status both OFF give OFF; both ON give ON
  with zero rows and no command written, and domain 5 also needs its gate
  and reset bits clear. Anything else gives FAILED.
- **Transitions.** A domain is OFF, ON_WAIT, ON, OFF_WAIT or FAILED, with a
  deadline 10 ms after its command. A requester that finds a wait state gets
  PENDING. Every requester's step runs the same idempotent poll, which
  publishes ON, OFF, or FAILED once the deadline has passed, so a parked
  initiator cannot hold a domain in a wait state.
- **PCK600 acknowledgement.** No transition interrupt is documented, and the
  vendor driver polls. The owner spins with no lock held, then returns
  RETRY_TICK. The spin bound is a provisional 50 µs; board evidence replaces
  it with the longest measured acknowledgement plus 50%.

## One PMIC service loop wakes requesters

`a733_pmic_service`, called by every image that binds the provider, replaces
the board and OS loop and the loops in camera, gpu-display, gpu-render,
gpu-finite, hdmi, i2s, mimus, npu-convolution, npu-returning,
thermal-returning, usb-returning and storage-update. It runs at priority 3
on A55 cores 2–5, below the USB0, audio, headset and thermal tasks.

- **Work in.** Every enqueue, and every startup stage that waits on no
  hardware, notifies the service task after the provider try-lock is
  released. The task blocks, up to 250 ms, only when nothing is queued.
- **Completion out.** The provider slot records the requester's task
  handle, which the service task notifies after publication, outside its
  locks; reap and cancel clear it. Requesters wait with
  `ulTaskNotifyTakeIndexed` on a dedicated power index, so a PMIC wait
  cannot consume an interrupt wake on index 0, and a stale notification
  costs one recheck. Tasks that submit requests are never deleted.
- **Refusals.** Submit returns BUSY with no request ID while the provider is
  starting, so PENDING always means a queued request with its ID written.
  Every production caller already retries on BUSY.

A snapshot is four single-register transactions (page, enable, selector,
page); a write adds a selector write and a readback. The pinned AXP8191
driver reads one register per transaction, and no pinned source shows
register auto-increment. Chip identity is read once, at verify. Per request
the provider rereads only state another agent can change: the S_TWI0 gate
and reset word, the divisor, the interrupt 235 enable bit, and the three
bus-idle registers after every STOP.

## Bus waits spin to the expected completion, then wait ticks

- **One bus rate.** Verify admits divisor `0x28` only with a 24 MHz parent,
  giving 400 kHz and a 22.5 µs byte time. The tick-per-phase path for slower
  rates is removed.
- **Admission and revocation.** The task checks `clock_live`, `owned()` and
  the borrow status when a request becomes active. Three events can make
  that stale: provider stop, refused while a slot is active; a clock
  retention or counter fault, which latches the registry's failed word; and
  `a733_rail_retain`, which latches the ledger fault word. Before each
  transaction and before the selector-write byte, the task makes one acquire
  load of each word (section 1.1) and faults if either is set.
- **Phases.** The task spins to the expected completion (9 bit times per
  byte), then re-reads control once per byte time for up to 90 µs, then once
  per tick until the deadline. An iteration cap faults if the counter stops.
  Time comes from `CNTPCT_EL0` with `a733_monotonic_advance`.
- **Barriers.** A `DSB SY` follows the write that begins a timed phase;
  polled reads have none. Dropping the `a733_mmio_*_dsb_sy_both` accessors
  waits on the barrier qualification in `docs/shared-helpers.md`.
- **No TWI interrupt.** Byte interrupts would cost 34 per write and a route
  in every image, while settle on the same path waits up to 1,000 µs.

## Memory placement and synchronization

Both objects live in kernel `.bss` (Normal Write-Back, Inner Shareable). No
DMA master reads them, so no cache maintenance applies.

| Ledger region | Size | Written by |
| --- | --- | --- |
| Lock word and free bitmap | 64 B | Acquire, release |
| Fault word with the UNCERTAIN mask | 64 B | A fault (atomic OR) |
| DCDC3 and DCDC5 rail records | 64 B | The holder's publish |
| Holder slots, 32 B × 32 | 1,024 B | That slot's acquire, release and pin count |
| Borrows, 48 B × 10 | 480 B | Provider submit and reap; camera |

The total is 1,696 bytes, down from 20,096. Thirty-two slots cover the 31
current holders; ten borrows cover the provider's eight request slots, the
camera's lifetime borrow and one spare. Two holder slots share a line; pin
counts change at PMIC-request rate, so the sharing is accepted.

Every operation takes one nonblocking try-lock per object and returns BUSY on
contention. Slot indexing removes the global scan per MMIO that the scope
names. A token identifier holds its slot index in the low bits (5 for the
ledger, 7 for the clock registry) and a never-reused generation above them,
so status compares one slot under the lock. The domain owner releases its
lock before it calls the ledger or provider or spins on PCK600. The
forged-token cases in `checks/clock-wide.c`, `clock-width.c` and
`clock-usb-scopes.c` still hold.

A follow-up outside this package moves `live()` in `a733_mmc_platform.c`,
`healthy()` in `a733_twi_native.c` and `leases_live()` in
`a733_camera_native.c` to one status check at admission plus the fault-word
loads (section 2.4).

## Reused, removed and changed elsewhere

- **Reused:** the ledger API and retention, PMIC transactions,
  `a733_pmic_submit_write` and its expected-selector check, the DE_SYS
  PCK600 code and delay row, the PCK gate lease and the CPU grant.
- **Removed:** clock scope `A733_POWER_DCDC2`; the GPU RAIL resource; ledger
  bridges and OPENING; `a733_pmic_consumer.c`; four duplicate PCK
  sequences; the consumer precondition reads; the domain-9 path; six supply
  reads; the NPU direct S_TWI0 path and settle stage; the tick-per-phase
  path; twelve per-image PMIC loops; the separate borrow count; about 44 of
  50 setup reads per request; the 128-slot status scan; the CPU clock
  driver's selector writes.
- **Changed outside this package:** GPU resource and platform files, NPU
  platform, G2D, deinterlace, framebuffer, `hdmi_native.c`, DisplayPort and
  DSI scanout; the image PMIC loops; checks that build a ledger.
  `include/a733_power_domains.h` needs the coordinator's approval.
- **Needed from the primary agent:** `configTASK_NOTIFICATION_ARRAY_ENTRIES`
  of 2 in the platform FreeRTOS configuration, for the power index.

## Performance budgets

The first implementation commit adds the counters below and records the
parent-commit counts with their input identities; baselines are
provisional until then. Instruction and line counts come from a step in
`checks/rail-leases.py` that runs status and acquire, built with clang
`-O2 -mcpu=cortex-a55`, in Unicorn with 42 clock and 16 rail leases, then
126 of each. Bus, spin, tick and wake counts come from
`checks/a733-pmic-polls.c`, `checks/power-domains-pck.c` and
`checks/cpu-clock-model.c` in the board, GPU, NPU and HDMI configurations.

| Operation | Provisional baseline | Target |
| --- | --- | --- |
| Rail status, one-rail token | 536 / 1,554 instructions; 60 / 233 lines | ≤ 60 instructions, ≤ 3 lines |
| Clock token status | 243 / 600 instructions | ≤ 50 instructions, no scan |
| Acquire, poll and release | 4,026 / 7,773 instructions | ≤ 450 instructions, no bus access |
| Authority check per PMIC transaction | 818 / 2,179 instructions per phase | ≤ 10 instructions |
| Bus reads per wait phase | 1 per poll, repeated after each yield | ≤ 2 on time; ≤ 4 more before a tick wait |
| Setup reads per request | About 50 | 6, plus 3 after each STOP |
| Ledger storage | 20,096 B | ≤ 1,696 B (static assertion) |
| Snapshot, submit to requester wake | Up to one tick per step | 4 transactions, ≤ 0.45 ms, 0 ticks |
| Startup to PMIC READY | Recorded by the first commit | ≤ 1 tick plus one transaction |
| Domain power-on with supply read | GPU: 27 polls | ≤ 0.7 ms, 1 wake, 0 ticks within the spin bound |
| Requester finding a wait state | One poll per tick | ≤ 1 tick, once per boot for domains 1, 4 and 10 |
| Slow PCK600 acknowledgement | One tick per poll | ≤ 1 tick per retry; FAILED by 10 ms plus 1 tick |
| Domain 5 power-off | GPU: 8 polls | 1 step |
| CPU rail write, model only | Not applicable | ≤ 0.65 ms to readback; ≤ 2 wakes; spin ≤ 300 µs |
| Delay added to USB0 or OS audio | Not applicable | 0, by priority |

## Model cases

- `checks/rail-leases.c`: production-style acquire, poll and release; a
  RETAINED submit leaves DCDC3 and DCDC5 UNCERTAIN and refuses later
  publishes; an early baseline is refused.
- `checks/a733-pmic.c`: submit before READY returns BUSY with no ID, a
  queued submit PENDING with one; a registry fault latched during a write
  faults the provider before the selector-write byte.
- `checks/power-domains-pck.c`: each table entry blocks the ON write; a
  request before the settle time writes no ON; adoption of each domain; an
  open GPU gate refuses power-off; domains 1, 4 and 10 never get OFF; a
  second requester observes FAILED within the bound when the initiator
  never returns.
- `checks/cpu-clock-model.c`: a fault between READY and the PLL write
  prevents the write; wakes per raise are counted; the existing ordering and
  ramp cases run against the floor calls.

## Failure classes (section 1.8)

- **Refused before any bus access** (BUSY, EXPIRED, INVALID, EXHAUSTED):
  nothing changed or retained.
- **Supply mismatch or failed precondition:** the row is not counted and no
  ON command is written.
- **Unknown state:** a bus fault, readback mismatch, changed selector,
  latched fault word or expiry after a write's first transaction retains the
  provider and active borrow and marks DCDC3 and DCDC5 UNCERTAIN. An
  unexpected PCK600 status, or a passed transition deadline, retains the
  domain, its rows, their tokens and the PCK lease.

## Not included, with reasons

- **DCDC2 and DCDC4 writes:** no NPU point at 492 MHz needs more than 800 mV
  and no GPU operating-point table exists.
- **Lowering below the baseline:** it needs every consumer of each rail.
- **Per-domain waiter lists:** a wait state occurs once per boot for domains
  1, 4 and 10, and domain 5 has one row.
- **Lock-free status:** it waits for a measured per-access caller that
  remains after the admission follow-up.
- **Lock-free floor publication:** it waits for a production governor, the
  first reader of the rail record other than its holder.
- **PMIC burst reads:** no pinned source shows register auto-increment.
- **Power-off for domains 1, 4 and 10:** it needs drain evidence for every
  master, and no caller needs it.
- **CPU cluster and core domains** (reserved code) and **domains 0, 2, 3
  and 6–9** (current owners stay).

## Board evidence that would confirm the design

- The measured S_TWI0 SCL rate for divisor `0x28`, and whether a two-byte
  read from `0x10` returns registers `0x10` and `0x11`.
- DCDC3 or DCDC5 readbacks after a raise and a lowering, with
  request-to-readback and wake-to-READY times and the longest spins.
- PMIC page 0 and unchanged DCDC2–5 selectors after PSCI CPU_ON, CPU_OFF and
  suspend, and a capture showing no other S_TWI0 master.
- PCK600 time to ON for domains 1, 4, 5 and 10 and to OFF for domain 5
  across repeated cycles, which sets the spin bound.
- PMU cycles for rail and clock status on an A55 and an A76.
- A schematic source for the DE_SYS, G2D and deinterlace supply.
