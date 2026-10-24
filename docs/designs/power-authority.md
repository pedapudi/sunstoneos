# Design: one owner for supply rails and one for power domains

Status: **implemented in part and host-modeled**. The power-domain owner and
the PMIC service task are implemented as designed in
[power-domains.md](power-domains.md), which supersedes the sections below on
the domain owner and the service task. The section "Implemented state and
measurements" at the end lists what exists, what differs from this design and
what remains. Physical behavior stays unqualified until the board
evidence exists. Section numbers refer to `docs/performance-architecture.md`.

Today the clock registry holds AXP8191 output DCDC2, the GPU resource manager
holds DCDC4, and the rail ledger reaches both through bridges. Five drivers
repeat the PCK600 power-domain sequence, and six read the same supplies from
the PMIC.

## One state word carries readiness and revocation

The **power-authority state word** is one 64-byte line in kernel `.bss`. Its
bits are provider READY, supply verification done, one verified bit each for
DCDC2, DCDC4, CLDO2 and CLDO5, and fault bits for the provider, the clock
registry, the rail ledger and each of domains 1, 4, 5 and 10. Every bit is
set once per boot with `atomic_fetch_or` and never cleared, so READY cannot
overwrite a fault set on another core during startup. Readers make one
acquire load and test a mask, and any set fault bit in the mask refuses the
caller. The word replaces `p->failed`, the clock registry failed word and
the ledger fault word.

## The rail ledger owns fifteen rails; the CPU clock driver writes two

The ledger keeps a fixed table of the fifteen outputs that production code
acquires: DCDC1–5, DCDC8, DCDC9, BLDO1, BLDO2, BLDO5, CLDO1, CLDO2, CLDO3,
CLDO5 and ELDO6. DC1SW2 stays an alias of DCDC1; other outputs are refused.
A holder slot stores the token identifier, owner, rail masks and one pin
count; a counted domain row and a PMIC borrow each raise the pin count, and
rail release returns BUSY while it is nonzero. A token identifier
holds its slot index in its low bits and a never-reused generation above
them, so status compares one slot instead of scanning. The ledger shrinks
from 20,096 to 1,632 bytes: 32 holder slots of 32 B cover the 31 current
holders, and 10 borrows of 48 B cover eight request slots, the camera's
lifetime borrow and one spare.

`a733_rail_acquire` returns PENDING and `a733_rail_poll` returns READY on
its first call with no bus access, a compatibility shim for the 59 `a733_rail_poll` call sites; a follow-up in
the consumers' packages makes acquire return READY and removes the poll
calls. The framebuffer, GPU and NPU acquire their own tokens instead of
reaching DCDC2 or DCDC4 through the clock registry, the GPU manager or
`a733_pmic_consumer.c`, and the NPU startup's direct S_TWI0 path is removed.

DCDC3 (big cluster) and DCDC5 (little cluster and DSU) accept an exclusive
mask with one holder, the CPU clock driver `drivers/clock/a733_cpu_clock.c`.
For the voltage floor per consumer that the scope requires, the driver
records each cluster's boot baseline from its first selector read and sets
the target to the larger of that baseline and the operating point. The
existing `a733_pmic_submit_write` path with its expected-selector check
stays the only writer, so a refused or expired lowering keeps the higher
voltage. The settle wait blocks for whole ticks until 300 µs before the
deadline, then spins on raw counter ticks as the PMIC phases do. The driver
has no production caller; a core placement mask for its settle spin and a
ledger-held floor record are preconditions for its first production caller.

## One power-domain owner

Status: **implemented and host-modeled** for domains 1, 4 and 5;
[power-domains.md](power-domains.md) is the design that was built and records
its measurements, and [docs/power-domains.md](../power-domains.md) describes
the component. `drivers/power/a733_power_domains.c` replaced the DE_SYS
provider (`a733_de_sys_power.c`, deleted) and the GPU's own PCK600 steps. It
is the only writer of domains 1 (DE_SYS) and 5 (GPU_TOP); the framebuffer,
G2D and the GPU request them through rows, and the service task of each image
runs every transition. Domain 4 has an owner entry and a row interface, but
the NPU platform still writes it until the NPU startup moves onto its row.
Domain 10 (VO1) has no owner entry: HDMI and DisplayPort still power it
themselves, and CLDO2 and CLDO5 are not verified. Domains 0, 2, 3 and 6–9 and
the CPU cluster and core domains keep their current owners. The owner has
these properties, which `checks/power-domains-pck.c` covers:

