# Design: one owner for supply rails and one for power domains

Status: **design**, not implemented; physical behavior stays unqualified
until the board evidence at the end exists. Section numbers refer to
`docs/performance-architecture.md`.

Today the clock registry holds AXP8191 output DCDC2, the GPU resource manager
holds DCDC4, and the rail ledger reaches both through bridges. Five drivers
repeat the PCK600 power-domain sequence, and six read the same supplies from
the PMIC.

## The rail ledger owns fifteen rails; the CPU clock driver writes two

The ledger keeps a fixed table of the fifteen outputs that production code
acquires: DCDC1–5, DCDC8, DCDC9, BLDO1, BLDO2, BLDO5, CLDO1, CLDO2, CLDO3,
CLDO5 and ELDO6. DC1SW2 stays an alias of DCDC1; other outputs are refused.
A **holder slot** stores the token identifier, owner, rail masks and one
**pin count**. A full slot table returns BUSY, as today, and callers such as
`a733_pcie.c` and the MMC platform retry on it; EXHAUSTED still means only
that identifiers ran out.

`a733_rail_acquire` keeps its signature, token layout and PENDING result.
`a733_rail_poll` returns READY on its first call for all fifteen rails, with
no bus access. The framebuffer, GPU and NPU acquire their own tokens instead
of reaching DCDC2 or DCDC4 through the clock registry, the GPU manager or
`a733_pmic_consumer.c`. The NPU startup's direct S_TWI0 read path is removed;
`npu-convolution`, the only image that runs NPU startup, binds the provider.

**DCDC3 and DCDC5 are written only by the CPU clock driver**
(`drivers/clock/a733_cpu_clock.c`), which holds DCDC5 for the little cluster
and the DSU and DCDC3 for the big cluster. Only these two rails accept an
exclusive mask, and each admits one holder. The scope requires a voltage
floor per consumer; with one exclusive holder, that floor is the driver's own
target, so the floor stays in the driver:

- The driver records each cluster's boot baseline from its first selector
  read taken no earlier than provider start plus 5,160 µs (the full-range
  ramp and enable delay in the device tree), in its own struct beside
  `from_uv`, `selector` and the settle deadline it already keeps.
- The target voltage is the larger of the baseline and the operating point.
  The existing `a733_pmic_submit_write` path, with the expected-selector
  check, stays the only writer. A refused or expired write changes nothing,
  so a failed lowering keeps the higher voltage.
- Before each PLL or mux write the driver calls the lock-free
  `a733_pmic_live`, which loads the provider state word, the clock registry
  failed word and the ledger fault word, and writes nothing if any is set.
- The settle wait blocks for whole ticks until 300 µs before the deadline,
  then spins on the counter with no lock held. The window covers the
  800→870 mV step at 250 µV/µs, which needs at most 280 µs.

The driver has no production caller. A ledger-held floor record is added
only when a second consumer of DCDC3 or DCDC5, or a governor that reads the
floor, exists.

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
26 speed bins (`checks/npu-platform-reference.py`); the NPU supply selection
is therefore the DCDC2 = 800 mV check in its row.

- **Read-only check before consumer writes.** `a733_power_domain_check`
  runs the table's domain-level reads and the adoption-state check and
  writes nothing. Each consumer calls it where it reads today, before its
  first reset or clock write: the NPU in `baseline()`, before `0xb04` and
  `0xb00`; the framebuffer in `cold_step` case 0, before `0xa74` and
  `0xa04`; the GPU, deinterlace, G2D, HDMI and DisplayPort at their current
  CHECK rows. The consumers' own copies of these reads are removed. The row
  request repeats the reads, because another row's request or release can
  change the domain between the check and the request.
- **Request.** A consumer passes its rail token, checked once. The first
  time a row's supply is needed after boot, the owner submits one read
  sequence under that token, outside its own lock, and keeps the request
  handle. Any requester's step for that domain reaps it; an EXPIRED result
  or a mismatch clears the in-flight read so the next requester resubmits.
  The read waits until provider start plus 5,160 µs, so a ramp left by the
  bootloader cannot pass. A matching read sets a per-rail verified bit;
  later requests, including each new GPU session, cost one acquire load of
  the ledger fault word. A set fault word or a domain retention clears every
  verified bit.
