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
A **holder slot** stores the token identifier, owner, rail mask and one
**pin count**. When no slot is free, acquire returns EXHAUSTED and changes
nothing.

`a733_rail_acquire` keeps its signature, token layout and results. It still
returns PENDING for every rail, because many production callers treat any
other result as a failure (for example `a733_mmc_platform.c`, `a733_pcie.c`,
`typec_native.c` and `apps/spi/run.c`). For the thirteen rails other than
DCDC3 and DCDC5, the first poll or status returns READY with no bus access;
those rails are admitted by mask alone and keep no count. A new case in
`checks/rail-leases.c` drives a production-style caller through acquire,
poll and release.

Each consumer holds one token. The display framebuffer, GPU and NPU, which
today reach DCDC2 or DCDC4 through the clock registry, the GPU manager or
`a733_pmic_consumer.c`, call `a733_rail_acquire` themselves. The NPU
startup's direct S_TWI0 read path is removed: `npu-convolution`, the only
image that runs NPU startup, already binds the PMIC provider.

**DCDC3 and DCDC5 are written.** The CPU clock driver holds DCDC5 for the
little cluster and DCDC3 for the big cluster and is the one floor-setter for
each. The little-cluster floor includes the DSU, which shares DCDC5 at a
fixed rate. Each rail has one 64-byte **rail line** holding a holder count,
the baseline (the voltage read by the snapshot that the first holder
triggers), the settle deadline and one 64-bit **status word**: state,
published voltage, floor, an in-flight flag and a baseline-valid flag. A
disabled or undecodable output is refused, because nothing here enables a
rail.

- **Target.** The larger of the baseline and the floor. A floor change
  moves the target when the new target differs from the published voltage.
- **Floor change.** Under the ledger lock the call validates the holder and
  CPU grant and returns BUSY if the in-flight flag is set. If the target
  does not move, it release-stores a status word with the new floor and
  returns. Otherwise it release-stores the word with the flag set and the
  floor unchanged, then submits a write that carries the requested floor.
- **Single writer.** While the flag is set, only the PMIC task writes the
  status word; while it is clear, only the floor-setter does, under the
  ledger lock. The first holder's acquire sets the flag when it submits the
  baseline snapshot. Publication therefore takes no lock and cannot be
  refused.
- **Publication.** The PMIC task stores the settle deadline, then
  release-stores one status word that holds the voltage decoded from the
  readback and the requested floor, with the flag clear.
- **Refusal before the first transaction.** A synchronous PENDING, BUSY or
  RETAINED from the submit makes the caller release-store the word with the
  flag clear; a write found expired while queued makes the PMIC task do the
  same. The floor is unchanged in both cases, so a failed lowering leaves
  the higher floor, which is safe. No previous floor is kept.
- **Settle deadline.** STOP time plus |target − previous published voltage|
  at 250 µV/µs, never earlier than the stored deadline. With no previous
  publication, the first snapshot sets it no earlier than provider start
  plus 5,160 µs, the full-range ramp and enable delay in the device tree.
- **Status** is READY when the output is enabled, the baseline is valid, the
  flag is clear, the published voltage is at least the floor and the
  deadline has passed. It also returns the published floor and voltage.
- **Release** of a CPU holder requires a zero floor.

The CPU clock driver raises the floor and waits for READY. Immediately
before the PLL or mux write it checks that the published floor equals the
floor it requested and that the published voltage covers the target
operating point. If either check fails, as after a queued write expired, it
makes no PLL or mux write and reports EXPIRED. It lowers the frequency
before the floor. Its own selector writes and ramp waits are removed. Model
cases:

- one table-driven case in `checks/rail-leases.c` for a submit that returns
  PENDING, BUSY or RETAINED, or expires while queued: floor and voltage
  unchanged, flag clear, rails not UNCERTAIN;
- a case in `checks/cpu-clock-model.c` where a raise expires while queued
  and no PLL or mux write follows; the existing cases (frequency changed
  before voltage, ramp skipped) run against the floor calls.

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

These are the values the consumers check today. The NPU runs only at
492 MHz, where the pinned vendor operating-point table gives 800 mV for all
26 speed bins (`checks/npu-platform-reference.py`).

- **Request.** A consumer passes its rail token. The owner checks once that
  the token is live and covers the row's rails. When a row's count goes from
  zero to one, it submits one read sequence under that token: page `0xff`,
  enable and selector for each rail, page. The six per-consumer supply reads
  are removed. A counted row relies on the earlier read, because nothing in
  the OS writes these four rails.