- A release that meets the ledger try-lock held on another core returns BUSY
  and keeps the row counted; a failed unpin answers the row FAILED and keeps
  it counted, and a later ON request from that row is answered FAILED.
- Supply verification starts 5,160 µs after the service's first step and
  ends within 100 ms of the provider becoming READY. Any terminal failure (an
  expired deadline, a refused token or submission, a provider, clock or
  ledger fault) sets the done bit with no verified bit at once, so
  row requests become REFUSED within a bounded time instead of PENDING
  forever.

The table lists every domain the design covered; the text above states which
rows exist.

| Domain | Consumer rows | Required supply bits | Domain-level reads |
| --- | --- | --- | --- |
| 1 (DE_SYS) | Framebuffer, G2D, deinterlace, DSI scanout | DCDC2 | MBUS gates `0x5c0` bit 5, `0x5e0` bit 11 |
| 4 (NPU) | NPU startup | DCDC2 | MBUS gates `0x5c0` bit 6, `0x5e0` bit 18 |
| 5 (GPU_TOP) | GPU | DCDC4 | MBUS gates `0x5c0` bit 7, `0x5e0` bit 16 |
| 10 (VO1) | HDMI; DisplayPort | DCDC2, and CLDO2 or CLDO5 | None beyond the shared reads |

Every domain also requires the PCK600 bus clock (R_CCU `0x1ac` bit 0) and
agreement between its command and status fields. The NPU runs only at
492 MHz, where the pinned vendor operating-point table gives 800 mV for all
26 speed bins (`checks/npu-platform-reference.py`), so the NPU supply
selection is the DCDC2 verified bit. Nothing writes DCDC2 or DCDC4.

- **Supply verification once per boot.** The service task waits until its
  first step plus 5,160 µs (the full-range ramp and enable delay in the
  device tree), then reads the enable and selector of DCDC2 and DCDC4: 12
  transactions. Each rail at 800 mV sets its verified bit, and then the done
  bit is set. A row request passes its rail token, checked once, and makes
  one acquire load of the state word. It waits before the done bit, and is
  refused if a required verified bit is clear or a relevant fault bit is
  set. No row request submits a PMIC read. The scheme relies on no agent
  writing those rails after boot: the OS issues no PSCI CPU_OFF or suspend
  call, and `checks/pmic-*` fails if such a function identifier appears in
  the tree. Firmware writes and a second S_TWI0 master stay open until the
  board evidence exists.
- **Read-only check before consumer writes.** `a733_power_domain_check`
  runs the domain-level reads and the adoption check and writes nothing.
  Each consumer calls it before its first reset or clock write, and the
  consumer's own copies of those reads are removed. The row request repeats
  the reads, because another row can change the domain in between. Reads
  whose expected value depends on the block or session stay in the
  consumer, such as the GPU reset idle check and the framebuffer's check for
  inherited scanout DMA.
- **Domain 4 keeps the pinned NPU order.** The NPU deasserts its resets
  (`0xb04` bits 18, 17, 16), sets the module rate and enables its clocks
  before it requests the domain, as the pinned VIPLite driver
  (`vip_drv_device_platform.c`) does. The CCU and the PPU are different
  peripherals, so the NPU issues a `DSB SY` after its last CCU write and
  before the row request; that barrier stays when the move off
  `a733_mmio_*_dsb_sy_both` is qualified. The owner writes the delay row and
  the ON command; the NPU keeps its 2,000 µs post-ON settle and readback.
  Domains 1, 5 and 10 power on before their consumers enable clocks.
- **Power-off only for domain 5.** Its one bus master is the GPU. At count
  zero, after the GPU drains its DMA, the owner rereads the GPU gate and
  reset bits (`0x0b20` bit 31, `0x0b24` bits 0 and 16); if any is set,
  release returns RETAINED. Domains 1, 4 and 10 have several masters each,
  no caller needs them off, and they stay on.
- **Adoption and transitions.** Command and status both OFF give OFF; both
  ON give ON, and domain 5 also needs its gate and reset bits clear;
  anything else gives FAILED without a write. A domain is UNKNOWN, OFF,
  ON_WAIT, ON, OFF_WAIT or FAILED, with a deadline 10 ms after its command.
  Only the service task reads or changes this state, so the owner takes no
  lock: it spins on status for up to 50 µs after the command, then reads it
  once per step, and publishes ON, OFF or FAILED to the rows with release
  order. Commands are written only from OFF or ON.
