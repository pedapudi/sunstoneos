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
`a733_rail_acquire` keeps its signature, token layout and PENDING result,
and its exclusive mask must be zero. A **holder** record stores the token
identifier, owner and rail mask.

Each consumer holds one token for its rails. The display framebuffer, GPU
and NPU, which today reach DCDC2 or DCDC4 through the clock registry, the
GPU manager or `a733_pmic_consumer.c`, call `a733_rail_acquire` themselves.
The NPU startup's direct S_TWI0 read path is removed: `npu-convolution`, the
only image that runs NPU startup, already binds the PMIC provider, and
startup init refuses an unbound provider.

**DCDC3 and DCDC5 are written.** The CPU clock driver holds DCDC5 for the
little cluster and DCDC3 for the big cluster. Each of these rails has one
**floor word**, written only by its cluster's driver under the CPU grant.
The DSU shares DCDC5 and its rate never changes, so the little-cluster floor
includes the DSU's requirement. Each written rail also has a **status word**
(state, published voltage, in-flight flag), a settle deadline and a
**baseline**: the voltage read by the provider snapshot that the rail's
first holder triggers. A disabled or undecodable output is refused, because
nothing in this design enables a rail.

- **One write per rail.** The target is the larger of the baseline and the
  floor. A floor change that moves the target sets the in-flight flag and
  queues a write in a provider slot reserved for that rail. While the flag
  is set, another floor change returns BUSY with nothing changed and the
  caller retries. A CPU holder releases only after setting its floor to
  zero; a release with a nonzero floor is refused.
- **Publication.** The PMIC task stores the settle deadline, then
  release-stores the status word. That one store publishes the voltage
  decoded from the readback and clears the in-flight flag.
- **Settle deadline.** STOP time plus the ramp from the lowest possible
  voltage at 250 µV/µs, never earlier than the stored deadline. The first
  snapshot sets it no earlier than provider start plus 5,160 µs, the
  full-range ramp and enable delay in the board device tree.
- **Status** is READY when the output is enabled, no write is in flight, the
  published voltage is at or above the floor and the deadline has passed.
- **Deadlines.** Ledger snapshots and writes carry the deadline passed by
  the acquiring or floor-setting caller.

The CPU clock driver raises the floor, waits for READY and then raises the
frequency; it lowers the frequency first and then the floor. Its own
selector writes and ramp waits are removed, and its model cases (frequency
changed before voltage, ramp skipped) run against the floor calls.

## The DE_SYS provider becomes the domain owner

`drivers/power/a733_de_sys_power.c`, which powers domains 1 and 10 for G2D
and the framebuffer today, becomes the owner of domains 1, 4, 5 and 10,
renamed `a733_power_domains.c`. Its per-consumer ops, `authorize` callback
and domain-9 path, which has no production caller, are removed.

| Domain | Consumer rows | Supply each row requires |
| --- | --- | --- |
| 1 (DE_SYS) | Framebuffer, G2D, deinterlace, DSI scanout | DCDC2 = 800 mV (DSI: none) |
| 4 (NPU) | NPU startup | DCDC2 = 800 mV |
| 5 (GPU_TOP) | GPU | DCDC4 = 800 mV |
| 10 (VO1) | HDMI; DisplayPort | DCDC2 = 800 mV and CLDO2 or CLDO5 = 1,800 mV |

These are the values the consumers check today. The NPU runs only at
492 MHz, and the pinned vendor operating-point table gives 800 mV there for
all 26 speed bins (`checks/npu-platform-reference.py`).

- **Request.** A consumer passes its rail token. The owner checks once that
  the token is live and covers the row's rails, and acquires nothing. When a
  row's count goes from zero to one, the owner submits one read sequence
  under that token: page `0xff`, enable and selector for each rail, page.
  The six per-consumer supply reads are removed. A counted row relies on the
  earlier read, because nothing in the OS writes these four rails. A
  consumer releases its row before its token.
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
  provider nor NPU stop turns them off today. Domain 5 found on at boot gets
  a count of one.
- **Transitions.** A domain is OFF, ON_WAIT, ON, OFF_WAIT or FAILED; during
  either wait state other requests for it return PENDING.

`checks/power-domains-pck.c` gains domains 4, 5 and 10, a case where an open
GPU gate refuses power-off, and a case where releasing every row of domains
1, 4 and 10 writes no OFF command.

## The PMIC task performs every snapshot and write

Every enqueue calls the adapter's `kick` after the submitter releases the
provider try-lock; in the board and OS images `kick` notifies the PMIC task.
The task blocks on the notification, bounded by 250 ms, only when the
provider is READY with nothing active or queued. On BUSY it yields. During
startup it repeats the poll while a stage advances, for at most 200 µs,
because the CLOCK, BASELINE, PINS and VERIFY stages wait on no hardware.
Other images keep their one-tick loop.

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

## Waiting uses one bounded counter spin

- **One bus rate.** Verify admits only divisor `0x28`, recorded on the
  attached board, which decodes to 400 kHz from the 24 MHz parent. The
  one-tick-per-phase wait for slower rates is removed.
- **Phases.** The task spins without the provider object, as the pin and bus
  waits already do; only it changes the active slot and phase, and it takes
  the object back to publish. It reads the counter until the expected
  completion (9 bit times per byte, 1 for START or STOP), then reads
  control. While the flag is clear it re-reads once per byte time (22.5 µs).
  At 2 ms from phase start it reads status once, faults only on a bus error,
  and otherwise waits one tick before resuming; the request deadline alone
  decides expiry. An iteration cap faults if the counter stops.
