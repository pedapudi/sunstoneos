# Design: one owner for supply rails and one for power domains

Status: **design**, not implemented; physical behavior stays unqualified until
the board evidence at the end exists. The performance principles applied are
those in `AGENTS.md`, because `docs/performance-architecture.md` does not
exist yet.

Today the clock registry holds AXP8191 output DCDC2, the GPU manager holds
DCDC4, and the rail ledger reaches both through bridges. The PCK600
power-domain sequence exists in five places: the GPU power controller, the
DE_SYS provider, the NPU platform, `platform/a733/hdmi_native.c` and
`apps/displayport/native.c`.

## The rail ledger owns every output; a domain owner owns four domains

The rail ledger becomes the only authority for all 39 PMIC outputs. The clock
registry refuses DCDC2 (scope bit 18 becomes reserved), and the GPU manager
loses its RAIL resource and guard slots 16–31. A holder record names an owner,
a rail mask, a mode (shared or exclusive) and a voltage floor that lives as
long as the rate or domain it protects. Only the six rails with consumers are
acquirable: DCDC2–DCDC5, CLDO2 and CLDO5.

- **Adoption** runs once, when the PMIC task starts. It reads the page
  register (`0xff`) and chip identity (`0x0e`–`0x0f`), then the enable and
  selector registers of the six rails in three auto-increment bursts
  (`0x10`–`0x16`, `0x21`, `0x30`–`0x33`). After one shared 5,160 µs wait
  (full-range ramp plus enable delay from the board device tree) it repeats
  the bursts. Agreeing observations become each rail's **baseline**. An
  acquire before adoption completes returns PENDING.
- **Raise.** Only DCDC3 and DCDC5 are written, and only to meet a CPU floor
  that the CPU session grant covers. Any other rail is READY when its
  programmed voltage meets the floor and is refused otherwise.
- **Lower.** The target is the larger of the baseline and the highest floor,
  so the ledger only undoes its own raises.
- **READY** requires floor ≤ programmed voltage, no change in flight, and a
  passed settle deadline; otherwise the result is PENDING.
- **Release during a raise** always succeeds, because the change slot holds
  its own authority.

The CPU clock driver keeps one persistent floor holder per cluster and for the
DSU. A frequency increase raises the floor before the PLL changes; a decrease
changes the PLL first and lowers the floor afterwards.

A new domain owner, `drivers/power/a733_power_domains.c`, holds one entry per
domain: register block, pinned PCK600 delay row, rail floors, and the local
clock and reset words that must be off before power-down. It counts
references and holds one GPU-manager PCK lease while any count is above zero.

| Domain | Consumers | Rail floors |
| --- | --- | --- |
| 1 (DE_SYS) | Display engine, G2D, deinterlace | DCDC2 ≥ 800 mV (assumption) |
| 4 (NPU) | NPU startup | DCDC2 ≥ the column's 492 MHz voltage |
| 5 (GPU_TOP) | GPU | DCDC4 ≥ 800 mV |
| 10 (VO1) | HDMI; DisplayPort | DCDC2 ≥ 800 mV; CLDO2 at baseline (HDMI); CLDO5 ≥ 1800 mV (DisplayPort) |

DCDC5 supplies CPU-L and the DSU, and DCDC3 supplies CPU-B only. The owner
refuses domains 0, 2, 3 and 6–9; the DSI path keeps its PCK client slot to
read domains 0, 1 and 9.

## Power-up and power-down follow one order

Power-up: rail floors READY, PCK lease held, domain on, then the consumer
releases its clocks and resets under its clock-registry scope. Power-down runs
in reverse after the owner checks the local clock and reset words. A domain
found on at boot is adopted with a count of one and never turned off, because
no drain evidence exists for inherited traffic. Other consumers receive BUSY
while a domain is TRANSITIONING.

- **Expiry before the domain write** releases the domain's ledger records,
  retrying a BUSY release on each later poll. The domain is never reported
  OFF while a record remains.