- **PCK600 acknowledgement is polled.** The pinned `pck-600@7060000` node
  has no `interrupts` property. The owner spins up to a provisional 50 µs,
  then returns RETRY_TICK; board evidence replaces the bound with the
  longest measured acknowledgement plus 50%.
- **Retention.** An unexpected PCK600 status or a passed deadline retains
  the domain, its rows, their tokens and the PCK lease, and sets the
  domain's fault bit. A supply mismatch or a failed domain-level read leaves
  the row uncounted and writes no command.
- **Barriers (section 1.2).** Domain-level, command and status reads are
  plain reads of one Device-nGnRnE peripheral and stay in order, and the
  command write depends on them through a branch. The delay row and command
  are plain writes followed by one `DSB SY`, because the spin bound is timed
  from the command reaching the PPU.

## One PMIC service task wakes requesters

Status: **superseded.** The service task that exists is
`a733_power_service_step()` in `drivers/power/a733_power_domains.c`, one task
per image on A55 core 0 below its USB0 task, whose design and measurements are
in [power-domains.md](power-domains.md). Submitters keep polling
`a733_pmic_result` once per tick, so the requester notifications below do not
exist. The rest of this section is the earlier proposal.

`a733_pmic_service` replaces the PMIC loops in the board and OS images and in
camera, gpu-display, gpu-render, gpu-finite, hdmi, i2s, mimus,
npu-convolution, npu-returning, thermal-returning, usb-returning and
storage-update. It runs at priority 3, below the USB0, audio, headset and
thermal tasks, with affinity `A733_PMIC_SERVICE_CORES`: A55 cores 2–5 where
the scheduler starts them, and core 0 in single-core images.
`checks/pmic-*` fails if the mask is not a subset of the image's scheduled
cores.

- **Work in.** Every enqueue and startup stage notifies the task. With
  nothing queued it blocks with `portMAX_DELAY`; during the startup ramp it
  blocks for the remaining ticks.
- **Two object acquisitions per request.** The provider object is a
  nonblocking try-lock. The service task takes it at activation, together
  with I2C bus-mutex entry and the borrow and token admission checks, and
  again at publication. Phase, byte index and spin time are private to the
  task; submit, result and reap on other cores touch other slot fields.
- **Completion out.** Submit records `xTaskGetCurrentTaskHandle()` in the
  slot. Publication stores DONE or EXPIRED, copies and clears the handle,
  leaves the object, and only then notifies the copied handle, unless the
  provider fault bit is set. A woken higher-priority requester therefore
  never finds the object held by the task that woke it. `result()` clears
  the handle when it returns RETAINED. `checks/pmic-*` fails if
  `vTaskDelete` appears in the tree, because a task that reaps and deletes
  itself before the notify would be notified after deletion.
- **Waits are bounded.** Requesters wait with `ulTaskNotifyTakeIndexed` on
  `A733_NOTIFY_INDEX_POWER`, which keeps PMIC wakes off interrupt index 0,
  with a timeout of the ticks left before the request deadline. A BUSY from
  submit, result, reap or the domain owner's lock is retried only after the
  same wait with a one-tick timeout. A higher-priority caller therefore
  cannot spin against a preempted lower-priority holder, and a notify still
  ends the wait early.
- **Refusals.** Submit returns BUSY with no request ID until READY.

A snapshot is four single-register transactions (page, enable, selector,
page); a write adds a selector write and a readback. Chip identity, the
S_TWI0 gate and reset word and the divisor are checked once at verify,
because no agent other than the provider is known to change them. The three
bus-idle registers are read at activation and after every STOP, which
detects another master's activity. Only the provider names the S_TWI0
interrupt, GIC-600 INTID 235 (`twi@7083000`, SPI 203 in
`board-kernel/sun60iw2p1.dtsi`), and `checks/pmic-*` enforces this.

## Bus waits spin on raw counter ticks

- **One bus rate.** Verify admits divisor `0x28` only with a 24 MHz parent:
  400 kHz and 22.5 µs per byte. The tick-per-phase path is removed.
- **Time source.** `a733_counter_clock_now` takes the global ticket lock
  with interrupts masked (section 1.7), so the provider never calls it. The
  native adapter's time read returns `ISB; MRS CNTPCT_EL0` divided by the
  ticks per microsecond, the same microseconds as the counter clock, and
  refuses after a fault latched in the counter clock. The provider keeps its
  `a733_monotonic_advance` check, and each future spin adds an iteration cap
  that faults if the counter stops. The PCK600 and CPU settle spins use the
  same read.