- **Per-boot verification is provisional.** It relies on no agent writing
  DCDC2, DCDC4, CLDO2 or CLDO5 after boot. The OS issues no PSCI CPU_OFF or
  suspend call, and `checks/pmic-*` fails if a PSCI CPU_OFF or suspend
  function identifier appears in the tree; adding one must also clear the
  verified bits. Firmware writes and a second S_TWI0 master stay open until
  the board evidence below exists.
- **Pinning.** A counted row and a PMIC borrow each raise the slot's pin
  count. Rail release returns BUSY while it is nonzero.
- **Consumer reads that stay.** Reads whose expected value depends on the
  block or the session stay in the consumer: the GPU module clock and reset
  idle check (`0x0b20` bit 31, `0x0b24` bits 0 and 16); the framebuffer's
  DE clock-off check against inherited scanout DMA (`0xa00` bit 31, `0xa04`
  bit 0); HDMI's exclusion of the TV, eDP, HDCP and CEC producers; the NPU
  baseline of `0xb00` and `0xb04`; and the G2D and deinterlace IOMMU gates.
- **Domain 4 keeps the pinned NPU order.** After the check, the NPU
  deasserts the AHB, AXI and core resets (`0xb04` bits 18, 17, 16), sets the
  module rate, enables the bus and module clocks, and only then requests the
  domain, as the pinned VIPLite driver (`vip_drv_device_platform.c`) does.
  The owner writes the delay row and the ON command. The NPU keeps its
  2,000 µs post-ON settle and readback. Domains 1, 5 and 10 power on before
  their consumers enable clocks, as today.
- **Power-off only for domain 5,** whose one bus master is the GPU. At count
  zero, after the GPU drains its DMA, the owner rereads `0x0b20` bit 31 and
  `0x0b24` bits 0 and 16; if any is set, release returns RETAINED.
  Domains 1, 4 and 10 have several masters each and are never turned off.
- **Boot adoption.** Command and status both OFF give OFF; both ON give ON
  with zero rows and no command written, and domain 5 also needs its gate
  and reset bits clear. Anything else, such as command ON with status OFF,
  gives FAILED.
- **Transitions.** A domain is OFF, ON_WAIT, ON, OFF_WAIT or FAILED, with a
  deadline 10 ms after its command. A requester that finds a wait state gets
  PENDING. Every requester's step runs the same poll: it reads the state
  under the owner lock and spins on status with no lock held. It then
  retakes the lock and publishes ON, OFF or FAILED only if the state is
  still the wait state it read. The owner writes a command only from OFF or
  ON, and only domain 5, through its single GPU row, is ever turned off, so
  no newer command can arrive during a poll and no sequence number is
  needed. FAILED is published once the deadline passes, so a parked
  initiator cannot hold a domain in a wait state.
- **PCK600 acknowledgement is polled.** The pinned device tree node
  `pck-600@7060000` in `sun60iw2p1.dtsi` has no `interrupts` property, and
  `pck600_domains.c` polls status for up to 10,000 µs. The owner spins up to
  a provisional 50 µs, then returns RETRY_TICK; board evidence replaces the
  bound with the longest measured acknowledgement plus 50%.
- **Barriers (section 1.2).** Domain-level reads, command and status reads
  and status polls are plain reads: Device-nGnRnE accesses to one
  peripheral stay in order, and the command write depends on the checked
  read values through a branch, so it cannot issue before them. The delay
  row and the command write are plain writes followed by one `DSB SY`,
  because the spin bound is timed from the command reaching the PPU. The
  move off `a733_mmio_*_dsb_sy_both` waits on the qualification in
  `docs/shared-helpers.md`.

## One PMIC service task wakes requesters

