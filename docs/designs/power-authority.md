# Design: one owner for supply rails and one for power domains

Status: **design**, not implemented. Physical behavior stays unqualified until
the board evidence listed at the end exists. Readers are assumed to know the
AArch64 memory model, FreeRTOS tasks and regulator voltage selectors.

Today the clock registry holds AXP8191 output DCDC2, the GPU manager holds
DCDC4, and the rail ledger reaches both through bridges. The PCK600
power-domain sequence exists in five places: the GPU power controller, the
DE_SYS provider, the NPU platform, `platform/a733/hdmi_native.c` and
`apps/displayport/native.c`.

## The rail ledger owns every output; a domain owner owns four domains

The rail ledger becomes the only authority for all 39 PMIC outputs. The clock
registry refuses DCDC2 (scope bit 18 becomes reserved), and the GPU manager
loses its RAIL resource and guard slots 16–31.

A holder record names an owner, a rail mask, a mode (shared or exclusive) and
a voltage floor. A floor lives as long as the rate or domain it protects. Each
rail records a **baseline**, the voltage observed when the ledger adopts it.
Adoption uses the NPU startup check for every rail: two selector observations
at least 5,160 µs apart (full-range ramp plus enable delay from the board
device tree), once per rail per boot.

- **Raise.** Only DCDC3 and DCDC5 are written, and only to meet a CPU floor
  that the CPU session grant covers. Any other rail is READY when its
  programmed voltage meets the floor and is refused otherwise.
- **Lower.** The ledger only undoes its own raises: the target is the larger
  of the baseline and the highest floor. A rail with no holders keeps its
  programmed voltage.
- **READY** requires floor ≤ programmed voltage, no change in flight, and a
  passed settle deadline. Otherwise the result is PENDING.
- **Release during a raise** always succeeds, because the change slot holds
  its own authority. After the write completes, the ledger queues any
  lowering that the remaining floors allow.

The CPU clock driver keeps one persistent floor holder per cluster and for
the DSU and changes each floor in place. Its final step no longer releases
the rail. A frequency increase raises the floor before the PLL changes; a
decrease changes the PLL first and lowers the floor afterwards.

A new domain owner, `drivers/power/a733_power_domains.c`, holds one entry per
domain: register block, the pinned PCK600 delay row, rail floors, and the
local clock and reset words that must be off before power-down. It counts
references per domain and holds one GPU-manager PCK lease while any count is
above zero. Consumers that held a PCK client slot only to power these domains
drop it.

| Domain | Consumers | Rails and floors |
| --- | --- | --- |
| 1 (DE_SYS) | Display engine, G2D, deinterlace | DCDC2 ≥ 800 mV (assumption, see evidence) |
| 4 (NPU) | NPU startup | DCDC2 ≥ the NPU point's voltage |
| 5 (GPU_TOP) | GPU | DCDC4 ≥ 800 mV |
| 10 (VO1) | HDMI | DCDC2 ≥ 800 mV; CLDO2 at its observed voltage |
| 10 (VO1) | DisplayPort | DCDC2 ≥ 800 mV; CLDO5 ≥ 1800 mV |

The CPU clusters hold DCDC5 (CPU-L and DSU) and DCDC3 (CPU-B) floors without a
domain. The owner refuses domains 0, 2, 3 and 6–9 until a consumer powers one
through it. The DSI path only checks that domains 0, 1 and 9 are on and keeps
its PCK client slot for those reads.

## Power-up and power-down follow one order

1. The domain's rail floors are READY.
2. The domain owner's PCK lease is held.
3. The domain is on.
4. The consumer releases its clocks and resets under its clock-registry scope.

Power-down runs in reverse after the owner checks the local clock and reset
words. A domain found on at boot is adopted with a count of one and never
turned off, because no drain evidence exists for inherited traffic. While a
domain is TRANSITIONING, other consumers receive BUSY. A deadline that expires
before the domain write releases the ledger records and returns the domain to
OFF. Expiry of the deadline or the 10 ms acknowledgement bound after the
domain write marks the domain RETAINED, with no further access.

## NPU supply-voltage selection supplies the floor; the rate stays 492 MHz

NPU startup reads the speed-bin code from eFuse word `0x0300624c` with the CPU
clock driver's extraction, which moves to a shared header. The pinned
`vf_mapping_table` maps the code to a column name, and each table looks up
its own column:

- Codes `0x02`, `0x12`, `0x03`, `0x04`, `0x05`, `0x14` and `0x15` select NPU
  columns vf0200, vf0201, vf0300, vf0400, vf0500, vf0401 and vf0501.
- Codes `0x01`, `0x11` and `0x06` are refused: the NPU table has no vf0100,
  vf0101 or vf0600 column.
- Code 0 also means an unprogrammed fuse, so it uses the CPU driver's
  envelope: a point is admitted only when every NPU column lists it, at the
  highest voltage. This admits 492 MHz at 800 mV and refuses 852 MHz, which
  vf0205, vf0206 and vf0300 lack.

