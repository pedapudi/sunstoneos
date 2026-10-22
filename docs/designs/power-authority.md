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
two rails accept an exclusive mask, and each admits one holder. The scope
requires a voltage floor per consumer, and the CPU clusters are the only
consumers whose floor changes. The floor path replaces the driver's own
selector writes and ramp waits, so each rail keeps one writer. The driver has
no production caller today, so the rail records, baseline snapshot, publish
call and settle path live in `drivers/power/a733_rail_floor.c`, which only
images that link the CPU clock driver link. Other images carry no rail
records or publish code.

- **Rail record.** One 32-byte record per rail holds the baseline, the
  published voltage, the floor and the settle deadline. Only the holder's
  `a733_rail_publish` writes it, under the ledger lock. The PMIC task never
  writes a rail record or a holder slot.
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
- **Fault word.** The ledger fault word holds the retention bit and a
  per-rail UNCERTAIN mask for DCDC3 and DCDC5. Every writer, the provider's
  fault path and `a733_rail_retain` included, sets bits with
  `atomic_fetch_or` through one inline helper, and nothing clears them, so
  two cores cannot overwrite each other's bits. `checks/rail-leases.py`
  fails if any other code stores to the word. Status reports UNCERTAIN, and
  publication is refused while it is set.
- **Before the PLL or mux write** the driver makes one acquire load each of
  the ledger fault word and the clock registry's failed word, and writes
  nothing if either is set. It lowers frequency before the floor, and
  releasing its token requires a zero floor.
- **Settle.** The driver blocks once for the whole ticks until 300 µs before
  the deadline, then spins on the counter with no lock held. The window
  covers the 800→870 mV step, which needs at most 280 µs.

## The DE_SYS provider becomes the domain owner

`drivers/power/a733_de_sys_power.c`, which powers domains 1 and 10 for G2D
and the framebuffer today, becomes the owner of domains 1, 4, 5 and 10,
renamed `a733_power_domains.c`. Its per-consumer ops, `authorize` callback
and domain-9 path, which has no production caller, are removed.

| Domain | Consumer rows | Supply each row requires | Domain-level reads |
| --- | --- | --- | --- |
| 1 (DE_SYS) | Framebuffer, G2D, deinterlace, DSI scanout | DCDC2 = 800 mV | MBUS gates `0x5c0` bit 5, `0x5e0` bit 11 |
| 4 (NPU) | NPU startup | DCDC2 = 800 mV | MBUS gates `0x5c0` bit 6, `0x5e0` bit 18 |
| 5 (GPU_TOP) | GPU | DCDC4 = 800 mV | MBUS gates `0x5c0` bit 7, `0x5e0` bit 16 |
| 10 (VO1) | HDMI; DisplayPort | DCDC2 = 800 mV and CLDO2 or CLDO5 = 1,800 mV | None beyond the shared reads |

Every domain also requires the PCK600 bus clock (R_CCU `0x1ac` bit 0) and
agreement between the command and status fields. The NPU runs only at
492 MHz, where the pinned vendor operating-point table gives 800 mV for all
26 speed bins (`checks/npu-platform-reference.py`).

- **Request.** A consumer passes its rail token, checked once. The first
  time a row's supply is needed after boot, the owner submits one read
  sequence under that token outside its own lock, and the requester's next
  step reaps it. The read waits until provider start plus 5,160 µs, so a
  ramp left by the bootloader cannot pass; until then the request is
  PENDING. A matching read sets a per-rail verified bit. Nothing in the OS
  writes DCDC2, DCDC4, CLDO2 or CLDO5, so later requests, including each new
  GPU session after domain 5 turned off, cost one acquire load of the ledger
  fault word instead of a read. A set fault word or a domain retention
  invalidates every verified bit.
- **Pinning.** A counted row and a PMIC borrow each raise the slot's pin
  count, which replaces the borrow count. Rail release returns BUSY while it
  is nonzero.
- **Domain-level reads on every request.** The owner runs the table's
  domain-level reads on every row request, in every domain state, and at
  boot adoption, and counts no row if one fails. The table is one constant
  array in `a733_power_domains.c`; `checks/power-domains-pck.c` includes that
  file and exercises the array.