- **Expiry after the domain write**, or of the 10 ms acknowledgement bound,
  marks the domain RETAINED. It keeps its ledger holders and the PCK lease
  for the platform's lifetime and receives no further access.

## NPU startup takes its floor from the speed-bin column

NPU startup reads the speed-bin code from eFuse word `0x0300624c` with the CPU
clock driver's extraction, moved to a shared header. The package imports the
vendor code-to-column map and each NPU column's 492 MHz voltage. Codes `0x01`,
`0x11` and `0x06` have no NPU column and are refused. Code 0 also means an
unprogrammed fuse, so it takes the highest 492 MHz voltage of any column,
800 mV.

The rate stays 492 MHz. The follow-up, outside this package, is a 1704 MHz
(N = 71) PLL_NPU profile for 852 MHz in the ENABLED_RETUNE mode of
`drivers/npu/a733_npu_pll.c`, whose platform-wide reservation already
excludes the other PLL_NPU consumers. Column vf0400 runs 852 MHz at 800 mV,
so it needs no floor change.

## A voltage change flows from the ledger through the PMIC task

A raise on DCDC3 or DCDC5 stores a target, between the baseline and the
highest point the CPU grant admits, in the rail's change slot and notifies the
PMIC task. The provider accepts writes only from that slot, which replaces
today's exclusive consumer lease.

A write request has four transactions: read the enable register, read the
selector, write it, read it back. The pre-write selector must equal the
ledger's value and the readback the written value. Page and identity are read
only at adoption: boot admission excludes secure and remote firmware from
S_TWI0 for the provider's lifetime, and the S_TWI0 clock lease is exclusive.
If board evidence shows another master on S_TWI0, a per-write page read would
not close the race; a hardware spinlock or firmware handoff would be needed.