`a733_pmic_service`, called by every image that binds the provider, replaces
the board and OS loop and the loops in camera, gpu-display, gpu-render,
gpu-finite, hdmi, i2s, mimus, npu-convolution, npu-returning,
thermal-returning, usb-returning and storage-update. It runs at priority 3,
below the USB0, audio, headset and thermal tasks.

- **Placement.** The affinity mask `A733_PMIC_SERVICE_CORES` is A55 cores
  2–5 in images whose scheduler starts those cores, which keeps the byte
  spins off core 0, where USB0 and audio run. In single-core images, such as
  npu-convolution-single-core, gpu-render-single-core and board-single-core,
  it is core 0. `checks/pmic-*` fails if the mask is not a subset of the
  image's scheduled cores. Any task that polls the CPU clock driver uses the
  same mask, so its 300 µs settle spin never runs on A76 cores 6–7.
- **Work in.** Every enqueue, and every startup stage that waits on no
  hardware, notifies the service task. The task blocks, up to 250 ms, only
  when nothing is queued.
- **Completion out.** Submit records `xTaskGetCurrentTaskHandle()` in the
  request slot. The service task notifies that handle at publication, while
  it still holds the provider object; reap and cancel clear the handle under
  the object. A reap therefore cannot succeed until the notify has returned,
  and a task reaps or cancels before deletion, as it must already do to
  release its rail token. Reap checks the request identifier and borrow, not
  the calling task, so the domain owner can reap for any requester.
  Requesters wait with `ulTaskNotifyTakeIndexed` on
  `A733_NOTIFY_INDEX_POWER`, so a PMIC wait cannot consume an interrupt wake
  on index 0. A BUSY from result or reap is retried at once, without a wait.
- **Refusals.** Submit returns BUSY with no request ID while the provider is
  starting, so PENDING always means a queued request with its ID written.
  Every production caller already retries on BUSY.

A snapshot is four single-register transactions (page, enable, selector,
page); a write adds a selector write and a readback. The pinned AXP8191
driver reads one register per transaction, and no pinned source shows
register auto-increment. Chip identity is read once, at verify. Per request
the provider rereads only state another agent can change: the S_TWI0 gate
and reset word, the divisor, and the three bus-idle registers, which it
rereads after every STOP. The S_TWI0 interrupt is GIC-600 INTID 235: the
pinned `board-kernel/sun60iw2p1.dtsi` (SHA-256 `1c7313f6…`) gives node
`twi@7083000` `interrupts = <GIC_SPI 203 …>`, and SPI 203 is INTID 235.
`checks/pmic-*` fails if any source other than the provider names INTID 235
or SPI 203; no runtime GIC read is made.

## Bus waits spin to the expected completion, then wait ticks

- **One bus rate.** Verify admits divisor `0x28` only with a 24 MHz parent,
  giving 400 kHz and a 22.5 µs byte time. The tick-per-phase path for slower
  rates is removed.
- **Two object acquisitions per request.** The service task takes the
  provider object once at activation (QUEUED to ACTIVE), together with
  I2C bus-mutex entry and the admission checks (`clock_live`, `owned()` and
  the borrow status), and once at publication (DONE or EXPIRED,
  release-stored). The bus mutex, held from START to STOP, excludes every
  other bus user; phase, byte index and the spin's time value are private to
  the service task. Submit, result and reap on other cores touch other slot
  fields under the object.
- **One atomic latch for provider failure.** The provider state word
  (STARTING, READY, RETAINED) replaces `p->failed`. `fault()` sets RETAINED
  with `atomic_fetch_or`, so a fault on any core needs no object lock; a
  fault on a core other than the service task's does not touch the bus
  mutex. `p->last_now` is written only under the object; the spin uses a
  task-local time value that publication folds in with
  `a733_monotonic_advance`.
- **Revocation inside a transfer.** Before each transaction and before the
  selector-write byte, the task makes one acquire load each of the provider
  state word, the clock registry failed word and the ledger fault word
  (section 1.1). If any is set, it calls `fault()` in its own context, which
  retains the I2C mutex with the transaction incomplete, and makes no
  further register access.
