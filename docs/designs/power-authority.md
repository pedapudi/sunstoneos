# Design: one owner for supply rails and one for power domains

Status: **design**, not implemented; physical behavior stays unqualified
until the board evidence at the end exists. Section numbers refer to
`docs/performance-architecture.md`.

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

## The DE_SYS provider becomes the domain owner

`drivers/power/a733_de_sys_power.c`, renamed `a733_power_domains.c`, owns
domains 1, 4, 5 and 10. Its per-consumer ops, `authorize` callback and
unused domain-9 path are removed. Domains 0, 2, 3 and 6–9 and the CPU
cluster and core domains keep their current owners.

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

- **Supply verification once per boot.** The service task waits until
  provider start plus 5,160 µs (the full-range ramp and enable delay in the
  device tree), then reads the enable and selector of DCDC2, DCDC4, CLDO2
  and CLDO5: 16 transactions, about 1.8 ms. Each rail at its expected value
  (800 mV for DCDC2 and DCDC4, 1,800 mV for CLDO2 and CLDO5) sets its
  verified bit, and then the done bit is set. A row request passes its rail
  token, checked once, and makes one acquire load of the state word. It
  returns PENDING before the done bit, and is refused if a required verified
  bit is clear or a relevant fault bit is set. No row request submits a
  PMIC read. The scheme relies on no agent writing those four rails after
  boot: the OS issues no PSCI CPU_OFF or suspend call, and `checks/pmic-*`
  fails if such a function identifier appears in the tree. Firmware writes
  and a second S_TWI0 master stay open until the board evidence exists.
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
  ON give ON with zero rows, and domain 5 also needs its gate and reset bits
  clear; anything else gives FAILED. A domain is OFF, ON_WAIT, ON, OFF_WAIT
  or FAILED, with a deadline 10 ms after its command. Any requester's step
  reads the state under the owner lock, spins on status with the lock
  released, and publishes ON, OFF or FAILED only if the state is still the
  one it read. Commands are written only from OFF or ON, so no newer command
  can arrive during a poll.
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
  with interrupts masked (section 1.7), so no spin calls it. The provider
  calls it once at activation and once at publication, converts the
  deadline and each phase's expected completion to `CNTPCT_EL0` targets,
  and spins on `ISB; MRS CNTPCT_EL0` through a raw-tick op in
  `a733_pmic_native.c` that the checks model. Each caller keeps its own
  `a733_monotonic_advance` check, and an iteration cap faults if the counter
  stops. The PCK600 and CPU settle spins use the same op, with one
  counter-clock call at their start.
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
