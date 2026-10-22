# Design: one owner for supply rails and one for power domains

Status: **design**, not implemented; physical behavior stays unqualified
until the board evidence at the end exists. Section numbers refer to
`docs/performance-architecture.md`.

Today the clock registry holds AXP8191 output DCDC2, the GPU resource manager
holds DCDC4, and the rail ledger reaches both through bridges. The PCK600
power-domain sequence exists in the GPU, DE_SYS, NPU, HDMI and DisplayPort
code.

## The rail ledger owns fifteen rails; a domain owner owns four domains

The ledger keeps a fixed table of the fifteen outputs that production code
acquires and the PMIC provider decodes. DC1SW2 stays an alias of DCDC1.
Other outputs are refused.

| Rails | Consumers | Ledger action |
| --- | --- | --- |
| DCDC3 (CPU-B), DCDC5 (CPU-L, DSU) | CPU clock driver | Floors, raise, lower |
| DCDC2; DCDC4; CLDO2, CLDO5 | NPU, display, G2D, deinterlace, video, ISP, PCIe, USB2; GPU; HDMI, DisplayPort, MMC, camera | Floor check |
| DCDC1, DCDC8, DCDC9, BLDO1, BLDO2, BLDO5, CLDO1, CLDO3, ELDO6 | USB, Type-C, MMC, camera, radio, PWM, PCIe, SPI, ADC, monitor, thermal | Holder record only |

A **holder** record stores a token identifier, owner, rail mask and floors
for DCDC3 and DCDC5. A floor on another rail is checked once, when set,
because nothing writes those rails. Holder-only consumers keep their PMIC
snapshot at session open. `a733_rail_acquire` keeps its signature, token
layout and PENDING result, so the 26 other production call sites do not
change; its exclusive mask must be zero.

Each of the six **floor rails** has a status entry: state, programmed
voltage, lowest voltage the output may still be at, and settle deadline. When
the holder count goes from zero to one, the ledger queues a **refresh**: four
single-register reads of page (`0xff`), enable, selector and page. This keeps
the session-open read the GPU and NPU take today. The first refresh of DCDC3
or DCDC5 records its **baseline**. Status is READY when every floor is at or
below the programmed voltage, nothing is in flight and the deadline has
passed; otherwise PENDING.

- **Raise and lower.** A DCDC3 or DCDC5 floor above the programmed voltage,
  within the CPU session grant, queues a write. When the highest floor drops,
  the target becomes the larger of the baseline and the highest remaining
  floor.
- **Settle deadline.** A write sets it to the later of the stored deadline and
  its STOP time plus the ramp from the lowest possible voltage at 250 µV/µs.
  Deadlines never decrease, so a lowering cannot shorten a raise still
  ramping. A refresh sets it no earlier than provider start plus 5,160 µs,
  the full-range ramp and enable delay in the board device tree.
- **Release during a change** succeeds; the per-rail change slot keeps its
  own authority until the PMIC task publishes.

The domain owner, `drivers/power/a733_power_domains.c`, keeps per domain a
count, a state, a ledger token with floors, the pinned PCK600 delay row, and
the local clock and reset words that must read off before power-down.

| Domain | Consumers | Floors |
| --- | --- | --- |
| 1 (DE_SYS) | Display framebuffer, G2D, deinterlace | DCDC2 ≥ 800 mV (assumption) |
| 4 (NPU) | NPU startup | DCDC2 ≥ the speed-bin column's 492 MHz voltage |
| 5 (GPU_TOP) | GPU | DCDC4 ≥ 800 mV |
| 10 (VO1) | HDMI; DisplayPort | DCDC2 ≥ 800 mV; CLDO2 as programmed; CLDO5 ≥ 1,800 mV |

The GPU resource manager keeps the PCK600 bus-clock gate (`RCCU + 0x1ac`),
because PCIe, video, ISP and USB2 lease it for other domains; the owner holds
one shared lease while any count is above zero. The DSI path keeps its own
lease, because it only reads domains 0, 1 and 9.

Power-up: floors READY, PCK lease held, domain on, then the consumer ungates
its clocks and resets. Power-down runs in reverse after the owner reads the
local clock and reset words. A domain found on at boot gets a count of one
and is never turned off, because inherited traffic has no drain evidence.