The second board's word `0x00000496` decodes to code `0x04` (vf0400): 492 and
852 MHz at 800 mV, nothing faster. Faster points exist only in other columns;
for example, vf0401 lists 852 MHz at 820 mV and 1044–1188 MHz at 1000–1140 mV.
For every column a code reaches, the 24 MHz and 26 MHz rows agree.

852 MHz is not an integer multiple of the 24 MHz PLL_NPU reference and needs
PLL_NPU retuned to 1704 MHz. PLL_NPU also feeds the MBUS, video and DRAM0
selectors, so that retune belongs to a PLL_NPU owner that excludes those
consumers. Until it exists, the NPU stays at 492 MHz and this package supplies
only the floor and the order.

## A voltage change flows from the ledger through the PMIC task

A floor raise on DCDC3 or DCDC5 stores a target in the rail's change slot and
notifies the PMIC task. The provider accepts writes only from the ledger's
change slot, which replaces today's exclusive consumer lease. A raise target
must lie between the baseline and the voltage of the highest point the CPU
grant admits, within 800–1050 mV. A lowering needs no grant, because it only
undoes a granted raise.

A write request reads the page (`0xff`), the identity (`0x0e` and `0x0f` in
one two-byte auto-increment read), the enable and the selector, then the page
again, and then writes and reads back the selector. The page reads guard the
write's target address. The identity read confirms that address `0x36` still
answers as the AXP8191; `baseline()` checks only SoC-side clock, pin and
controller state. A fault or readback mismatch marks the rail UNCERTAIN, its
holders observe RETAINED, and no further write occurs.

## Waiting uses interrupts and notifications; only PCK600 acknowledgement polls

- **TWI phases.** The provider sets INT_EN (bit 7) of the TWI control
  register, writing `0xc8` where it writes `0x48` today. The S_TWI0 handler
  (GIC interrupt 235) disables the interrupt at the GIC and notifies the PMIC
  task, which re-enables it before the next wait. The STOP wait stays a
  busy-wait of about one bit time. The idle task sleeps on its notification
  instead of calling `taskYIELD` or `vTaskDelay(1)` in `apps/board/main.c`.
- **Ledger and domain-owner calls** never wait. They return READY, PENDING,
  BUSY (mutations only) or a refusal.
- **Rail waiters** use notification index 1 with set-bits semantics; index 0
  stays with interrupt users such as the NPU service. The holder slot records
  the waiting task, the ledger notifies after releasing its lock, and the
  waiter rechecks status and its absolute deadline after every wake. A settle
  remainder of 100 µs or less is busy-waited; a longer one sleeps whole ticks.
- **PCK600 acknowledgement** is polled for up to 50 µs, then once per tick up
  to 10 ms. The A733 manual lists PCK600 interrupts 221 (Q-channel) and 244
  (CPU idle) and documents neither as a domain-transition completion. The
  vendor driver also polls.

## Memory placement and cache policy

Both objects live in kernel `.bss` (Normal Write-Back, Inner Shareable,
platform lifetime). No DMA master reads them, so they need no cache
maintenance. Lines that status reads are kept apart from lines that
mutations write:

| Ledger region | Size | Written by |
| --- | --- | --- |
| Lock word and 128-bit free-slot bitmap | 64 B | Every acquire and release |
| Fault word | 64 B | A fault only |
| Rail status: state word, settle deadline (16 B × 39) | 640 B | Voltage changes |
| Rail bookkeeping: count, exclusive holder, floor, baseline, change slot (16 B × 39) | 640 B | Acquire, release, floor change |
| Holder slots: identifier, owner, mask, mode, floor, waiting task (64 B × 128) | 8,192 B | That slot's acquire and release |

The total is 9,600 bytes, down from 20,096. PCK600 and clock registers use the
`a733_mmio.h` helpers on the existing Device mapping, with the existing
readbacks and no added barrier.

## Synchronization across cores

Acquire, release, floor change and domain transitions keep one nonblocking
try-lock per object: compare-and-swap with acquire ordering, release store on
exit, BUSY on contention, no spin and no nesting. The domain owner marks a
domain TRANSITIONING and drops its lock before calling the ledger.

Status checks take no lock and never return BUSY. A token identifier holds its
slot index in the low seven bits and a never-reused generation above them.
Under the lock, a grant writes the slot fields and then publishes the
identifier with a release store; a release stores zero. A status check:

1. acquire-loads the fault word;
2. acquire-loads the slot word and compares it with the token identifier;
3. compares owner, mask and mode, read with relaxed atomic loads;
4. reloads the slot word after an acquire fence.

Any mismatch means the token is no longer live, so the result is INVALID with
no retry. Rail status also loads the state word and the settle deadline. The
deadline is stored first, so a reader that sees parts of two updates can
report PENDING early but never a false READY. The clock registry adopts the
same scheme without changing its 48-byte token, so the 57 source files that
call `a733_clock_token_status` do not change. The ledger's fault word becomes
an atomic with release stores. The PMIC provider checks each bus phase with
one acquire load of each fault word.

