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
A **holder** record stores the token identifier, owner and rail mask. When
no holder slot is free, acquire returns EXHAUSTED before any change.

`a733_rail_acquire` keeps its signature and token layout, including the
exclusive field that must now be zero, so the 26 callers outside this
package do not change. It returns READY at once for every rail except DCDC3
and DCDC5. For those two it returns PENDING until the baseline snapshot
described below is published.

Each consumer holds one token for its rails. The display framebuffer, GPU
and NPU, which today reach DCDC2 or DCDC4 through the clock registry, the
GPU manager or `a733_pmic_consumer.c`, call `a733_rail_acquire` themselves.
The NPU startup's direct S_TWI0 read path is removed: `npu-convolution`, the
only image that runs NPU startup, already binds the PMIC provider, and
startup init refuses an unbound provider.

**DCDC3 and DCDC5 are written.** The CPU clock driver holds DCDC5 for the
little cluster and DCDC3 for the big cluster. Each of these rails has one
64-byte **rail line**, holding:

- the **floor** and the previous floor, written only by the cluster's
  driver under the CPU grant; the little-cluster floor includes the DSU's
  requirement, because the DSU shares DCDC5 and its rate never changes;
- the **status word**: state, published voltage, in-flight flag and a
  baseline-valid flag;
- the **settle deadline**, and the **baseline**, which is the voltage read
  by the snapshot that the rail's first holder triggers.

A disabled or undecodable output is refused, because nothing in this design
enables a rail.

- **Target.** The target is the larger of the baseline and the floor. A
  floor change **moves the target** when the new target differs from the
  published (readback) voltage.
- **Floor change order.** Under the ledger lock, the call validates the
  holder and grant, refuses with BUSY if the in-flight flag is set, saves
  the previous floor, stores the new floor and, if the target moves, sets
  the flag. It releases the lock and then submits the write to the
  provider slot reserved for that rail; the submit takes the ledger lock
  again to borrow the token.
- **Rollback.** If the provider refuses the write before its first bus
  transaction (PENDING during startup, BUSY, RETAINED, or EXPIRED while
  queued), the ledger restores the previous floor and clears the flag under
  its lock. The floor call returns the provider's result; when the PMIC
  task finds the deadline already passed for a queued write, it performs
  this rollback when it publishes. Only a fault or expiry after the write's
  first transaction has started marks DCDC3 and DCDC5 UNCERTAIN.
- **Publication.** The PMIC task stores the settle deadline, then
  release-stores the status word. That one store publishes the voltage
  decoded from the readback and clears the in-flight flag.
- **Settle deadline.** STOP time plus the ramp from the lowest possible
  voltage at 250 µV/µs, never earlier than the stored deadline. The first
  snapshot sets it no earlier than provider start plus 5,160 µs, the
  full-range ramp and enable delay in the board device tree.
- **Status** is READY when the output is enabled, the baseline is valid, no
  write is in flight, the published voltage is at or above the floor and
  the deadline has passed.
- **Release.** A CPU holder releases only after setting its floor to zero;
  a release with a nonzero floor is refused.

The CPU clock driver raises the floor, waits for READY as described under
"The PMIC task wakes the requester", and then raises the frequency. It
lowers the frequency first and then the floor. Its own selector writes and
ramp waits are removed, and its model cases (frequency changed before
voltage, ramp skipped) run against the floor calls. `checks/rail-leases.c`
gains floor-change cases for submit BUSY, PENDING, RETAINED and EXPIRED
before the first transaction, each ending with the flag clear, the old floor
restored and the rails not UNCERTAIN.

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

These are the values the consumers check today. DSI scanout already holds a
token covering DCDC2, because its panel profile is refused without a DCDC2
entry, so its row is checked like the other domain-1 rows. The NPU runs only
at 492 MHz, and the pinned vendor operating-point table gives 800 mV there
for all 26 speed bins (`checks/npu-platform-reference.py`).

- **Request.** A consumer passes its rail token. The owner checks once that
  the token is live and covers the row's rails, and acquires nothing. When a
  row's count goes from zero to one, the owner submits one read sequence
  under that token: page `0xff`, enable and selector for each rail, page.
  The six per-consumer supply reads are removed. A counted row relies on the
  earlier read, because nothing in the OS writes these four rails.
- **Token pinning.** Counting a row raises a row count in the token's
  holder slot under the ledger lock; releasing the row lowers it. Rail
  release refuses a token whose row count is nonzero and returns INVALID
  with nothing changed. A row retained by a RETAINED or FAILED domain keeps
  its count, so its consumer keeps the token.
- **Power-on.** Supply read checked, the shared PCK600 bus-clock lease
  (`RCCU + 0x1ac`) held, domain on; the consumer then ungates its clocks and
  resets. The GPU manager keeps that gate, which PCIe, video, ISP and USB2
  also lease.