- **Phases.** The task spins to the expected completion (9 bit times per
  byte), rereads control once per byte time for up to 90 µs, then once per
  tick until the deadline. A `DSB SY` follows each write that starts a
  timed phase; polled reads have none.
- **Revocation inside a transfer.** Before each transaction and before the
  selector-write byte, the task makes one acquire load of the state word
  (section 1.1). A set fault bit makes it call `fault()`, which retains the
  I2C mutex with the transaction incomplete, and the task makes no further
  register access. `fault()` only sets a bit, so a fault raised on another
  core takes no lock.
- **`a733_pmic_live` takes no lock.** It compares the caller's lifetime
  with the provider's start time and makes one acquire load.
- **The TWI packet engine replaces the byte spin at a rate threshold.** The
  pinned board device tree sets `twi_drv_used = <1>` on `s_twi0`, so the
  vendor driver uses the TWI packet engine (`TWI_DRV_CTRL`,
  `TWI_DRV_INT_CTRL`, a FIFO and DMA request 47). The engine raises one
  completion per transaction on INTID 235, about 6 per write, with no
  per-byte CPU work. A spun write costs about 0.65 ms of A55 time, plus
  0.3 ms of settle spin on a CPU rail; at 10 CPU voltage changes per second
  that is about 1% of one core. The packet engine replaces the byte spin
  when the measured voltage-change rate exceeds 10 per second or the engine
  is qualified on the board.

## HDMI and other per-access callers

HDMI's resources sit on domain 10, so HDMI's `validate()` must observe the
domain-10 fault bit. The HDMI driver package changes `hdmi_native.c`:
`validate()` loads the state word and the GPU resource manager's failed
latch, and the register accessors drop their per-access try-locks. This
package supplies the state word, the HDMI mask and the domain-10 bit. A
follow-up outside this package moves `live()` in `a733_mmc_platform.c`,
`healthy()` in `a733_twi_native.c` and `leases_live()` in
`a733_camera_native.c` to one admission check plus the state-word load
(section 2.4).

## Changes outside this package

- GPU resource manager and platform, NPU platform, G2D, deinterlace,
  framebuffer, DisplayPort, DSI scanout, the image PMIC loops, and checks
  that build a ledger. `include/a733_power_domains.h` needs the
  coordinator's approval.
- **Primary agent:** `configTASK_NOTIFICATION_ARRAY_ENTRIES` of 2,
  `A733_NOTIFY_INDEX_POWER` of 1, and a constant naming each platform's
  scheduled cores, from which `A733_PMIC_SERVICE_CORES` is derived.

## Performance budgets

Baselines are provisional until the first implementation commit records the
parent commit's counts with their input identities. Instruction and line counts come from `checks/rail-leases.py`
(clang `-O2 -mcpu=cortex-a55`, Unicorn, 42 clock and 16 rail leases, then
126 of each). Bus, spin, busy-time, tick, wake, barrier and lock counts come
from `checks/a733-pmic-polls.c`, `checks/power-domains-pck.c` and
`checks/cpu-clock-model.c` in the board, GPU, NPU, HDMI, SMP and single-core
configurations; busy time is the sum of modelled spin intervals.

| Operation | Provisional baseline | Target |
| --- | --- | --- |
| Rail status, one-rail token | 536 / 1,554 instructions; 60 / 233 lines | ≤ 60 instructions, ≤ 3 lines |
| Clock token status | 243 / 600 instructions | ≤ 50 instructions, no scan |
| Acquire, poll and release | 4,026 / 7,773 instructions | ≤ 450 instructions, no bus access |
| Authority check per PMIC transaction | 818 / 2,179 instructions | 1 acquire load, ≤ 6 instructions |
| Provider object acquisitions per request | One per phase poll | 2, none during the transfer |
| Counter-clock lock acquisitions per snapshot; write; PCK600 transition; CPU settle | One per poll | 2; 2; 1; 1, independent of spin length |
| `DSB SY` per PMIC transaction | Counted at the parent | 7 per read, 5 per write |
| Setup reads per request | About 50 | 3, plus 3 after each STOP |
| Snapshot, submit to requester wake | Up to one tick per step | 4 transactions, ≤ 0.45 ms busy, 0 ticks |
| Selector write, submit to requester wake | Up to one tick per step | 6 transactions, ≤ 0.65 ms busy, 0 ticks |
| Supply verification | Six drivers, each on use | 16 transactions, about 1.8 ms, once per boot |
| Row request, first or later | GPU: 27 polls and one supply read | 0 PMIC requests, 1 acquire load |
| Barriers per row request; per transition | About 10 `DSB SY`; 2 per status poll | 0; 1 after the command, plus the NPU's 1 after its CCU writes |
| Slow PCK600 acknowledgement | One tick per poll | ≤ 1 tick per retry; FAILED by 10 ms plus 1 tick |
| Domain 5 power-off | GPU: 8 polls | 1 step |
| Service task idle wakes per second | One per loop iteration in each image | 0 |
| BUSY results after a publication wake, single core | Not modelled | 0 |
| 200 µs USB0 burst on core 0 during a PMIC snapshot and write, single core and SMP with the snapshot on core 2 | Not modelled | Every phase deadline holds; ≤ 2 counter-clock acquisitions by the service task per request |
| Ledger storage | 20,096 B | ≤ 1,632 B (static assertion) |