## Reused and removed mechanisms

- **Reused:** the ledger API and retention semantics, the PMIC write and
  readback request, the pinned PCK600 delay row, clock-registry scopes, the
  GPU-manager PCK lease, the CPU session grant, and the 5,160 µs guard.
- **Removed:** clock scope `A733_POWER_DCDC2`; the GPU RAIL resource; the
  ledger bridges and OPENING state; `a733_pmic_consumer.c` and the
  `pmic_view` binding, which is deleted rather than ported; the PCK steps and PCK
  client slots in the GPU, DE_SYS, NPU, HDMI and DisplayPort code; and the
  CPU clock driver's direct selector write.
- **Changed outside this package:** `a733_gpu_resources.c`,
  `a733_gpu_resources_native.c`, `a733_gpu_platform.c` and their headers;
  `a733_npu_platform.c` and its native adapter; the G2D, deinterlace, HDMI
  and DisplayPort applications; `hdmi_native.c`; `native_usb.c`;
  `apps/board/main.c`; interrupt route tables of images with the PMIC
  provider; `configTASK_NOTIFICATION_ARRAY_ENTRIES` set to 2 in
  `include/FreeRTOSConfig.h`; and checks that construct a ledger. If the
  planned single TWI engine takes S_TWI0 first, the interrupt route moves to
  it. No reserved CPU startup code changes.

## Performance budgets

Instruction baselines come from the current sources (clang `-O2
-mcpu=cortex-a55`, Unicorn AArch64 emulation) with 42 clock and 16 rail leases,
or 126 of each. At 400 kHz a one-byte register read takes about 100 µs, the
two-byte read 120 µs and a write 73 µs, so a write request takes 0.70 ms
(2.8 ms at 100 kHz). The PMIC model reads the inherited TWI divisor and
checks both rates.

| Operation | Baseline (typical / near full) | Target | Measurement |
| --- | --- | --- | --- |
| Rail status, DCDC2 and DCDC4 lease | 536 / 1,554 instructions; 60 / 233 lines | ≤ 40 instructions, ≤ 3 lines, no read-modify-write | Unicorn harness `checks/rail-authority-cost.py` |
| Clock token status | 243 / 600 instructions | ≤ 30 instructions, 2 lines | Same harness |
| Status from 4 threads while 1 acquires and releases | Not measured; each check takes the lock | No BUSY; p99 ≤ 2 × uncontended median; no store to another lease's status lines | Host pthread model with C11 atomics, `checks/rail-status-contention.c` |
| Authority check per PMIC bus phase | 818 / 2,179 instructions | 2 acquire loads, ≤ 10 instructions | Unicorn harness |
| DCDC2 shared lease: acquire, poll, release | 4,026 / 7,773 instructions | ≤ 400 instructions | Unicorn harness |
| Ledger storage | 20,096 B | ≤ 9,728 B | Static assertion |
| CPU-B 800→870 mV (1196→1716 MHz, vf0400): request to readback | About 60 polls, 60 ms at one tick each (model) | ≤ 2.2 ms: 0.70 ms bus, 41 interrupts at ≤ 10 µs, ≤ 1 ms wake | `checks/a733-pmic-polls.c` with interrupt completions |
| Same raise: request to READY at the waiter | Not available | ≤ 3.5 ms: readback, 280 µs settle at 250 µV/µs, ≤ 1 tick rounding | PMIC and ledger host models |
| GPU domain power-on | 27 steps, one per poll | One call plus acknowledgement wait | GPU power host model |
| Power-authority calls per NPU job | 0 | 0 | NPU startup model |

The rail, CPU clock and NPU startup host checks extend, under AddressSanitizer
and UndefinedBehaviorSanitizer, to persistent floors, release after and
during a raise, adoption above the floor, rails with no holders, ungranted
writes, stale tokens, every speed-bin code, domain expiry on both sides of
the domain write, and retained faults.

## Not included, with reasons

- **DCDC2 and DCDC4 writes:** no reachable NPU point needs more than 800 mV,
  no grant covers DCDC2, and no GPU operating-point table exists. Display
  holders therefore need no voltage ceiling.
- **Lowering below the baseline:** it needs a complete consumer list per rail.
- **CPU cluster and core domains:** they belong to reserved CPU startup code.
- **A one-shot settle timer:** raises occur only on granted CPU transitions.

## Board evidence that would confirm the design

- DCDC3 or DCDC5 readbacks after a granted raise and after the lowering, with
  the counter time to readback and 41 S_TWI0 interrupts per write.
- The speed-bin word of the board on USB port 3-1.
- PCK600 status for domains 1, 4, 5 and 10 across repeated on and off cycles.
- A schematic source for the DE_SYS, G2D and deinterlace supply; the board
  device tree names DCDC2 only for the NPU, HDMI and eDP.
- HDMI tolerance of the CPU-B voltage range; the device tree also names DCDC3
  as the HDMI `cpu-supply`.