NPU startup decodes the speed-bin code in eFuse word `0x0300624c` with the
CPU clock driver's decoder, moved to a shared header, and the imported vendor
column table. Codes `0x01`, `0x11` and `0x06` have no NPU column and are
refused. Code 0 can mean an unprogrammed fuse, so it takes the highest
492 MHz voltage of any column, 800 mV.

## The PMIC task performs every refresh and write

The ledger stores a request in the rail's change slot and calls the adapter's
`kick`. In the OS and board images, `kick` notifies the PMIC task on index 0,
which it uses for nothing else. Its loop in `apps/board/main.c` replaces the
idle `vTaskDelay(1)` with `ulTaskNotifyTake` bounded by 250 ms. Other images
set `kick` to NULL and keep their one-tick loop.

A write is six transactions: page, enable, selector, page, write, readback.
The enable bit must be set, the selector must equal the ledger's value, both
pages must read 0 and the readback must match. Per-request chip identity
reads are dropped, because the board fixes the identity. Every transaction is
today's single-register sequence ending in NACK, so control values and the
native allow-list do not change. Any fault sets the provider's failed state,
makes every floor rail UNCERTAIN and returns RETAINED to its holders.

## Waiting uses bounded counter spins and no TWI interrupt

- **S_TWI0 phases.** The task spins on the generic counter for each phase,
  bounded by the earlier of the deadline and 2 ms (over one byte at 10 kHz).
  It holds the provider object for one transaction (about 100 µs at 400 kHz)
  and yields between transactions. Byte interrupts would cost 34 per write,
  a route in every image and GIC masking, while settle on the same path
  already waits up to 1,000 µs. Interrupt 235 stays masked.
- **Rail settle.** Status compares the deadline with the counter. The CPU
  clock driver has no production caller, so no waiter is added; budgets end
  at the ledger's publication.
- **PCK600 acknowledgement.** No transition interrupt is documented and the
  vendor driver polls. The owner spins up to 50 µs with no lock held and
  interrupts enabled, then returns PENDING; the consumer retries once per
  tick up to 10 ms. No other call waits.

## Memory placement and cache policy

Both objects live in kernel `.bss` (Normal Write-Back, Inner Shareable). No
DMA master reads them, so no cache maintenance applies. Status lines are
separate from mutation lines.

| Ledger region | Size | Written by |
| --- | --- | --- |
| Lock word and free bitmap; fault word | 2 × 64 B | Acquire and release; a fault |
| DCDC3 and DCDC5 status, one line each | 128 B | Their refresh and changes |
| DCDC2, DCDC4, CLDO2, CLDO5 status | 64 B | Their refresh |
| Floor-rail count, baseline, highest floor, change slot | 128 B | Acquire, release, floor change |
| Holder slots, 32 B × 40 | 1,280 B | That slot's acquire and release |
| PMIC snapshot borrows, 48 B × 16 | 768 B | Snapshot submit and reap |

The total is 2,496 bytes, down from 20,096. Forty slots cover 33 concurrent
holders (26 acquire sites, four domains, three CPU floors) plus seven spare.
Sixteen borrows cover eight provider request slots plus camera and TWI users.
Exhaustion returns BUSY before any change. Registers keep the `a733_mmio.h`
helpers until section 1.2's barriers are qualified.

## Synchronization across cores

Mutations, result publication and domain transitions take one nonblocking
try-lock per object: compare-and-swap with acquire, release store on exit,
BUSY on contention, no spin or nesting, about 400 instructions at most. The
domain owner drops its lock before calling the ledger.

Status checks take no lock. A token identifier holds the slot index in six
low bits and a never-reused generation. A check acquire-loads the fault word
and slot word, compares owner, mask and floors, and reloads the slot word
after an acquire fence; a mismatch returns INVALID. The writer stores a floor
rail's deadline before its state word (release); the reader acquire-loads
the state first. A reader therefore pairs a state with the same or a later
deadline, which at worst returns PENDING early. The clock registry adopts
this scheme without changing its 48-byte token or its 73 production status
callers. `a733_pmic_live` becomes two acquire loads.

## Reused and removed mechanisms

- **Reused:** the ledger API and retention, PMIC transactions and snapshots,
  the PCK600 delay row, clock scopes, the PCK gate lease, the CPU grant.
- **Removed:** clock scope `A733_POWER_DCDC2`; the GPU RAIL resource; ledger
  bridges, OPENING and exclusive mode; `a733_pmic_consumer.c`; public
  `a733_pmic_submit_write`; four duplicate PCK sequences; chip identity reads
  per request. The provider's per-phase clock and borrow queries become fault
  word loads; the provider never releases its S_TWI0 lease and alone ends its
  borrows, so coverage is unchanged.