- **Power-off only for domain 5,** whose one bus master is the GPU. When its
  count reaches zero after the GPU has drained its DMA, the owner reads CCU
  `0x0b20` bit 31 and `0x0b24` bits 0 and 16, from the GPU table adapted from
  the pinned `pck600_domains.c`. If any is set, the domain stays on with its
  rails and PCK lease and the release returns RETAINED. Domains 1, 4 and 10
  are never turned off. Several masters share each, and neither the DE_SYS
  provider nor NPU stop turns them off today.
- **Domain 5 found on at boot** gets a count of one only when those three
  CCU bits are clear, as the GPU power-on requires today. Otherwise the
  domain is FAILED and no GPU row is admitted.
- **Transitions.** A domain is OFF, ON_WAIT, ON, OFF_WAIT or FAILED; during
  either wait state other requests for it return PENDING.

`checks/power-domains-pck.c` gains domains 4, 5 and 10 and these cases:

- an open GPU gate refuses power-off;
- domain 5 found on with a gate open is FAILED;
- a DSI-first request on an OFF domain 1 writes no ON command before a
  DCDC2 read;
- releasing every row of domains 1, 4 and 10 writes no OFF command.

`checks/rail-leases.c` gains a case where releasing a token with a counted
row is refused.

## The PMIC task wakes the requester

**Work in.** Every enqueue calls the adapter's `kick` after the submitter
releases the provider try-lock; in the board and OS images `kick` notifies
the PMIC task. Startup stages that wait on no hardware (CLOCK, BASELINE,
PINS, VERIFY) kick the task when they advance. The task blocks on the
notification, bounded by 250 ms, only when the provider is READY with
nothing active or queued. On BUSY it yields. Other images keep their
one-tick loop.

**Completion out.** At submit, the provider slot records the requesting
task's handle, supplied by the adapter. After its release store, the PMIC
task calls the adapter's `wake` with that handle, outside its locks; in the
board and OS images `wake` is `xTaskNotifyGive`. A consumer task that
receives PENDING blocks in `ulTaskNotifyTake` with its deadline as the
timeout instead of `vTaskDelay(1)`. A wake carries no data: the woken task
reruns its state-machine step, so a notification from another source costs
one extra step. Wake-ups per operation:

- **Snapshot or supply read:** one wake at publication, no tick wait.
- **Domain power-on with a supply read:** one wake; the PCK600 spin then
  runs in the same step, and a tick wait occurs only when the
  acknowledgement takes longer than 50 µs.
- **CPU floor raise:** one wake at publication, then the clock driver spins
  on the counter to the published settle deadline: the step's ramp less the
  readback time, at most 280 µs for 800→870 mV and 1,000 µs for the table's
  800→1,050 mV range. It then changes the frequency, with no tick wait.

Both images pin the PMIC task to A55 cores 2–5, away from recognition on
cores 0 and 1. It runs at priority 3, below `board-usb0`, `mimus-usb` and
the OS audio, headset and thermal tasks at 4, which therefore preempt a
spin at once.

The provider reads chip identity once, in its verify stage. A snapshot is
then four transactions (page, enable, selector, page) and a write is six
(the same four, a selector write and a readback). Control values and the
native allow-list do not change. A fault sets the provider's failed state,
marks DCDC3 and DCDC5 UNCERTAIN and returns RETAINED to their holders.

`baseline()` today reads about 25 device registers at activation and again
at publication. Per request the provider keeps only the reads whose inputs
another agent can change: the S_TWI0 gate and reset word, the divisor, the
interrupt 235 enable bit and the three bus-idle registers, which it reads
again after every STOP. Clock parents, GPIO function, TWI_DRV registers and
the other GIC state are checked at admission. The provider holds the clock,
pin and S_TWI0 leases for its lifetime, and the pin lease check already runs
per request.

## Bus waits spin to the expected completion, then wait ticks

- **One bus rate.** Verify keeps the parent-rate check and admits divisor
  `0x28`, recorded on the attached board, only with a 24 MHz parent: APBS1
  from PLL_REF, or a raw DCXO that decodes to 24 MHz. That gives 400 kHz and
  a 22.5 µs byte time. The one-tick-per-phase wait for slower rates is
  removed.
- **Transaction start.** Each transaction, the selector write included,
  starts under the provider object after the task checks the failed state,
  `clock_live`, `owned()` and the borrow status. The task then releases the
  object for the phases, as the pin and bus waits already do; only it
  changes the active slot and phase. Once a client latches a fault, the task
  issues no bus access other than the STOP of a transaction in progress.