- **Phases.** The task spins to the expected completion (9 bit times per
  byte), then rereads control once per byte time for up to 90 µs, then once
  per tick until the deadline. An iteration cap faults if the counter stops.
- **`a733_pmic_live` takes no lock.** It compares the caller's lifetime with
  the provider's start-time fields, then makes the same three acquire loads.
  An MMC step on another core never sees BUSY from a PMIC request.
- **Barriers.** A `DSB SY` follows the write that begins a timed phase;
  polled reads have none.
- **No TWI interrupt yet.** Byte interrupts would cost 34 per write and a
  route in every image. A spun write costs about 0.95 ms of A55 time
  including settle; at 10 CPU voltage changes per second that is about 1% of
  one core. INTID 235 replaces the byte spin when a measured voltage-change
  rate exceeds 10 per second.

## HDMI register access drops its try-locks

`a733_hdmi_phy.c` calls `ops.validate` before every `a733_hdmi_read` and
`a733_hdmi_write`, and any result other than 1 retains every HDMI resource.
`hdmi_native.c` then calls `tokens()` again in `live()` and `lock()`. This
package changes `hdmi_native.c`; `a733_hdmi_phy.c` is unchanged.

- `tokens()` runs only at sequence admission, in the acquire step, where a
  BUSY already returns 0 and the caller retries.
- `validate()` keeps the request and grant equality, the pixel-source check
  and `timely()`, and replaces `live()` with four acquire loads: the
  lock-free `a733_pmic_live` (three words) and the GPU resource manager's
  failed word, which becomes an atomic latch. `read32`, `write32`, `read8`
  and `write8` drop `live()`, and `lock()` drops `tokens()`.
- HDMI's clock, rail and PCK leases change only through HDMI's own calls or
  through one of those latches. The GPU manager's per-lease RETAINED path
  writes only the calling client's lease, so it cannot reach HDMI's index; a
  model case asserts this.

The remaining per-access costs, one register-mutex pair, the counter reads
and two `DSB SY`, belong to the HDMI sequence-batching follow-up (sections
1.1 and 2.5), owned by the HDMI driver package.

## Memory placement and synchronization

All objects live in kernel `.bss` (Normal Write-Back, Inner Shareable). No
DMA master reads them, so no cache maintenance applies. Each word that
another core reads on a per-access path has its own 64-byte line, checked by
a static assertion in its owning file.

| Region | Size | Written by |
| --- | --- | --- |
| Ledger lock word and free bitmap | 64 B | Acquire, release |
| Ledger fault word | 64 B | `atomic_fetch_or` only |
| Holder slots, 32 B × 32 | 1,024 B | That slot's acquire, release and pin count |
| Borrows, 48 B × 10 | 480 B | Provider submit and reap; camera |
| Provider state word | 64 B | `atomic_fetch_or`; READY release-stored at start |
| Clock registry failed word | 64 B | `atomic_fetch_or` only |
| GPU resource manager failed word | 64 B | `atomic_fetch_or` only (file outside this package) |

The ledger totals 1,632 bytes, down from 20,096. Thirty-two slots cover the
31 current holders; ten borrows cover eight request slots, the camera's
lifetime borrow and one spare. Two holder slots share a line; pin counts
change at session-start rate.

Acquire, release, submit, reap and admission status take one nonblocking
try-lock per object and return BUSY on contention; per-access paths take
none. A token identifier holds its slot index in the low bits (5 for the
ledger, 7 for the clock registry) and a never-reused generation above them,
so status compares one slot instead of scanning. The domain owner releases
its lock before it calls the ledger or provider or spins on PCK600.

A follow-up outside this package moves `live()` in `a733_mmc_platform.c`,
`healthy()` in `a733_twi_native.c` and `leases_live()` in
`a733_camera_native.c` to one status check at admission plus the latch
loads (section 2.4).

## Reused, removed and changed elsewhere