- **Consumer reads stay.** Reads whose expected value depends on the block
  or the session stay in the consumer and run in every domain state. They
  are the GPU module clock and reset idle check (`0x0b20` bit 31, `0x0b24`
  bits 0 and 16); the framebuffer's DE clock-off check against inherited
  scanout DMA (`0xa00` bit 31, `0xa04` bit 0); HDMI's exclusion of the TV,
  eDP, HDCP and CEC producers and their bus gates; the NPU baseline of
  `0xb00` and `0xb04`; and the G2D and deinterlace IOMMU gates, compared
  against `session.iommu_gates`. Consumers also keep their assert-and-wait
  writes, exclusion leases and the PCK600 bus-clock lease.
- **Removed consumer reads.** The GPU's CHECK rows for `0x05c0`, `0x05e0`
  and R_CCU `0x01ac`; the framebuffer's and deinterlace's `0x5c0` bit 5,
  `0x5e0` bit 11 and domain-1 command and status reads; the HDMI and
  DisplayPort domain-10 command and status reads and HDMI's R_CCU `0x1ac`
  read; the NPU's `0x5c0` bit 6, `0x5e0` bit 18, R_CCU `0x1ac` and domain-4
  command and status reads. In each of the three domain states (OFF, adopted
  ON with no rows, ON with rows) the owner's domain-level reads for that
  request replace them.
- **Domain 4 keeps the pinned NPU order.** The NPU deasserts the AHB, AXI and
  core resets (`0xb04` bits 18, 17, 16), sets the module rate, enables the
  bus clock (`0xb04` bit 0) and the module clock (`0xb00` bit 31), and only
  then requests the domain. The owner writes the delay row and the ON
  command. This is the order of the pinned VIPLite platform driver
  (`vip_drv_device_platform.c`) that `checks/npu-platform-reference.py`
  extracts. The domain-4 table has no entry for `0xb00` or `0xb04`, so the
  owner accepts that pre-ON state. The NPU keeps its 2,000 µs post-ON settle
  and its readback of `0xb04` bits `0xf0001` and `0xb00`. Domains 1, 5 and
  10 power on before their consumers enable clocks, as today.
- **Power-off only for domain 5,** whose one bus master is the GPU. At count
  zero, after the GPU drains its DMA, the owner rereads `0x0b20` bit 31 and
  `0x0b24` bits 0 and 16, replacing the GPU's final off CHECK rows. If any is
  set, release returns RETAINED. The MBUS gates stay set across OFF, as
  today. Domains 1, 4 and 10 have several masters each and are never turned
  off.
- **Boot adoption.** Command and status both OFF give OFF; both ON give ON
  with zero rows and no command written, and domain 5 also needs its gate
  and reset bits clear. Anything else gives FAILED.
- **Transitions.** A domain is OFF, ON_WAIT, ON, OFF_WAIT or FAILED, with a
  deadline 10 ms after its command and a sequence number that each command
  increments. A requester that finds a wait state gets PENDING. Every
  requester's step runs the same poll. It captures the sequence number under
  the owner lock and spins on status with no lock held. It then retakes the
  lock, rereads status, and publishes ON, OFF or FAILED only if the domain is
  still in the wait state of that sequence number. Otherwise it returns
  PENDING, so a stale poll cannot overwrite a newer command. FAILED is published once
  the deadline has passed, so a parked initiator cannot hold a domain in a
  wait state.
- **PCK600 acknowledgement is polled.** The pinned device tree node
  `pck-600@7060000` in `sun60iw2p1.dtsi` has no `interrupts` property, and
  `pck600_domains.c` polls status with `readx_poll_timeout_atomic` for up to
  10,000 µs and never touches the PPU interrupt mask or status registers
  that the Arm PPU architecture defines. The owner spins with no lock held,
  then returns RETRY_TICK. The spin bound is a provisional 50 µs; board
  evidence replaces it with the longest measured acknowledgement plus 50%.
  A GIC line for the PCK-600, named in a vendor source or shown pending
  after an unmasked transition on the board, would replace the spin and the
  RETRY_TICK.

## One PMIC service loop wakes requesters