## Model cases

- `checks/rail-leases.c`: acquire, poll and release in production order;
  READY on the first poll for all fifteen rails; a full table returns BUSY.
- `checks/a733-pmic.c`: submit before READY returns BUSY; a fault raised on
  another core during startup stays latched after READY; a registry fault
  during a write faults the provider before the selector-write byte; a fault
  during a spin stops all later TWI access.
- `checks/a733-pmic-polls.c`: on one core, a priority-5 requester woken by
  the publication notify reaps within the deadline with no BUSY; a
  higher-priority requester that preempts the service task during
  activation blocks one tick, and the request completes; no spin makes a
  counter-clock call; the single-core and SMP USB0 burst rows.
- `checks/power-domains-pck.c`: boot verification issues 16 transactions
  and no row request issues a PMIC read; a mismatched DCDC4 refuses the GPU
  row; incoherent domains 4 and 1 are refused before any NPU or framebuffer
  reset or clock write; the domain-4 write order, including the `DSB SY`
  after the CCU writes, matches `checks/npu-platform.c`; a DisplayPort
  request that retains domain 10 sets its fault bit, and a check with the
  HDMI mask then fails; an open GPU gate refuses power-off; a second
  requester observes FAILED within the bound.
- `checks/cpu-clock-model.c`: the target never falls below the baseline.

## Board evidence that would confirm the design

- The S_TWI0 SCL rate for divisor `0x28`; the packet engine's register
  behavior, and whether it reads `0x10` and `0x11` in one transaction.
- DCDC3 and DCDC5 readbacks after a raise and a lowering, with
  request-to-readback times and the longest spins.
- Unchanged DCDC2–5 selectors and PMIC page 0 after PSCI CPU_ON, and a bus
  capture showing no other S_TWI0 master.
- PCK600 time to ON for domains 1, 4, 5 and 10 and to OFF for domain 5.
- USB0 and audio latency with PMIC requests running, in an SMP and a
  single-core image.
- PMU cycles for rail and clock status on an A55 and an A76.
- A schematic source for the DE_SYS, G2D and deinterlace supply.

## Implemented state and measurements

Implemented:

- the state word in the rail ledger, with the provider READY bit and the
  provider, registry and ledger fault bits;
- the fifteen-output ledger with direct-slot tokens, borrow pins and no
  bridges;
- clock token status that compares one slot;
- the provider's state-word check between bus phases;
- PMIC time reads from the raw counter in the native adapter, with no
  counter-clock lock, and one kernel mutex-holder query per I2C bus
  acquisition;
- the CPU-rail floor, recorded before the snapshot request is reaped;
- `checks/pmic-tree.py`, `checks/rail-leases-cost.py` and
  `checks/a733-pmic-cost.c`.

Baselines come from the merge base `d40ca4b72b42348880d89f1ebc4eaec61704b8d0`
built from `git archive` output. Instruction and line counts come from
`checks/rail-leases-cost.py`, which compiles with the production flags
(`-march=armv8-a -mstrict-align -O2`, tuned for the Cortex-A55; the design's
`-mcpu=cortex-a55` would emit LSE atomics the images do not use) and excludes
the measuring wrappers. PMIC counts come from `checks/a733-pmic-cost.c` with the
model timing of `checks/a733-pmic-polls.c`; the merge-base PMIC counts come
from a harness that wraps the same provider operations. A register access in
the merge-base adapter queried the kernel's mutex holder, which enters a kernel
critical section in this FreeRTOS SMP kernel, so the merge-base query count
equals its register accesses.