- **Phases.** The task spins on the counter to the expected completion
  (9 bit times per byte, 1 for START or STOP). If the control flag is still
  clear, it re-reads control once per byte time for up to four byte times
  (90 µs), then once per tick until the request deadline. The controller sets the control
  flag for a bus error (status `0x00`) as for any other state change, so the
  control read detects it and status is read once the flag is set. An
  iteration cap faults if the counter stops.
- **Barriers.** Inside a phase and in the PCK600 poll the accesses are plain
  Device accesses. A `DSB SY` follows the START, STOP or command write that
  begins a timed phase, because the timing assumes that write reached the
  device (section 1.2). Polled reads have no barrier. Moving off the
  `a733_mmio_*_dsb_sy_both` accessors waits on the barrier qualification in
  `docs/shared-helpers.md`.
- **Time source.** The provider and domain owner read `CNTPCT_EL0` and keep
  `a733_monotonic_advance` under their try-locks. Status skips it, because a
  counter that reads behind only delays READY. The adapter keeps its init
  check that `locks->now` is `a733_counter_clock_now` and adds a check that
  `CNTFRQ_EL0` is 24 MHz.
- **No TWI interrupt.** Byte interrupts would cost 34 per write plus a route
  in every image, while settle on the same path waits up to 1,000 µs.
- **PCK600 acknowledgement.** No transition interrupt is documented and the
  vendor driver polls. The owner spins up to 50 µs with no lock held, then
  returns PENDING; the consumer retries once per tick up to 10 ms.

## Memory placement and synchronization

Both objects live in kernel `.bss` (Normal Write-Back, Inner Shareable). No
DMA master reads them, so no cache maintenance applies.

| Ledger region | Size | Written by |
| --- | --- | --- |
| Lock word, free bitmap, fifteen holder counts | 64 B | Acquire, release |
| Fault word | 64 B | A fault |
| DCDC3 and DCDC5 rail lines | 128 B | PMIC task; floor change |
| Holder slots, 32 B × 32 | 1,024 B | That slot's acquire, release and row count |
| Borrows, 48 B × 10 | 480 B | Provider submit and reap; camera |

The total is 1,760 bytes, down from 20,096. Thirty-two slots cover the 26
acquire sites outside this package, the framebuffer, GPU, NPU and two CPU
clusters, with one spare. Ten borrows cover their users:

- the provider's eight request slots, two of them reserved for DCDC3 and
  DCDC5 writes and the rest serving baseline snapshots, domain-owner supply
  reads and consumer snapshots;
- the camera's lifetime borrow, which its TWI11 driver checks per transfer
  (`drivers/camera/a733_camera_native.c`);
- one spare.

The slot array starts on a 64-byte line, so a 32-byte slot never crosses a
line and a status read touches the fault line, one slot line and, for DCDC3
or DCDC5, one rail line.

Mutations, publication and domain transitions take one nonblocking try-lock
per object, returning BUSY on contention. The domain owner releases its lock
before calling the ledger or provider and before the PCK600 spin.

Status takes no lock. A token identifier holds the slot index in its low
bits (5 for the ledger, 7 for the clock registry) and a never-reused
generation above them. Slot fields are relaxed atomics.

- **Writer:** stores an invalid slot word, `DMB ISHST`, writes the fields,
  then store-releases the new word.
- **Reader:** load-acquires the fault word and slot word, loads the fields,
  `DMB ISHLD`, and reloads the slot word.

No DSB and no full-system barrier is needed, because only CPUs share these
lines. The reader returns INVALID when the index or generation differs. It
also returns INVALID when a stored field differs from the presented token:
owner and mask for rails; owner, shared and exclusive for clocks. The
forged-token cases in `checks/clock-wide.c`, `clock-width.c` and
`clock-usb-scopes.c` therefore still hold. Holder PENDING or READY state
lives in the rail line, and the row count is not compared by status, so a
live token's slot word never changes. If it changes between the two loads,
the reader retries once, then returns BUSY. The clock registry adopts this
scheme without changing its 48-byte token or its 73 production status
callers.

## Reused and removed mechanisms

- **Reused:** the ledger API and retention, PMIC transactions and snapshots,
  the DE_SYS PCK600 code and delay row, clock scopes, the PCK gate lease and
  the CPU grant.
- **Removed:** clock scope `A733_POWER_DCDC2`; the GPU RAIL resource; ledger
  bridges, OPENING and exclusive mode; `a733_pmic_consumer.c`; public
  `a733_pmic_submit_write`; four duplicate PCK sequences; the domain-9 path;
  six supply reads; the NPU direct S_TWI0 path; the tick-wait phase path;
  about 44 of 50 setup reads per request; `owned()` and counter-clock reads
  per register access; the CPU clock driver's selector writes.
- **Changed outside this package:** GPU resource and platform files, NPU
  platform, G2D, deinterlace, framebuffer, `hdmi_native.c`, DisplayPort, DSI
  scanout, the board and OS PMIC loops, task creation and consumer task
  loops (notification wait), and checks that build a ledger.
  `include/a733_power_domains.h` needs the coordinator's approval. No
  reserved CPU code changes.