- **Reused:** the ledger API and retention; PMIC transactions and
  `a733_pmic_submit_write` with its expected-selector check; the CPU clock
  driver's voltage path and ramp wait; the DE_SYS PCK600 code and delay row;
  the PCK gate lease.
- **Removed:** clock scope `A733_POWER_DCDC2`; the GPU RAIL resource; ledger
  bridges and OPENING; `a733_pmic_consumer.c`; four duplicate PCK
  sequences; the consumers' copies of the domain-level reads; the domain-9
  path; six supply reads and every repeat of them; the NPU direct S_TWI0
  path; the tick-per-phase path; twelve per-image PMIC loops; the separate
  borrow count; about 44 of 50 setup reads per request; the 128-slot status
  scan; HDMI's per-access try-locks.
- **Changed outside this package:** GPU resource manager and platform, NPU
  platform, G2D, deinterlace, framebuffer, `hdmi_native.c`, DisplayPort,
  DSI scanout, the image PMIC loops, and checks that build a ledger.
  `include/a733_power_domains.h` needs the coordinator's approval.
- **Needed from the primary agent:** `configTASK_NOTIFICATION_ARRAY_ENTRIES`
  of 2 and `A733_NOTIFY_INDEX_POWER` of 1 in the platform FreeRTOS
  configuration, and a constant naming the cores each platform's scheduler
  starts, from which `A733_PMIC_SERVICE_CORES` is derived.

## Performance budgets

The first implementation commit adds the counters and records the parent
commit's counts with their input identities; baselines are provisional until
then. Instruction and line counts come from a step in
`checks/rail-leases.py` that runs status and acquire, built with clang
`-O2 -mcpu=cortex-a55`, in Unicorn with 42 clock and 16 rail leases, then
126 of each. Bus, spin, busy-time, tick, wake and lock counts come from
`checks/a733-pmic-polls.c` and `checks/power-domains-pck.c` in the board,
GPU, NPU, HDMI and single-core configurations; busy time is the sum of
modelled spin intervals. HDMI counts come from the HDMI model case driving
`a733_hdmi_read` and `a733_hdmi_write`.

| Operation | Provisional baseline | Target |
| --- | --- | --- |
| Rail status, one-rail token | 536 / 1,554 instructions; 60 / 233 lines | ≤ 60 instructions, ≤ 3 lines |
| Clock token status | 243 / 600 instructions | ≤ 50 instructions, no scan |
| Acquire, poll and release | 4,026 / 7,773 instructions | ≤ 450 instructions, no bus access |
| Authority check per PMIC transaction | 818 / 2,179 instructions per phase | 3 acquire loads, ≤ 12 instructions |
| Provider object acquisitions per request | One per phase poll, held up to 8 phases | 2, none during the transfer |
| `a733_pmic_live` BUSY on another core during a snapshot | Up to 8 phases | 0 |
| HDMI 32-bit register read | 11 try-locks, 1 mutex pair, 10 counter reads, 9 task-context checks, 2 `DSB SY` (counted) | 0 try-locks and 4 acquire loads; the rest unchanged |
| Setup reads per request | About 50 | 5, plus 3 after each STOP |
| Ledger storage | 20,096 B | ≤ 1,632 B (static assertion) |
| Snapshot, submit to requester wake | Up to one tick per step | 4 transactions, ≤ 0.45 ms, 0 ticks; ≤ 0.45 ms busy |
| First domain power-on with supply read | GPU: 27 polls | ≤ 0.7 ms, 1 wake, 0 ticks within the spin bound |
| Later row request, supply verified | One supply read | 0 PMIC requests, 1 acquire load |
| Barriers per row request; per transition | About 10 `DSB SY`; 2 per status poll | 0; 1 after the command write (after qualification) |
| Slow PCK600 acknowledgement | One tick per poll | ≤ 1 tick per retry; FAILED by 10 ms plus 1 tick |
| Domain 5 power-off | GPU: 8 polls | 1 step |
| Single-core image: 200 µs USB0 burst during a PMIC snapshot and write on core 0 | Not modelled | Every phase deadline holds; added USB0 delay ≤ the provider's longest interrupts-masked section, one counter-clock read |