- **Pinning.** Counting a row and borrowing the token for a PMIC request
  both raise the slot's pin count under the ledger lock. Rail release
  returns BUSY with nothing changed while the count is nonzero; this
  replaces the separate borrow count. A row retained by a RETAINED or FAILED
  domain stays counted, so its consumer keeps the token.
- **Preconditions.** Before the ON command the owner reads a per-domain
  table taken from today's sequences and writes nothing if any entry fails.
  The framebuffer, HDMI and DisplayPort assert their resets and wait 10 µs
  before they request; the GPU's resets stay asserted from reset.
  - Domain 1: MBUS gates `0x05c0` bit 5 and `0x05e0` bit 11 set; `0x0a00`
    bit 31, `0x0a04` bits 0 and 16 and `0x0a74` bit 16 clear.
  - Domain 4: MBUS gates `0x05c0` bit 6 and `0x05e0` bit 18 set. The NPU
    keeps its pinned order, which releases its resets and enables its clock
    before ON, so the table requires `0x0b04` bits 0 and 16–18 and `0x0b00`
    bit 31 set.
  - Domain 5: `0x05c0` bit 7 and `0x05e0` bit 16 set; `0x0b20` bit 31 and
    `0x0b24` bits 0 and 16 clear.
  - Domain 10, the union of the HDMI and DisplayPort checks: bit 31 clear
    at `0x1640`, `0x1680`, `0x1684`, `0x1690` and `0x1694`; bit 0 clear at
    `0x1604`, `0x160c`, `0x164c`, `0x168c` and `0x16cc`; bit 16 clear at
    `0x160c`, `0x164c`, `0x16cc` and `0x16ec`.
- **Power-on.** Supply read checked, preconditions met, the shared PCK600
  bus-clock lease (`RCCU + 0x1ac`) held, domain on. The GPU manager keeps
  that gate, which PCIe, video, ISP and USB2 also lease.
- **Power-off only for domain 5,** whose one bus master is the GPU. When its
  count reaches zero after the GPU has drained its DMA, the owner rereads
  the domain-5 gate and reset bits. If any is set, the domain stays on with
  its rails and PCK lease and the release returns RETAINED. Domains 1, 4 and
  10 have several masters each and are never turned off, as today.
- **Domain 5 found on at boot** with those bits clear is marked ON with a
  count of zero; otherwise it is FAILED and no GPU row is admitted. The
  first GPU request then counts normally without an ON command, and the
  GPU's release at zero powers the domain off.
- **Transitions and waiters.** A domain is OFF, ON_WAIT, ON, OFF_WAIT or
  FAILED. A request on a domain in a wait state records the task in that
  domain's waiter list, sized to its rows (four for domain 1), and returns
  PENDING. When the transition publishes ON, OFF or FAILED, the owner wakes
  every waiter after releasing its lock.
- **PCK600 acknowledgement.** No transition interrupt is documented and the
  vendor driver polls. The owner spins up to 50 µs with no lock held, then
  returns RETRY_TICK; that requester waits one tick and reruns its step,
  bounded by 10 ms. Other waiters stay blocked until publication.

`checks/power-domains-pck.c` gains domains 4, 5 and 10 and these cases:

- an open gate or released reset in each domain's table blocks the ON write;
- an open GPU gate refuses power-off, and domain 5 found on with a gate open
  is FAILED;
- adoption of domain 5, then a GPU request and release, powers it off;
- a DSI-first request on an OFF domain 1 writes no ON command before a
  DCDC2 read;
- releasing every row of domains 1, 4 and 10 writes no OFF command;
- two requesters on one ON_WAIT domain both wake with no extra tick, and a
  slow acknowledgement costs at most one tick per retry.

## One PMIC service loop wakes requesters

`drivers/power` gains one service loop, `a733_pmic_service`, which every
image that binds the provider calls, as images share
`apps/board/usb0_service.h`. It blocks on a task notification, bounded by
250 ms, only when the provider is READY with nothing active or queued; it
yields on BUSY and handles park and stop. It replaces the board and OS loop
and the per-image loops in camera, gpu-display, gpu-render, gpu-finite,
hdmi, i2s, mimus, npu-convolution, npu-returning, thermal-returning,
usb-returning and storage-update.