`a733_pmic_service`, called by every image that binds the provider, replaces
the board and OS loop and the loops in camera, gpu-display, gpu-render,
gpu-finite, hdmi, i2s, mimus, npu-convolution, npu-returning,
thermal-returning, usb-returning and storage-update. It runs at priority 3
on A55 cores 2–5, below the USB0, audio, headset and thermal tasks.

- **Work in.** Every enqueue, and every startup stage that waits on no
  hardware, notifies the service task after the provider try-lock is
  released. The task blocks, up to 250 ms, only when nothing is queued.
- **Completion out.** A task registers once with `a733_pmic_register`
  before it submits; submit returns INVALID for an unregistered task. The
  slot records the task handle, which the service task notifies after
  publication, outside its locks; reap and cancel clear it. A task that may
  be deleted calls `a733_pmic_unregister` first, which returns BUSY while
  the task has a queued or active request, so the provider never notifies a
  deleted handle. Requesters wait with `ulTaskNotifyTakeIndexed` on
  `A733_NOTIFY_INDEX_POWER`, so a PMIC wait cannot consume an interrupt wake
  on index 0, and a stale notification costs one recheck.
- **Refusals.** Submit returns BUSY with no request ID while the provider is
  starting, so PENDING always means a queued request with its ID written.
  Every production caller already retries on BUSY.

A snapshot is four single-register transactions (page, enable, selector,
page); a write adds a selector write and a readback. The pinned AXP8191
driver reads one register per transaction, and no pinned source shows
register auto-increment. Chip identity is read once, at verify. Interrupt
235 (S_TWI0) has no handler or enable call anywhere in the tree, so verify
checks its enable, pending and active bits once, and `checks/pmic-*` fails
if any source other than the provider names that interrupt. Per request the
provider rereads only state another agent can change: the S_TWI0 gate and
reset word, the divisor, and the three bus-idle registers, which it rereads
after every STOP.

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
- **The provider object is never held across a spin.** The task takes the
  object's try-lock only to access a TWI register and update the slot's
  phase, and releases it before each spin. The I2C bus mutex, held from
  START to STOP, alone excludes other bus users. Only the service task
  changes the active slot's phase, so submit and reap on other cores touch
  other fields. If the object is busy when a spin ends, the task spins one
  more byte time, within the phase deadline.
- **`a733_pmic_live` takes no lock.** It compares the caller's lifetime with
  the provider's start-time fields, then makes one acquire load each of the
  provider state word (STARTING, READY or RETAINED, release-stored under the
  object), the clock registry failed word and the ledger fault word. A
  `live()` caller on another core, such as `a733_mmc_platform.c` once per
  MMC step, never sees BUSY from a PMIC request.
- **Barriers.** A `DSB SY` follows the write that begins a timed phase;
  polled reads have none. Dropping the `a733_mmio_*_dsb_sy_both` accessors
  waits on the barrier qualification in `docs/shared-helpers.md`.
- **No TWI interrupt yet.** Byte interrupts would cost 34 per write and a
  route in every image. Each spun write costs about 0.95 ms of A55 time
  including settle, so at 10 floor changes per second the spin uses about
  1% of one core. Interrupt 235 replaces the byte spin when a production
  governor's measured rate exceeds 10 floor changes per second.

## HDMI register access checks four words instead of seven try-locks

`hdmi_native.c` calls `tokens()` inside both `live()` and `lock()`, so each
`read32`, `write32` and `update` runs the clock, rail and GPU-resource
status try-locks twice and `a733_pmic_live` once. A BUSY from any of them
reaches `fail()`, which retains every HDMI resource. This package changes
that file:

- `tokens()` runs once when the sequence is admitted and in `validate()`. A
  BUSY there returns 0, and the caller retries; INVALID or RETAINED still
  fails.
- Each register access makes four acquire loads: the lock-free
  `a733_pmic_live` (provider state, clock registry failed word, ledger fault
  word) and the GPU resource manager's failed word. That word becomes an
  atomic latch set by `atomic_fetch_or`; the manager file is changed
  outside this package. A token is revoked only by retention, and every
  retention latches one of these words.

## Memory placement and synchronization

Both objects live in kernel `.bss` (Normal Write-Back, Inner Shareable). No
DMA master reads them, so no cache maintenance applies.