| Operation | Merge base | Now | Target |
| --- | --- | --- | --- |
| Rail status, one-rail token, 16 / full holders | 222 / 888 instructions; 34 / 256 lines | 37 / 37; 3 / 3 lines | ≤ 60, ≤ 3 lines |
| Clock token status, 42 / 126 leases | 235 / 592 instructions | 45 / 45 | ≤ 50, no scan |
| Acquire, poll and release | 1,643 / 3,565 instructions | 335 / 335, no bus access | ≤ 450 |
| Authority check per PMIC transaction | 498 / 1,507 instructions | 6, one acquire load | ≤ 6 |
| Time reads per snapshot; per selector write | 590; 732 | 469; 599 | — |
| Counter-clock lock acquisitions per snapshot; per selector write | 590; 732, one per time read | 0; 0, raw counter reads | ≤ 2; ≤ 2 |
| Kernel mutex-holder queries per snapshot; per selector write | 250; 309 | 1; 1, at bus entry | 1 per request |
| Register accesses per snapshot; per selector write | 250; 309 | 211; 270 | — |
| Setup reads per snapshot | 64 | 25 | 3, plus 3 after each STOP (21) |
| `DSB SY` per snapshot | 500, two per register access | 422, two per register access | 7 per read transaction, 5 per write, after board qualification |
| Longest poll; BUSY per snapshot (model) | 58 µs; 591 µs | 17 µs; 470 µs | — |
| Polls per snapshot (model) | 44 | 44 | 0 ticks with the service task |
| Ledger storage | 20,096 B | 1,600 B | ≤ 1,632 B |
| Image `.bss`, board / HDMI / storage maintenance | 5,502,352 / 465,376 / 541,776 B | 5,481,872 / 446,944 / 523,344 B | — |

Differences from the design, each with its reason:

- `a733_pmic_live` takes no lock: it compares the immutable lifetime and
  makes one acquire load of the state word, so it never returns BUSY
  ([power-domains.md](power-domains.md)).
- Request admission rereads the clock route and divisor (four or five reads),
  because `checks/a733-clock-reference-pmic.c` requires drift detection there.
- Exclusive tokens remain accepted on all fifteen outputs as pure exclusion;
  `checks/a733-radio-platform.c` expects BUSY, not INVALID, for an exclusive
  DCDC8 request. DC1SW1 is accepted as a DCDC1 alias with DC1SW2.
- The CPU floor lives in the ledger (`a733_rail_floor`), because
  `include/a733_cpu_clock.h` is outside this package.
- The native adapter's time read returns the raw counter in microseconds
  (`ISB; MRS CNTPCT_EL0` divided by the ticks per microsecond) instead of
  calling the counter clock once per poll and comparing raw ticks in between.
  It gives the same microseconds as the counter clock with no lock, so the
  provider and `struct a733_pmic_ops` stay unchanged: checks outside this
  package build that structure with positional initializers, which a new
  field would break under `-Wmissing-field-initializers`. The provider's own
  monotonic check faults on a counter that runs backwards, the time read
  refuses after a fault latched in the shared counter clock, and the provider
  has no spin for an iteration cap to bound. `checks/pmic-tree.py` fails if the
  time read calls the counter clock.
- The native adapter checks mutex ownership once per bus entry instead of once
  per poll. A FreeRTOS mutex changes holder only when its holder gives it, and
  only the adapter's leave gives it on the provider task, so a flag set at
  entry is exact until leave.
- The native adapter keeps `DSB SY` before and after each register access.
  One barrier per control write is unqualified until a board run completes
  snapshots and selector writes with identical results under both placements;
  `docs/shared-helpers.md` requires that qualification for a weaker barrier.
- The DE_SYS provider `a733_de_sys_power.c` is deleted; the power-domain
  owner has no domain-9 or domain-10 entry.

Not implemented, with the dependency that blocks each:

- The NPU on its domain-4 row and removal of the NPU startup's direct S_TWI0
  path, which the NPU startup and clock-handoff models outside this package
  still use ([docs/power-domains.md](../power-domains.md) lists the NPU
  owner's changes).
- Domain 10 (VO1) rows for HDMI and DisplayPort, and the CLDO2 and CLDO5
  verified bits they need.
- The settle spin and core placement for the CPU clock driver, which has no
  production caller.