- **Work in.** Every enqueue calls `kick` after the submitter releases the
  provider try-lock, and startup stages that wait on no hardware (CLOCK,
  BASELINE, PINS, VERIFY) kick when they advance. `kick` is
  `xTaskNotifyGive` to the service task.
- **Completion out.** At submit the provider slot records the requester's
  task handle; after its release store the service task calls
  `xTaskNotifyGive` on that handle, outside its locks. Reap and cancel clear
  the handle. Tasks that submit PMIC or domain requests are never deleted;
  no production task calls `vTaskDelete` today, and `include/a733_pmic.h`
  states the rule. A consumer that receives PENDING blocks in
  `ulTaskNotifyTake` with its deadline as the timeout. A wake carries no
  data, so a notification from another source costs one extra step.
- **CPU-side settle.** After the wake, the clock driver spins on the counter
  only when at most 300 µs remain to the published deadline, which covers
  the 800→870 mV step (at most 280 µs). Otherwise it blocks one tick at a
  time and rechecks. The spin is at most 300 µs, preemptible and holds no
  lock; an 800→1,050 mV raise reaches READY within one tick of its deadline.

The service task runs at priority 3 on A55 cores 2–5, below `board-usb0`,
`mimus-usb` and the OS audio, headset and thermal tasks at 4, which preempt
it at once.

The provider reads chip identity once, in its verify stage. A snapshot is
four transactions (page, enable, selector, page) and a write is six (the
same four, a selector write and a readback). Retention rules:

- a selector read before the write that differs from the published value
  retains the provider without writing, as the CPU clock driver does today;
- a fault sets the failed state, marks DCDC3 and DCDC5 UNCERTAIN, returns
  RETAINED to their holders and keeps the active request's borrow, so its
  token stays pinned;
- after a fault the task issues no bus access other than the STOP of a
  transaction in progress.

`baseline()` today reads about 25 device registers at activation and again
at publication. Per request the provider keeps only the reads whose inputs
another agent can change: the S_TWI0 gate and reset word, the divisor, the
interrupt 235 enable bit and the three bus-idle registers, which it reads
again after every STOP. Clock parents, GPIO function, TWI_DRV registers and
the other GIC state are checked at admission.

## Bus waits spin to the expected completion, then wait ticks

- **One bus rate.** Verify admits divisor `0x28`, recorded on the attached
  board, only with a 24 MHz parent: APBS1 from PLL_REF, or a raw DCXO that
  decodes to 24 MHz. That gives 400 kHz and a 22.5 µs byte time. The
  one-tick-per-phase path for slower rates is removed.
- **Request admission.** The task checks `clock_live`, `owned()` and the
  borrow status once, when a request becomes active. The provider holds its
  clock lease, pin lease and object for its lifetime. Their only revocation
  event is provider stop, which is refused while a slot is active, so the
  admission result covers every transaction of the request and its final
  STOP. The failed state, which a client fault latch changes, is checked
  before each transaction.
- **Phases.** The task spins on the counter to the expected completion
  (9 bit times per byte, 1 for START or STOP). If the control flag is still
  clear, it re-reads control once per byte time for up to four byte times
  (90 µs), then once per tick until the request deadline. A bus error sets
  the control flag like any other state change, and status is read once the
  flag is set. An iteration cap faults if the counter stops.
- **Barriers.** Inside a phase and in the PCK600 poll the accesses are plain
  Device accesses. A `DSB SY` follows the START, STOP or command write that
  begins a timed phase, because the timing assumes that write reached the
  device (section 1.2). Polled reads have no barrier. Moving off the
  `a733_mmio_*_dsb_sy_both` accessors waits on the barrier qualification in
  `docs/shared-helpers.md`.
- **Time source.** The provider and domain owner read `CNTPCT_EL0` and keep
  `a733_monotonic_advance` under their try-locks. Status skips it, because a
  counter that reads behind only delays READY. The adapter checks at init
  that `CNTFRQ_EL0` is 24 MHz.
- **No TWI interrupt.** Byte interrupts would cost 34 per write and a route
  in every image, while settle on the same path waits up to 1,000 µs.

## Memory placement and synchronization

Both objects live in kernel `.bss` (Normal Write-Back, Inner Shareable). No
DMA master reads them, so no cache maintenance applies.