| Ledger region | Size | Written by |
| --- | --- | --- |
| Lock word and free bitmap | 64 B | Acquire, release |
| Fault word | 64 B | `atomic_fetch_or` only |
| Holder slots, 32 B × 32 | 1,024 B | That slot's acquire, release and pin count |
| Borrows, 48 B × 10 | 480 B | Provider submit and reap; camera |

The ledger totals 1,632 bytes, down from 20,096; the two rail records add 64
bytes only in images that link the CPU clock driver. Thirty-two slots cover
the 31 current holders; ten borrows cover the provider's eight request
slots, the camera's lifetime borrow and one spare. Two holder slots share a
line; pin counts change at session-start rate, so the sharing is accepted.

Acquire, release, submit, reap and admission status take one nonblocking
try-lock per object and return BUSY on contention; per-access paths take
none. Slot indexing removes the global scan that the scope names. A token
identifier holds its slot index in the low bits (5 for the ledger, 7 for the
clock registry) and a never-reused generation above them, so status compares
one slot under the lock. The domain owner releases its lock before it calls
the ledger or provider or spins on PCK600. The forged-token cases in
`checks/clock-wide.c`, `clock-width.c` and `clock-usb-scopes.c` still hold.

A follow-up outside this package moves `live()` in `a733_mmc_platform.c`,
`healthy()` in `a733_twi_native.c` and `leases_live()` in
`a733_camera_native.c` to one status check at admission plus the fault-word
loads (section 2.4). Until then the MMC step still takes the clock and rail
status try-locks, which return retry on BUSY; its PMIC check no longer does.

## Reused, removed and changed elsewhere

- **Reused:** the ledger API and retention, PMIC transactions,
  `a733_pmic_submit_write` and its expected-selector check, the DE_SYS
  PCK600 code and delay row, the PCK gate lease and the CPU grant.
- **Removed:** clock scope `A733_POWER_DCDC2`; the GPU RAIL resource; ledger
  bridges and OPENING; `a733_pmic_consumer.c`; four duplicate PCK
  sequences; the consumer domain-level reads; the domain-9 path; six supply
  reads and every repeat of them; the NPU direct S_TWI0 path and settle
  stage; the tick-per-phase path; twelve per-image PMIC loops; the separate
  borrow count; about 44 of 50 setup reads per request; the 128-slot status
  scan; the CPU clock driver's selector writes; HDMI's per-access try-locks.
- **Changed outside this package:** GPU resource and platform files, NPU
  platform, G2D, deinterlace, framebuffer, `hdmi_native.c`, DisplayPort and
  DSI scanout; the image PMIC loops; checks that build a ledger.
  `include/a733_power_domains.h` needs the coordinator's approval.
- **Needed from the primary agent:** `configTASK_NOTIFICATION_ARRAY_ENTRIES`
  of 2 and a named constant `A733_NOTIFY_INDEX_POWER` of 1 in the platform
  FreeRTOS configuration, reserving that index for power waits.

## Performance budgets

The first implementation commit adds the counters below and records the
parent-commit counts with their input identities; baselines are
provisional until then. Instruction and line counts come from a step in
`checks/rail-leases.py` that runs status and acquire, built with clang
`-O2 -mcpu=cortex-a55`, in Unicorn with 42 clock and 16 rail leases, then
126 of each. Bus, spin, CPU-busy, tick and wake counts come from
`checks/a733-pmic-polls.c`, `checks/power-domains-pck.c` and
`checks/cpu-clock-model.c` in the board, GPU, NPU and HDMI configurations;
CPU-busy time is the sum of modelled spin intervals.

