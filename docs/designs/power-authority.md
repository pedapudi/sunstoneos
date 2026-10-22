# Design: one owner for supply rails and one for power domains

Status: **design**, not implemented. Physical behavior stays unqualified until
the board evidence listed at the end exists. Readers are assumed to know the
AArch64 memory model, FreeRTOS tasks and regulator voltage selectors.

Three objects currently decide who may use a supply rail of the AXP8191 power
controller (PMIC). The clock registry holds DCDC2 as scope bit 18. The GPU
resource manager holds DCDC4 as its RAIL resource. The rail ledger owns the 39
PMIC outputs and reaches DCDC2 and DCDC4 through private guards in the other
two objects. PCK600 power-domain sequences are copied into the GPU power
controller, the DE_SYS provider and the NPU platform, and domain authority sits
in clock-registry local scopes or GPU LOCAL ownership. Every token check
searches up to 128 slots under a try-lock, and a rail check nests two more
searches. The PMIC provider and the TWI adapter repeat these checks before
each bus phase.

## The rail ledger owns every output; a domain owner owns every PCK600 domain

The rail ledger becomes the only authority for all 39 outputs. The clock
registry refuses DCDC2 (bit 18 becomes reserved), and the GPU manager loses
its RAIL resource and guard slots 16–31. Without bridges, acquisition is
synchronous and the OPENING state disappears.

Each ledger holder record names an owner, a rail mask, a mode (shared or
exclusive), a voltage floor and a voltage ceiling. Each rail keeps an aggregate
record: holder count, exclusive holder, highest floor, lowest ceiling, the
programmed voltage from the last selector readback, and a state (STABLE,
CHANGING or UNCERTAIN). A request is admitted only when its floor does not
exceed any holder's ceiling and its ceiling is not below the highest floor.
When the programmed voltage already meets the floor, acquisition returns READY.
Otherwise it returns RAISING and queues the one permitted change for that rail.
The first holder of a rail triggers one selector observation that establishes
the programmed voltage. That observation replaces the platform-lifetime
accelerator observations in `a733_pmic_consumer.c` and the GPU and NPU snapshot
copies.

A new domain owner, `drivers/power/a733_power_domains.c`, holds one table entry
per domain: register block, the pinned pck600 delay row, supply rail, minimum
floor, and the local clock and reset words that must be off before power-down.
It keeps a reference count per domain. The 0→1 transition acquires a shared
ledger record for the domain's supply and then turns the domain on. The 1→0
transition turns it off and then releases that record. A domain found on at
boot is adopted with the same count but never turned off, because no drain
evidence exists for inherited traffic.

| Consumer | Rail | Floor and ceiling | PCK domain |
| --- | --- | --- | --- |
| Cortex-A55 cores and DSU | DCDC5 | Floor from each cluster's operating point | None (cluster power is reserved CPU code) |
| Cortex-A76 cores | DCDC3 | Floor from the operating point | None |
| GPU | DCDC4 | 800 mV fixed | 5 (GPU_TOP) |
| NPU | DCDC2 | Floor from the NPU operating point | 4 |
| Display engine, G2D, deinterlace | DCDC2 | 800 mV fixed | 1 (DE_SYS) |
| HDMI and eDP | DCDC2 | 800 mV fixed | 10 (VO1) |
| LCD and DSI | None identified in board sources | — | 9 (VO0) |

The NPU gains supply-voltage selection. Its startup reads the speed bin with
the eFuse decode that the CPU clock driver uses; that decode moves to a shared
header. The pinned operating-point table for the 26 MHz reference gives
852 MHz at 800 mV on bins vf0000, vf0200 and vf0201, and at 880 mV on vf0202. It gives 1008 MHz at
960 mV on vf0000 and vf0200 and at 1030 mV on vf0202. It lists no 1008 MHz
voltage for vf0201 and no 26 MHz voltage above 1008 MHz. On most bins the NPU
therefore moves from 492 to 852 MHz with no rail change. A request above 800 mV
conflicts with an active display holder, whose ceiling stays 800 mV until a
source gives the HDMI and eDP supply tolerance; the NPU then stays at 852 MHz.