| Ledger region | Size | Written by |
| --- | --- | --- |
| Lock word and free bitmap | 64 B | Acquire, release |
| Fault word | 64 B | A fault |
| DCDC3 and DCDC5 rail lines | 128 B | Floor-setter or PMIC task, as the in-flight flag selects; acquire and release for the holder count |
| Holder slots, 32 B × 32 | 1,024 B | That slot's acquire, release and pin count |
| Borrows, 48 B × 10 | 480 B | Provider submit and reap; camera |

The total is 1,760 bytes, down from 20,096. Thirty-two slots cover the 26
acquire sites outside this package, the framebuffer, GPU, NPU and two CPU
clusters, with one spare. Ten borrows cover the provider's eight request
slots (two reserved for DCDC3 and DCDC5 writes), the camera's lifetime
borrow, which its TWI11 driver checks per transfer, and one spare. The slot
array starts on a 64-byte line, so a status read touches the fault line, one
slot line and, for DCDC3 or DCDC5, one rail line.

Mutations and domain transitions take one nonblocking try-lock per object
and return BUSY on contention. The domain owner releases its lock before it
calls the ledger or provider, spins on PCK600 or wakes waiters.

Status takes no lock. A token identifier holds the slot index in its low
bits (5 for the ledger, 7 for the clock registry) and a never-reused
generation above them. Slot fields are relaxed atomics.

- **Slot writer:** stores an invalid slot word, `DMB ISHST`, writes the
  fields, then store-releases the new word.
- **Slot reader:** load-acquires the fault word and slot word, loads the
  fields, `DMB ISHLD`, and reloads the slot word. A changed word is retried
  once, then BUSY.
- **Rail line:** the reader load-acquires the status word and only then
  loads the deadline, which pairs with the publisher's deadline store before
  its release store. A deadline from a later publication is never earlier,
  so reading it can only delay READY. A contention case in
  `checks/rail-leases.c` races publication against status and requires that
  READY never appears before the new deadline.

No DSB and no full-system barrier is needed, because only CPUs share these
lines. The reader returns INVALID when the index or generation differs, or
when a stored field differs from the presented token: owner and mask for
rails; owner, shared and exclusive for clocks. The forged-token cases in
`checks/clock-wide.c`, `clock-width.c` and `clock-usb-scopes.c` still hold.
The pin count is not compared, so a live token's slot word never changes.
The clock registry adopts this scheme without changing its 48-byte token or
its 73 production status callers.

Lock-free status makes each per-access check cheaper but keeps it. A
follow-up outside this package moves `live()` in `a733_mmc_platform.c`,
`healthy()` in `a733_twi_native.c` and `leases_live()` in
`a733_camera_native.c` to one status check at admission, repeated only after
the fault word changes (section 2.4).

## Reused and removed mechanisms

- **Reused:** the ledger API and retention, PMIC transactions and snapshots,
  the DE_SYS PCK600 code and delay row, clock scopes, the PCK gate lease and
  the CPU grant.
- **Removed:** clock scope `A733_POWER_DCDC2`; the GPU RAIL resource; ledger
  bridges, OPENING and exclusive mode; `a733_pmic_consumer.c`; public
  `a733_pmic_submit_write`; four duplicate PCK sequences; the domain-9 path;
  six supply reads; the NPU direct S_TWI0 path; the tick-wait phase path;
  twelve per-image PMIC loops; the separate borrow count; about 44 of 50
  setup reads per request; `owned()` and counter-clock reads per register
  access; the CPU clock driver's selector writes.
- **Changed outside this package:** GPU resource and platform files, NPU
  platform, G2D, deinterlace, framebuffer, `hdmi_native.c`, DisplayPort,
  DSI scanout, the PMIC loops and task creation of the images listed above,
  consumer wait loops (notification wait, including the NPU supply sample
  in `apps/npu-convolution/native.c`), and checks that build a ledger.
  `include/a733_power_domains.h` needs the coordinator's approval. No
  reserved CPU code changes.

## Performance budgets

The first implementation commit is accepted only when it adds the counters
below and records the parent-commit counts with their input identities,
including a number for startup to PMIC READY, before any behavior changes.
The baselines below are provisional until then. Instruction and line counts
come from a step in `checks/rail-leases.py` that builds status and acquire
with clang `-O2 -mcpu=cortex-a55` and runs them in Unicorn with 42 clock and
16 rail leases, then 126 of each. Bus, spin, tick and wake counts come from
`checks/a733-pmic-polls.c` and `checks/power-domains-pck.c`, run in the
board, GPU, NPU and HDMI configurations. They count requester ticks from
submit to observed READY while another core calls `a733_pmic_result`.