- **Changed outside this package:** GPU resource and platform files, NPU
  platform, G2D, deinterlace, display framebuffer, HDMI, DisplayPort, the
  board PMIC loop and checks that build a ledger. No reserved CPU code.

## Performance budgets

Instruction and cache-line counts come from clang `-O2 -mcpu=cortex-a55` in
Unicorn emulation, in a new `checks/rail-authority-cost.py`, with 42 clock
and 16 rail leases, then 126 of each. Bus times come from
`checks/a733-pmic-polls.c`, extended to count transactions and spins: a read
takes about 95 µs and a write 70 µs at 400 kHz.

| Operation | Baseline | Target |
| --- | --- | --- |
| Rail status, one-rail token | 536 / 1,554 instructions; 60 / 233 lines | ≤ 40 instructions, ≤ 3 lines; +8 and +1 per further floor rail |
| Clock token status | 243 / 600 instructions | ≤ 30 instructions, 2 lines |
| Acquire, status and release | 4,026 / 7,773 instructions | ≤ 400 instructions, no bus access |
| Status during mutation | Lock; BUSY under contention | No BUSY or false READY in `checks/rail-status-contention.c` (4 readers, 1 writer, sanitizers, raise then early lowering) |
| Authority check per PMIC transaction | 818 / 2,179 instructions per phase | 2 acquire loads, ≤ 10 instructions |
| Ledger storage | 20,096 B | ≤ 2,560 B (static assertion) |
| Floor-rail refresh | Snapshot, one tick per poll | ≤ 0.45 ms at 400 kHz, ≤ 1.6 ms at 100 kHz |
| PMIC task wake, OS image | One tick | No tick wait (model); ≤ 50 µs (board) |
| CPU-B 800→870 mV, request to readback | About 60 polls, 60 ms (model) | 545 µs of bus: ≤ 0.65 ms at 400 kHz, ≤ 2.4 ms at 100 kHz |
| Same raise, request to READY | None | ≤ 0.85 ms at 400 kHz (280 µs settle); ≤ 2.4 ms at 100 kHz |
| Core time spun per write | Not applicable | ≤ 0.6 ms at 400 kHz |
| Domain power-on (1, 4, 5, 10) | GPU: 27 steps, one per poll | 2 PCK reads, write, readback, status in the 50 µs spin; 10 delay-row accesses once per boot |
| Domain power-down | GPU: 8 steps, one per poll | Local word reads, 2 PCK reads, write, readback, status |
| Power-authority calls per NPU job | 0 | 0 |

**Failure classes (section 1.8).** Refusals and BUSY precede any access and
retain nothing. Expiry before a domain write releases that domain's ledger
records. A bus fault, a spin at its bound, a readback mismatch or expiry
after a domain write leaves the state unknown: the provider and floor-rail
holders, or that domain with its holders and PCK lease, stay retained.

## Not included, with reasons

- **DCDC2 and DCDC4 writes:** no NPU point at 492 MHz needs more than
  800 mV, no grant covers DCDC2 and no GPU operating-point table exists.
- **Lowering below the baseline:** it needs every consumer of each rail.
- **Ledger refreshes in place of consumer snapshots:** 26 files outside this
  package would change.
- **A waiter for CPU frequency changes:** no production caller exists.
- **CPU cluster and core domains** (reserved code) and **domains 0, 2, 3
  and 6–9** (current owners stay).
- **The TWI_DRV packet engine** at S_TWI0 + `0x200`: one interrupt per
  transaction, so spun core time falls from about 0.55 ms to 60 µs per write
  at the same bus time. It is needed when a governor raises more than 10
  times per second or a measured phase spin exceeds its budget.

## Board evidence that would confirm the design

- DCDC3 or DCDC5 readbacks after a raise and a lowering, with request-to-
  readback time and the longest phase spin.
- PMIC page 0 and idle S_TWI0 after PSCI CPU_ON, CPU_OFF and suspend, and a
  boot-to-idle bus capture showing no other S_TWI0 master.
- The speed-bin word of the attached board.
- PCK600 status and time to ON for domains 1, 4, 5 and 10 across repeated
  cycles.
- PMU cycles for rail and clock status on an A55 and an A76, idle and during
  a mutation on the other cluster.
- A schematic source for the DE_SYS, G2D and deinterlace supply.