- **Time source.** The provider and domain owner read `CNTPCT_EL0` and keep
  `a733_monotonic_advance` under their try-locks. Status skips it, because a
  counter that reads behind only delays READY. The adapter keeps its init
  check that `locks->now` is `a733_counter_clock_now`, adds a check that
  `CNTFRQ_EL0` is 24 MHz, and checks `owned()` once per transaction.
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
| DCDC3, DCDC5: status word, deadline, baseline, floor (a line each) | 128 B | PMIC task; floor change |
| Holder slots, 24 B × 32 | 768 B | That slot's acquire and release |
| PMIC snapshot borrows, 48 B × 16 | 768 B | Snapshot submit and reap |

The total is 1,792 bytes, down from 20,096. Thirty-two slots cover the 26
acquire sites outside this package, the framebuffer, GPU, NPU and two CPU
clusters, with one spare. Exhaustion returns BUSY before any change.

Mutations, publication and domain transitions take one nonblocking try-lock
per object, returning BUSY on contention. The domain owner releases its lock
before calling the ledger or provider and before the PCK600 spin.

Status takes no lock. A token identifier holds the slot index in its low
bits (5 for the ledger, 7 for the clock registry) and a never-reused
generation above them. A writer stores an invalid slot word, fences, writes
the fields and release-stores the new word. A reader acquire-loads the fault
word and slot word, loads the fields, fences and reloads the slot word. It
returns INVALID when the index or generation differs. It also returns
INVALID when a stored field differs from the presented token: owner and
mask for rails; owner, shared and exclusive for clocks. The forged-token
cases in `checks/clock-wide.c`, `clock-width.c` and `clock-usb-scopes.c`
therefore still hold. Floors live in the rail lines, so a live token's slot
word never changes; if it changes between the two loads, the reader retries
once, then returns BUSY. The clock registry adopts this scheme without
changing its 48-byte token or its 73 production status callers.

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
  scanout, the board and OS PMIC loops and task creation, and checks that
  build a ledger. `include/a733_power_domains.h` needs the coordinator's
  approval. No reserved CPU code changes.

## Performance budgets

The first implementation commit adds the measurements and records the
parent-commit numbers with their inputs; the baselines below come from an
earlier uncommitted measurement and are provisional until then. Instruction
and line counts come from a step in `checks/rail-leases.py` that builds
status and acquire with clang `-O2 -mcpu=cortex-a55` and runs them in
Unicorn with 42 clock and 16 rail leases, then 126 of each. Bus, spin and
tick counts come from counters in `checks/a733-pmic-polls.c`, including a
case that counts ticks from submit to completion while another core calls
`a733_pmic_result`. Contention cases go into `checks/rail-leases.c` and
`checks/clock-counter-contention.c`.

| Operation | Provisional baseline | Target |
| --- | --- | --- |
| Rail status, one-rail token | 536 / 1,554 instructions; 60 / 233 lines | ≤ 40 instructions, ≤ 3 lines |
| Clock token status | 243 / 600 instructions | ≤ 30 instructions, 2 lines |
| Acquire, status and release | 4,026 / 7,773 instructions | ≤ 400 instructions, no bus access |
| Status with 4 readers, 1 writer, sanitizers | BUSY under contention | No false READY or INVALID; forged fields INVALID |
| Authority check per PMIC transaction | 818 / 2,179 instructions per phase | ≤ 10 instructions |
| Bus reads per wait phase | 1 per poll, repeated after each yield | ≤ 2 on time; late phases counted, ≤ 90 reads |
| Setup reads per request | About 50 | 6, plus 3 after each STOP |
| Ledger storage | 20,096 B | ≤ 1,792 B (static assertion) |
| Snapshot, submit to completion | Up to one tick, then bus time | No tick wait, also under contention; ≤ 0.45 ms |
| Startup to PMIC READY | Measured on the parent commit | ≤ 1 tick plus one transaction |
| Domain power-on, in consumer polls (PCK model) | GPU: 27 polls | ≤ 2 with a supply read (≤ 0.65 ms bus), else 1 |
| Domain 5 power-off | GPU: 8 polls | 1 |
| Delay added to USB0 or OS audio | Not applicable | 0, by priority |

A CPU rail write has no production caller. Its budget is ≤ 0.65 ms from
request to readback, ≤ 0.85 ms to READY for 800→870 mV, and ≤ 0.1 ms of
spin per transaction.

## Failure classes (section 1.8)

- **Refused or BUSY before any access:** nothing is retained.
- **Supply mismatch at row request:** the row is not counted; error.
- **Unknown state:** a bus fault, readback mismatch or expiry during a rail
  write retains the provider and the DCDC3 and DCDC5 holders. A PCK600
  status other than the requested one after the domain write, or expiry
  after it, retains the domain, its rows and the PCK lease and sets FAILED.

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
  request-to-readback time and the longest phase spin.
- PMIC page 0 and unchanged DCDC2–5 selectors after PSCI CPU_ON, CPU_OFF and
  suspend, and a capture showing no other S_TWI0 master.
- PCK600 time to ON for domains 1, 4, 5 and 10 and to OFF for domain 5,
  across repeated cycles.
- PMU cycles for rail and clock status on an A55 and an A76, idle and during
  a mutation on the other cluster.
- A schematic source for the DE_SYS, G2D and deinterlace supply.