## Model cases

- `checks/rail-leases.c`: production-style acquire, poll and release; poll
  returns READY on the first call for all fifteen rails; a full slot table
  returns BUSY.
- `checks/a733-pmic.c`: submit before READY returns BUSY with no ID; a
  registry fault latched during a write faults the provider before the
  selector-write byte; another core faults the provider during a spin of a
  write, and no later TWI access occurs; a requester that reaps and deletes
  itself on another core does so only after the notify returns.
- `checks/a733-pmic-polls.c`: a concurrent live, submit or reap caller sees
  no BUSY from `a733_pmic_live`; the object is free during every spin; two
  acquisitions per request; the single-core USB0 burst row.
- `checks/power-domains-pck.c`: an incoherent domain 4 is refused before any
  `0xb04` or `0xb00` write, and an incoherent domain 1 before any `0xa74` or
  `0xa04` write; each domain-level entry blocks the row in OFF, adopted ON
  and ON with rows; the domain-4 write order matches the
  `checks/npu-platform.c` trace; a second GPU session issues no supply read;
  a second requester reaps the supply read within the PMIC deadline when the
  initiator never returns, and resubmits after EXPIRED; a poll that finds
  the domain out of its wait state publishes nothing; an open GPU gate
  refuses power-off; a second requester observes FAILED within the bound.
- `checks/cpu-clock-model.c`: the target never falls below the baseline; a
  fault between READY and the PLL write prevents the write.
- HDMI model case: an admission BUSY retains nothing; a register access
  takes no try-lock; a latch set during a sequence fails and retains; a
  failed GPU-resource acquire by another client leaves HDMI's lease OK.

## Failure classes (section 1.8)

- **Refused before any bus access** (BUSY, EXPIRED, INVALID, EXHAUSTED):
  nothing changed or retained.
- **Supply mismatch or failed domain-level read:** the row is not counted
  and no ON command is written.
- **Unknown state:** a bus fault, readback mismatch, changed selector, set
  latch or expiry after a write's first transaction retains the provider,
  the I2C mutex and the active borrow. An unexpected PCK600 status, or a
  passed transition deadline, retains the domain, its rows, their tokens and
  the PCK lease.

## Not included, with reasons

- **DCDC2 and DCDC4 writes:** no NPU point at 492 MHz needs more than 800 mV
  and no GPU operating-point table exists.
- **Lowering below the baseline:** it needs every consumer of each rail.
- **A ledger floor record:** DCDC3 and DCDC5 each have one exclusive holder
  and no other reader.
- **Per-domain waiter lists:** a wait state occurs once per boot for domains
  1, 4 and 10, and domain 5 has one row.
- **Lock-free rail and clock token status:** after this change no per-access
  caller remains in this package's files.
- **PMIC burst reads:** no pinned source shows register auto-increment.
- **Power-off for domains 1, 4 and 10:** it needs drain evidence for every
  master, and no caller needs it.
- **CPU cluster and core domains** (reserved code) and **domains 0, 2, 3
  and 6–9** (current owners stay).

## Board evidence that would confirm the design

- The measured S_TWI0 SCL rate for divisor `0x28`, and whether a two-byte
  read from `0x10` returns registers `0x10` and `0x11`.
- DCDC3 and DCDC5 readbacks after a raise and a lowering, with
  request-to-readback times and the longest spins.
- Unchanged DCDC2–5 selectors and PMIC page 0 after PSCI CPU_ON, and a bus
  capture showing no other S_TWI0 master; until then per-boot supply
  verification stays provisional.
- PCK600 time to ON for domains 1, 4, 5 and 10 and to OFF for domain 5
  across repeated cycles, which sets the spin bound.
- USB0 and OS audio latency with PMIC requests running, in an SMP and a
  single-core image.
- PMU cycles for rail and clock status on an A55 and an A76.
- A schematic source for the DE_SYS, G2D and deinterlace supply.