A fault sets the provider-wide failed state (today's `p->failed`), which
blocks every later PMIC access on every rail. DCDC3 and DCDC5 become
UNCERTAIN and their holders observe RETAINED. A fault inside a write request
makes every rail UNCERTAIN, because the register the data byte reached cannot
be confirmed.

## Waiting uses interrupts where an image routes S_TWI0

- **Control values.** An image constant selects INT_EN (bit 7): START and
  repeated START write `0xe8` (today `0x68`), the other phases `0xc8` (today
  `0x48`). STOP stays `0x58`, which clears INT_EN, so an idle bus still reads
  `0x40` for `bus_idle()`, `baseline()` and other images' models.
  `await_status()` compares `control & 0xf4` with `0x40 | INT_EN`; the native
  allow-list for `0x0708300c` admits only the image's set.
- **Interrupt count.** A read raises six (START, address, register, repeated
  START, read address, receive), a write four; STOP is busy-waited for about
  one bit time. A write request takes 22; adoption takes 67.
- **Images.** The OS image gains a route for GIC interrupt 235 in
  `apps/os/irq.c`. The standalone board image dispatches through
  `apps/board/host_irq_platform.patch` on reserved `platform.c`; the
  storage-maintenance, npu-startup, npu-convolution, npu-clock-handoff and
  radio-session images have no route table. These six images keep bounded
  tick polling with INT_EN clear. The board image needs one dispatch line for
  interrupt 235 in that patch, left to the primary agent.
- **Handler.** It masks interrupt 235 at the GIC and notifies the PMIC task,
  which unmasks it after its next control write.
- **PMIC task.** With no request, its notification wait is bounded by 250 ms.
  Each phase wait is bounded by the earlier of the request deadline and 2 ms,
  which exceeds one byte at the slowest admitted 10 kHz rate. The task calls
  `BOARD_PROGRESS` on every wake, inside the 1 s `board-pmic` service window.
  A timed-out phase, for example after a lost interrupt, takes the fault path.
- **CPU clock driver.** After adoption, only CPU raises leave a rail
  PENDING, so this driver is the only task that waits on a rail. Its task lives
  for the platform's lifetime and registers once at ledger initialization.
  The PMIC task notifies it on index 0 with a dedicated set-bits bit after
  publishing READY or a fault. The driver rechecks status and its absolute
  deadline after every wake and busy-waits the settle remainder on the
  generic counter; the largest granted raise, 800→1050 mV at 250 µV/µs,
  settles in 1,000 µs.
- **Ledger and domain-owner calls** never wait. They return READY, PENDING,
  BUSY (mutations only) or a refusal.
- **PCK600 acknowledgement** is polled for up to 50 µs, then once per tick up
  to 10 ms. The A733 manual documents no domain-transition interrupt, and the
  vendor driver also polls.

## Memory placement and cache policy

Both objects live in kernel `.bss` (Normal Write-Back, Inner Shareable,
platform lifetime). No DMA master reads them, so they need no cache
maintenance. Lines that status checks read are kept apart from lines that
mutations write:

| Ledger region | Size | Written by |
| --- | --- | --- |
| Lock word and 128-bit free-slot bitmap | 64 B | Acquire and release |
| Fault word | 64 B | A fault |
| DCDC3 and DCDC5 status (state, settle deadline), one line each | 128 B | Their voltage changes |
| Status of the other 37 rails, 16 B each | 640 B | Adoption and faults |
| Rail bookkeeping (count, exclusive holder, floor, baseline, change slot), 16 B × 39 | 640 B | Acquire, release, floor change |
| Holder slots (identifier, owner, mask, mode, floor), 64 B × 128 | 8,192 B | That slot's acquire and release |

The total is 9,728 bytes, down from 20,096. PCK600 and clock registers use the
`a733_mmio.h` helpers on the existing Device mapping with no added barrier.

## Synchronization across cores

Acquire, release, floor change and domain transitions keep one nonblocking
try-lock per object: compare-and-swap with acquire ordering, release store on
exit, BUSY on contention, no spin and no nesting. The domain owner marks a
domain TRANSITIONING and drops its lock before calling the ledger.

Status checks take no lock and never return BUSY. A token identifier holds
its slot index in the low seven bits and a never-reused generation above
them; a grant release-stores it after the slot fields, and a release stores
zero. A check acquire-loads the fault word and the slot word, compares owner,
mask and mode (relaxed loads), and reloads the slot word after an acquire
fence. Any mismatch returns INVALID.

For rail status, the writer stores the settle deadline (relaxed) and then the
state word (release); the reader acquire-loads the state word and then loads
the deadline. A reader that sees a new state therefore sees its deadline, so
the result can be PENDING early but never a false READY. The clock registry
adopts the same scheme without changing its 48-byte token, so the 57 callers
of `a733_clock_token_status` do not change. The PMIC provider checks each bus
phase with one acquire load of each fault word.

## Reused and removed mechanisms

- **Reused:** the ledger API and retention semantics, the PMIC request and
  readback, the pinned PCK600 delay row, clock-registry scopes, the
  GPU-manager PCK lease, the CPU session grant, the 5,160 µs guard, and
  notification index 0.
- **Removed:** clock scope `A733_POWER_DCDC2`; the GPU RAIL resource; the
  ledger bridges and OPENING state; `a733_pmic_consumer.c` and the `pmic_view`
  binding; per-write page and identity reads; the PCK steps and PCK client
  slots in the GPU, DE_SYS, NPU, HDMI and DisplayPort code; and the CPU clock
  driver's direct selector write.
- **Changed outside this package:** GPU resource and platform files, the NPU
  platform, the G2D, deinterlace, HDMI and DisplayPort paths, `native_usb.c`,
  `apps/board/main.c`, `apps/os/irq.c`, and checks that construct a ledger.

## Performance budgets

Instruction and line counts come from clang `-O2 -mcpu=cortex-a55` code in
Unicorn AArch64 emulation with 42 clock and 16 rail leases, or 126 of each;
they are proxies until the board PMU counts exist. At 400 kHz a one-byte
register read takes about 95 µs, each further burst byte 23 µs, and a write
70 µs. The PMIC model reads the inherited TWI divisor and checks 400 and
100 kHz.

| Operation | Baseline (typical / near full) | Target | Measurement |
| --- | --- | --- | --- |
| Rail status | 536 / 1,554 instructions; 60 / 233 lines | ≤ 40 instructions, ≤ 3 lines, no read-modify-write | Unicorn harness `checks/rail-authority-cost.py` |
| Clock token status | 243 / 600 instructions | ≤ 30 instructions, 2 lines | Same harness |
| Status during a concurrent acquire and release | Each check takes the lock | No BUSY, no false READY | Static offset assertions; `checks/rail-status-contention.c`, 4 readers and 1 writer under sanitizers, including the state and deadline interleaving |
| Authority check per PMIC bus phase | 818 / 2,179 instructions | 2 acquire loads, ≤ 10 instructions | Unicorn harness |
| Shared lease acquire, status, release | 4,026 / 7,773 instructions | ≤ 400 instructions, no bus access, including the first acquire of each rail | Unicorn harness |
| Ledger storage | 20,096 B | ≤ 9,728 B | Static assertion |
| Adoption, OS image | None | 8 reads, 1 wait, 67 interrupts: ≤ 7.1 ms at 400 kHz, ≤ 11 ms at 100 kHz | PMIC host model with interrupt completions |
| Adoption, polled images | None | ≤ 16 ms: one tick per read plus the wait | `checks/a733-pmic-polls.c` |
| PMIC task wake | One tick (`vTaskDelay(1)`) | ≤ 50 µs; no tick wait on the path | Host model counts tick waits; time is board evidence |
| CPU-B 800→870 mV (vf0400, 1196→1716 MHz), request to readback | About 60 polls, 60 ms (model) | ≤ 0.65 ms at 400 kHz (0.36 ms bus, 22 interrupts at ≤ 10 µs, one wake); ≤ 1.8 ms at 100 kHz | `checks/a733-pmic-polls.c` with interrupt completions |
| Same raise, request to READY | None | ≤ 0.95 ms at 400 kHz (settle counted from the write); ≤ 2.1 ms at 100 kHz | PMIC and ledger host models |
| Power-on, domains 1, 4, 5, 10 | GPU: 27 steps, one per poll | 2 PCK reads, write, readback, status reads within the 50 µs spin; 10 delay-row accesses on the first power-on per boot | Domain host model counts accesses |
| Power-down, same domains | GPU: 8 steps, one per poll | Local clock and reset reads (GPU: 2), 2 PCK reads, write, readback, status reads | Domain host model |
| Power-authority calls per NPU job | 0 | 0 | NPU startup model |

Consumer clock and reset accesses stay with the consumer. The host checks
gain sanitizer cases for persistent floors, release during a raise, ungranted
writes, stale tokens, every speed-bin code, domain expiry on both sides of the
domain write, retained faults, a lost S_TWI0 interrupt, and idle progress.

## Not included, with reasons

- **DCDC2 and DCDC4 writes:** no reachable NPU point needs more than 800 mV,
  no grant covers DCDC2, and no GPU operating-point table exists.
- **Rails without a consumer:** adopting them would cost bus time for no user.
- **Lowering below the baseline:** it needs a complete consumer list per rail.
- **CPU cluster and core domains:** they belong to reserved CPU startup code.
- **The TWI_DRV packet engine** at S_TWI0 + `0x200` raises one interrupt per
  transaction, 4 per write request instead of 22. It is the follow-up, with
  the planned single TWI engine.

## Board evidence that would confirm the design

- DCDC3 or DCDC5 readbacks after a granted raise and after the lowering, with
  counter time to readback and 22 S_TWI0 interrupts per write.
- Adoption bursts agreeing with single-register reads (AXP8191 auto-increment).
- The speed-bin word of the board on USB port 3-1.
- PCK600 status for domains 1, 4, 5 and 10 across repeated on and off cycles,
  with counter time from the domain write to ON compared with the 50 µs spin.
- PMU cycle counts for rail and clock token status on an A55 and an A76 core,
  uncontended and with an acquire and release running on the other cluster.
- A schematic source for the DE_SYS, G2D and deinterlace supply; the board
  device tree names DCDC2 only for the NPU, HDMI and eDP.