| Operation | Provisional baseline | Target |
| --- | --- | --- |
| Rail status, one-rail token | 536 / 1,554 instructions; 60 / 233 lines | ≤ 40 instructions, ≤ 3 lines |
| Clock token status | 243 / 600 instructions | ≤ 30 instructions, 2 lines |
| Acquire, poll and release | 4,026 / 7,773 instructions | ≤ 450 instructions, no bus access |
| Status with 4 readers, 1 writer, sanitizers | BUSY under contention | No false READY, INVALID or early READY |
| Authority check per PMIC transaction | 818 / 2,179 instructions per phase | ≤ 10 instructions |
| Bus reads per wait phase | 1 per poll, repeated after each yield | ≤ 2 on time; ≤ 4 more before a tick wait |
| Late spin per phase | Not applicable | ≤ 90 µs, longest recorded |
| Setup reads per request | About 50 | 6, plus 3 after each STOP |
| Ledger storage | 20,096 B | ≤ 1,760 B (static assertion) |
| Snapshot, submit to requester wake, every image | Up to one tick per step | ≤ 0.45 ms, 0 requester ticks |
| Startup to PMIC READY | Recorded by the first commit | ≤ 1 tick plus one transaction |
| Domain power-on with supply read (GPU, NPU, HDMI) | GPU: 27 polls | ≤ 0.7 ms, 1 wake, 0 ticks |
| Second requester on a waiting domain | One poll per tick | 1 wake at publication, 0 extra ticks |
| Slow PCK600 acknowledgement | One tick per poll | ≤ 1 tick per retry |
| Domain 5 power-off | GPU: 8 polls | 1 step |
| CPU-side settle spin | Not applicable | ≤ 300 µs, longest recorded |
| Delay added to USB0 or OS audio | Not applicable | 0, by priority |

A CPU rail write has no production caller. Its budget is ≤ 0.65 ms from
request to readback, one wake, 0 ticks, and ≤ 0.85 ms to READY for
800→870 mV; larger steps reach READY within one tick of their deadline.

## Failure classes (section 1.8)

- **Refused or BUSY before any bus access,** including a floor write
  refused or expired before its first transaction: flag cleared, floor
  unchanged, nothing retained.
- **No holder slot:** EXHAUSTED, nothing changed.
- **Supply mismatch or failed precondition at row request:** the row is not
  counted and no ON command is written; error.
- **Unknown state:** a bus fault, readback mismatch, changed selector or
  expiry after a rail write's first transaction retains the provider, the
  active borrow and the DCDC3 and DCDC5 holders. A PCK600 status other than
  the requested one after the domain write, or expiry after it, retains the
  domain, its rows, their tokens and the PCK lease and sets FAILED.

## Not included, with reasons

- **DCDC2 and DCDC4 writes:** no NPU point at 492 MHz needs more than
  800 mV and no GPU operating-point table exists.
- **Lowering below the baseline:** it needs every consumer of each rail.
- **Per-holder floors:** the CPU clock driver is the only floor-setter and
  changes one cluster at a time.
- **Power-off for domains 1, 4 and 10:** it needs drain evidence for every
  master in the domain, and no caller needs it.
- **CPU cluster and core domains** (reserved code) and **domains 0, 2, 3
  and 6–9** (current owners stay).
- **The TWI_DRV packet engine:** needed only when a governor raises more
  than 10 times per second.

## Board evidence that would confirm the design

- The measured S_TWI0 SCL rate for divisor `0x28`.
- DCDC3 or DCDC5 readbacks after a raise and a lowering, with
  request-to-readback time, wake-to-READY time and the longest spins.
- PMIC page 0 and unchanged DCDC2–5 selectors after PSCI CPU_ON, CPU_OFF and
  suspend, and a capture showing no other S_TWI0 master.
- PCK600 time to ON for domains 1, 4, 5 and 10 and to OFF for domain 5,
  across repeated cycles, with each domain's preconditions read just before
  the ON write.
- PMU cycles for rail and clock status on an A55 and an A76, idle and during
  a mutation on the other cluster.
- A schematic source for the DE_SYS, G2D and deinterlace supply.