## Performance budgets

The first implementation commit adds the measurements and records the
parent-commit numbers with their inputs; the baselines below come from an
earlier uncommitted measurement and are provisional until then. Instruction
and line counts come from a step in `checks/rail-leases.py` that builds
status and acquire with clang `-O2 -mcpu=cortex-a55` and runs them in
Unicorn with 42 clock and 16 rail leases, then 126 of each. Bus, spin, tick
and wake counts come from counters in `checks/a733-pmic-polls.c`, which also
counts requester ticks from submit to observed READY while another core
calls `a733_pmic_result`, and records the longest late spin. Contention
cases go into `checks/rail-leases.c` and
`checks/clock-counter-contention.c`.

| Operation | Provisional baseline | Target |
| --- | --- | --- |
| Rail status, one-rail token | 536 / 1,554 instructions; 60 / 233 lines | ≤ 40 instructions, ≤ 3 lines |
| Clock token status | 243 / 600 instructions | ≤ 30 instructions, 2 lines |
| Acquire, status and release | 4,026 / 7,773 instructions | ≤ 400 instructions, no bus access |
| Status with 4 readers, 1 writer, sanitizers | BUSY under contention | No false READY or INVALID; forged fields INVALID |
| Authority check per PMIC transaction | 818 / 2,179 instructions per phase | ≤ 10 instructions |
| Bus reads per wait phase | 1 per poll, repeated after each yield | ≤ 2 on time; ≤ 4 more before a tick wait |
| Late spin per phase | Not applicable | ≤ 90 µs, longest recorded |
| Setup reads per request | About 50 | 6, plus 3 after each STOP |
| Ledger storage | 20,096 B | ≤ 1,760 B (static assertion) |
| Snapshot, submit to requester wake | Up to one tick, then bus time | ≤ 0.45 ms, 0 requester ticks, also under contention |
| Startup to PMIC READY | Measured on the parent commit | ≤ 1 tick plus one transaction |
| Domain power-on with supply read (PCK model) | GPU: 27 polls | ≤ 0.7 ms wall time, 1 wake, 0 ticks |
| Domain power-on without supply read | Included above | 1 step, 0 ticks |
| Domain 5 power-off | GPU: 8 polls | 1 step |
| Delay added to USB0 or OS audio | Not applicable | 0, by priority |

A CPU rail write has no production caller. Its budget is ≤ 0.65 ms from
request to readback, one wake, 0 ticks, and ≤ 0.85 ms to READY for
800→870 mV. On-time spin per transaction is its bus time, at most 0.1 ms.
If the parent-commit measurement shows that stage kicks miss the startup
budget, a bounded repeat of the startup poll is the fallback.

## Failure classes (section 1.8)

- **Refused or BUSY before any bus access,** including a floor write
  refused or expired before its first transaction: rolled back, nothing
  retained.
- **No holder slot:** EXHAUSTED, nothing changed.
- **Supply mismatch at row request:** the row is not counted; error.
- **Unknown state:** a bus fault, readback mismatch or expiry after a rail
  write's first transaction retains the provider and the DCDC3 and DCDC5
  holders. A PCK600 status other than the requested one after the domain
  write, or expiry after it, retains the domain, its rows, their tokens and
  the PCK lease and sets FAILED.

## Not included, with reasons

- **DCDC2 and DCDC4 writes:** no NPU point at 492 MHz needs more than
  800 mV and no GPU operating-point table exists.
- **Lowering below the baseline:** it needs every consumer of each rail.
- **Follow-up writes and per-holder floors:** the CPU clock driver is the
  only floor-setter and changes one cluster at a time.
- **Power-off for domains 1, 4 and 10:** it needs drain evidence for every
  master in the domain, and no caller needs it.
- **CPU cluster and core domains** (reserved code) and **domains 0, 2, 3
  and 6–9** (current owners stay).
- **The TWI_DRV packet engine:** needed only when a governor raises more
  than 10 times per second.

## Board evidence that would confirm the design

- The measured S_TWI0 SCL rate for divisor `0x28`.
- DCDC3 or DCDC5 readbacks after a raise and a lowering, with
  request-to-readback time, wake-to-READY time and the longest late spin.
- PMIC page 0 and unchanged DCDC2–5 selectors after PSCI CPU_ON, CPU_OFF and
  suspend, and a capture showing no other S_TWI0 master.
- PCK600 time to ON for domains 1, 4, 5 and 10 and to OFF for domain 5,
  across repeated cycles.
- PMU cycles for rail and clock status on an A55 and an A76, idle and during
  a mutation on the other cluster.
- A schematic source for the DE_SYS, G2D and deinterlace supply.