## Power-up and power-down follow one order

1. The rail floor is READY: the selector readback meets the floor and the
   settle deadline has passed.
2. The PCK600 controller clock is held through the existing shared GPU-manager
   PCK lease.
3. The domain is on.
4. The consumer's clocks and resets are released under its clock-registry scope.

Power-down runs the steps in reverse. The domain owner reads the domain's local
clock and reset words before turning it off, as GPU release does today. A
frequency increase raises the floor before the PLL; a decrease lowers the PLL
before the floor. When the highest floor drops by at least one selector step,
the ledger queues a lowering write. A raise takes priority over a queued
lowering. The four DCDC outputs are always-on in the board device tree, so the
ledger changes selectors and never disables an output.

## A voltage change flows from the ledger through the PMIC task

A consumer acquisition that needs a raise stores a change request in its rail's
change slot and wakes the PMIC provider task with a task notification. The
provider runs the existing write request: six snapshot reads, comparison with
the expected selector, one write and a readback. Its permitted ranges are
DCDC3 and DCDC5 at 800–1050 mV and DCDC2 at 800–1030 mV, and its only caller is
the ledger's change path. The CPU clock driver stops writing selectors itself
and sets floors for CPU-L, the DSU and CPU-B. On completion the provider
reports the readback to the ledger. The ledger records the programmed voltage,
sets the settle deadline from the board ramp delay of 250 µV/µs, and notifies
each waiting holder's task. Any write fault or readback mismatch marks the rail
UNCERTAIN. All its holders then observe RETAINED and no further write is
attempted, which keeps the existing retention contract.

## Waiting uses notifications; only PMIC phases and PCK600 acknowledgement poll

- **Ledger and domain owner calls** never wait. They return READY, PENDING,
  BUSY or a refusal.
- **Voltage changes** are waited for with a task notification and the caller's
  absolute deadline as timeout, with no per-tick status polling. A 230 mV rise
  settles in 920 µs, within one 1 ms tick.
- **PMIC bus phases** stay tick-paced with the current eight-phase batching.
  When its queue is empty, the provider sleeps on its notification instead of
  polling every tick. That loop is in `apps/board/main.c`, outside this
  package. Interrupt-driven TWI completion belongs to the planned single TWI
  engine.
- **PCK600 acknowledgement** is polled, as in the vendor driver, because a
  transition happens a few times per session. One call issues all register
  writes under the register lock. It then checks status for up to 50 µs and
  afterwards once per tick until the existing 10 ms bound. The GPU controller
  today advances one register step per poll, at least 27 polls for power-on.

## Memory placement and cache policy

Both objects live in kernel `.bss`: Normal Write-Back, Inner Shareable, with
platform lifetime. No DMA master reads them, so they need no cache
maintenance. The first 64-byte line holds the lock word, the fault word and the
free-slot bitmap. The per-rail aggregates follow at 16 bytes each, then 128
holder slots of 64 bytes each. The ledger shrinks from 20,096 to about 9 KiB.
PCK600 and clock registers are reached through the `a733_mmio.h` helpers on the
existing Device mapping. Each write is followed by the readback the sequences
already perform, and no barrier is added.

## Synchronization across cores

Each object keeps one nonblocking try-lock: compare-and-swap with acquire
ordering, release store on exit, BUSY on contention and never a spin. The
ledger no longer calls into the clock registry or the GPU manager. The domain
owner marks a domain TRANSITIONING and drops its lock before calling the
ledger, so no lock nests.

A token identifier encodes its slot index in the low seven bits and a
generation above them. A status check reads one slot and compares four fields,
independent of occupancy. The clock registry adopts the same encoding without
changing its 48-byte token layout, so the `a733_clock_token_status` callers in
57 driver, application and platform source files become constant-time
without source changes.

A borrowed rail lease cannot be released while the borrow exists, because
release refuses. Between request admission and result publication, only a
ledger-wide or registry-wide fault can therefore withdraw authority. The PMIC
provider performs full exact validation at admission and before publication.
Before each bus phase it performs one acquire load of each fault word. The TWI
native adapter can use the same rule, which is a change outside this package.