| Operation | Provisional baseline | Target |
| --- | --- | --- |
| Rail status, one-rail token | 536 / 1,554 instructions; 60 / 233 lines | ≤ 60 instructions, ≤ 3 lines |
| Clock token status | 243 / 600 instructions | ≤ 50 instructions, no scan |
| Acquire, poll and release | 4,026 / 7,773 instructions | ≤ 450 instructions, no bus access |
| Authority check per PMIC transaction | 818 / 2,179 instructions per phase | ≤ 10 instructions |
| `a733_pmic_live` BUSY results on another core during one snapshot | BUSY while a poll holds the object, up to 8 phases | 0; no lock taken |
| Provider object hold | Up to 8 bus phases | ≤ 4 register accesses; never across a spin |
| HDMI register access | 7 try-locks; BUSY retains | 0 try-locks, 4 acquire loads |
| Bus reads per wait phase | 1 per poll, repeated after each yield | ≤ 2 on time; ≤ 4 more before a tick wait |
| Setup reads per request | About 50 | 5, plus 3 after each STOP |
| Ledger storage | 20,096 B | ≤ 1,632 B (static assertion) |
| Snapshot, submit to requester wake | Up to one tick per step | 4 transactions, ≤ 0.45 ms, 0 ticks |
| CPU busy per snapshot | Not recorded | ≤ 0.45 ms |
| Startup to PMIC READY | Recorded by the first commit | ≤ 1 tick plus one transaction |
| First domain power-on with supply read | GPU: 27 polls | ≤ 0.7 ms, 1 wake, 0 ticks within the spin bound |
| Later row request, supply verified | One supply read per request | 0 PMIC requests, 1 acquire load |
| Requester finding a wait state | One poll per tick | ≤ 1 tick, once per boot for domains 1, 4 and 10 |
| Slow PCK600 acknowledgement | One tick per poll | ≤ 1 tick per retry; FAILED by 10 ms plus 1 tick |
| Domain 5 power-off | GPU: 8 polls | 1 step |
| CPU rail write, model only | Not applicable | ≤ 0.65 ms to readback; ≤ 2 wakes |
| CPU busy per write, model only | Not applicable | ≤ 0.65 ms on the service task plus ≤ 300 µs settle on the caller |
| Delay added to USB0 or OS audio | Not applicable | Unmeasured until the board session; the model asserts that no spin holds a lock that USB0 or audio code takes |

## Model cases

- `checks/rail-leases.c`: production-style acquire, poll and release; a
  RETAINED submit leaves DCDC3 and DCDC5 UNCERTAIN and refuses later
  publishes; an early baseline is refused; a retention and an UNCERTAIN
  fault set from two threads both survive.
- `checks/a733-pmic.c`: submit before READY returns BUSY with no ID, a
  queued submit PENDING with one; an unregistered task's submit returns
  INVALID; unregister returns BUSY with a request queued; a registry fault
  latched during a write faults the provider before the selector-write byte.
- `checks/a733-pmic-polls.c`: a concurrent `a733_pmic_live` caller sees no
  BUSY across a snapshot and a write; the object is free during every spin.
- `checks/power-domains-pck.c`: each domain-level entry blocks the row in
  OFF, adopted ON and ON with rows; adopted-ON domain 5 with `0x05c0` bit 7
  clear refuses the GPU row; adopted-ON domain 1 with the DE clock on is
  refused by the framebuffer's own check; the domain-4 write order matches
  the `checks/npu-platform.c` trace; a request before the settle time writes
  no ON; a second GPU session issues no supply read; a stale poll after a
  newer command returns PENDING and publishes nothing; an open GPU gate
  refuses power-off; domains 1, 4 and 10 never get OFF; a second requester
  observes FAILED within the bound when the initiator never returns.
- `checks/cpu-clock-model.c`: a fault between READY and the PLL write
  prevents the write; wakes and CPU-busy time per raise are counted; the
  existing ordering and ramp cases run against the floor calls.
- An HDMI model case: a rail status BUSY during admission returns 0 and
  retains nothing; a latched fault word during a register access fails.

## Failure classes (section 1.8)

- **Refused before any bus access** (BUSY, EXPIRED, INVALID, EXHAUSTED):
  nothing changed or retained. A BUSY from any status check returns retry.
- **Supply mismatch or failed domain-level read:** the row is not counted
  and no ON command is written.
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
- **Lock-free rail and clock token status:** after this change no per-access
  caller remains in this package's files; the MMC, TWI and camera callers
  move to admission in the follow-up.
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
  across repeated cycles, which sets the spin bound, and whether any GIC
  line goes pending after a PCK600 transition.
- USB0 and OS audio latency with PMIC requests running on the same cores.
- PMU cycles for rail and clock status on an A55 and an A76.
- A schematic source for the DE_SYS, G2D and deinterlace supply.