## Reused and removed mechanisms

Reused: the ledger API and retention semantics, the PMIC write and readback
request, the pinned pck600 delay row, the clock-registry local scopes for
clocks and resets, and the GPU-manager PCK clock lease.

Removed: clock scope `A733_POWER_DCDC2`, the GPU RAIL resource and its guard
slots, the ledger bridges, the OPENING state and bridge health checks,
`a733_pmic_consumer.c`, the PCK steps in `a733_gpu_power.c` and
`a733_de_sys_power.c`, the NPU platform's domain writes, and the CPU clock
driver's direct selector write.

Files outside this package must change with it: `a733_gpu_resources.c` and its
header, `a733_gpu_platform.c`, `a733_npu_platform.c`, the G2D, deinterlace and
HDMI applications, `platform/a733/hdmi_native.c`, `platform/a733/native_usb.c`,
and the checks that construct a ledger. No reserved CPU startup code changes.

## Performance budgets

Baselines were measured on the current sources compiled with clang
`-O2 -mcpu=cortex-a55` and run under Unicorn AArch64 emulation. The typical
case holds 42 clock leases and 16 rail leases; the near-full case holds 126 of
each. The measured token is last in each table. Instruction counts include the
callee's lock and time calls; "lines" counts distinct 64-byte lines touched.

| Operation | Baseline (typical / near-full) | Target | Measurement |
| --- | --- | --- | --- |
| Rail status of a DCDC2 and DCDC4 lease | 536 / 1,554 instructions; 60 / 233 lines | ≤ 60 instructions, ≤ 3 lines, both cases | Emulation harness in `checks/rail-authority-cost.py` |
| Clock token status | 243 / 600 instructions | ≤ 40 instructions | Same harness |
| Authority check per PMIC bus phase | 818 / 2,179 instructions | ≤ 10 instructions | Same harness, and poll counts in `checks/a733-pmic-polls.c` |
| DCDC2 shared lease: acquire, poll and release | 4,026 / 7,773 instructions | ≤ 400 instructions, one acquire call | Same harness |
| Ledger storage | 20,096 bytes | ≤ 9,216 bytes | Static assertion |
| GPU domain power-on | 27 sequence steps at one per poll | One call plus the acknowledgement wait; no added register access | GPU power host model counts calls and accesses |
| NPU raise from 800 to 960 mV until READY | Unsupported | ≤ 60 provider polls plus one tick of settling | PMIC poll model; a snapshot takes 44 polls today |
| Power-authority calls per NPU job | 0 (checked once at service activation) | 0 | NPU startup model counts calls |

The functional host checks (`checks/rail-leases.c`, `checks/cpu-clock-model.c`
and the NPU startup model) extend to floors, ceilings, conflicts, ordered
transitions, reference counts, stale tokens in reused slots and retained
faults, under AddressSanitizer and UndefinedBehaviorSanitizer.

## Not included, with reasons

- **CPU cluster and core power domains** use reserved CPU startup code; CPU
  rails take part only through floors.
- **PCK domains 2, 3, 7 and 8** (video, USB and PCIe) keep their current
  sequencers. The domain owner refuses those indices, so no domain has two
  owners. Their packages can migrate later.
- **GPU voltage scaling**: the pinned sources contain no GPU operating-point
  table, so DCDC4 stays at 800 mV.
- **NPU above 1008 MHz** has no 26 MHz voltage in the table.
- **Raising DCDC2 while a display consumer holds it** waits for a sourced
  HDMI and eDP supply tolerance.
- **Lock-free status reads**: the remaining cost is one compare-and-swap per
  check, and a sequence lock would add a retry path for little gain.

## Board evidence that would confirm the design

- A PMIC selector readback of DCDC2 after a raise to 960 mV and after the
  lowering back to 800 mV.
- A voltmeter reading at the DCDC2 output, if a test point is reachable.
- An NPU convolution at 852 MHz whose output matches the reference.
- PCK600 status words for domains 1, 4, 5 and 10 after repeated on and off
  cycles, and a returning monitor after each cycle.
