# Design: NPU cold boot, recovery and power management

Status: **design**; no code has changed. Every item ends implemented and
host-modeled, with physical behavior unqualified until the board evidence at
the end exists. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

The design covers NPU startup ([a733_npu_startup.c](../../drivers/npu/a733_npu_startup.c)),
the local clock and reset controller ([a733_npu_platform.c](../../drivers/npu/a733_npu_platform.c)),
the engine controller ([a733_npu_controller.c](../../drivers/npu/a733_npu_controller.c)),
the PLL controller ([a733_npu_pll.c](../../drivers/npu/a733_npu_pll.c)) and the
convolution application in `apps/npu-convolution`. Every image that runs NPU
startup is built from that application: the multicore build, the single-core
build, and the images built on the single-core one (`npu-runtime`,
`npu-finite` and the accelerator image). Counted figures below use the
accelerator image, whose model buffers are the largest
(`SUNSTONE_NPU_NN_POOL_GRAPH` in `include/npu_convolution_layout.h`).

## Admission reads one record from the private copy of the session page

The host launcher places a session page at `0x41f00000`. The existing
authentication stays: the EL3 contract carries a SHA-256 of the page, and
`a733_smp_contract_valid` checks it with the build ID, epoch, generation,
lease, window and deadlines; `npu_session_copy` (or `npu_single_core_seal`)
then copies the page into retained private memory and hashes the copy again.
Startup reads only that copy (`npu_session_get`), never the live page, which a
later writer could change between check and use. The contract's comparison of
its `reserved[0]` with the page's supervisor field is removed: the page digest
and the epoch, generation and lease equalities already bind the page to the
contract.

The page carries the **cold-boot record**: image digest, application boot
profile, whole-platform reservation, and the register state boot leaves:

- local module and bus gates closed; core, AXI and AHB resets asserted; SRAM
  reset released;
- domain 4 OFF, command and status;
- PLL_NPU either **disabled** (enable, LDO, lock detector, output and LOCK
  clear) or **locked at 492 MHz** with the exact control, gate (`0x1920`) and
  output-status (`0x19a0`) words that the enable sequence leaves.

Any other PLL_NPU state is refused before the first write. What boot leaves on
the attached board is not recorded; the first item of the board evidence
records it. If boot leaves PLL_NPU locked at another rate, the existing retune
sequence returns as the startup PLL step.

**Domain 4 found on is refused.** The shared SRAM aliases
(`0x74000`–`0xf3fff`) are powered by domain 4. With the domain verified OFF,
no bootloader, FEL continuation, CPU or DMA user can be running from or
holding data in them. A register read cannot show that SRAM was abandoned when
the domain is found on, so the quiescent-ON adoption is deleted with its
supervised-handoff test. The finite and returning images keep their return
path: after CLOSE, the local stop, the interrupt-route stop and the quiescence
check, they enter a new ROM instance by warm reset, and the host never
restores earlier SRAM contents. Domain 4 stays on through that return, as it
does today.

**One register pass.** Before its first write, startup reads the 24 MHz
reference (two words), the PERI0 parent and its gate words (five), the 13
PLL_NPU consumer snapshots (which include the local CCU words `0xb00` and
`0xb04`), PLL_NPU control and its four tuning words, `0x1920`, `0x19a0`, and
the two domain-4 status words: 29 reads under one register-lock acquisition, compared with the record copy. This pass
replaces three overlapping ones: the PLL controller's `baseline()`, the
application adapter's `parents()` (a copy of the PERI0 policy) and the
platform's `parents_stable()` with its own baseline.

## Startup owns one token and runs the PLL enable as a step

The power-authority design (`docs/designs/power-authority.md` on its own
branch) gives startup a DCDC2 rail token and a domain-4 row. Startup becomes:

1. Acquire the DCDC2 token and one clock-registry token: exclusive PLL_NPU and
   NPU-local scopes, shared reference, oscillator, fabric and PERI0.
2. Run the register pass.
3. If PLL_NPU is disabled, program it with the method-2 enable sequence
   under the same lock acquisition. This is the sequence of the mode now named
   `LIVE_ENABLE` (LOCK clear, live DDRPLL and PHY routes), kept and renamed
   `ENABLE`. Release the lock. Spin on LOCK: three observations 3 µs
   apart, then 20 µs, at most 100 observations. The spin holds no lock,
   because the exclusive PLL_NPU token makes startup the only writer of the
   control word and the register lock serializes writers.
4. Take the lock once: open the PLL output and read it back, release the AHB,
   AXI and core resets, set the module divider, open the bus and module
   gates, read back. Release it. This keeps the clock-before-power order of
   the pinned Allwinner driver.
5. Request domain 4 from the owner, wait 2 ms after it reports ON, and check
   the final local state under a third lock acquisition.
6. Open the engine controller and activate the service.

A locked 492 MHz PLL is adopted with no write, and step 3 is skipped.

**Supply.** Startup no longer reads the PMIC over S_TWI0 or waits 5,160 µs
between two DCDC2 snapshots. The domain owner reads DCDC2 enable and selector
after the power authority's first-snapshot settle deadline (provider start
plus 5,160 µs), and startup continues only when the owner reports ON. The
owner spins up to 50 µs for the PCK600 acknowledgement with no lock held, then
returns PENDING; startup retries once per tick for up to 10 ms.

**Removed, with the check that still covers each:**

| Removed | Covered by |
| --- | --- |
| PLL modes `COLD_ENABLE` (pre-DRAM, no image has the SRAM-resident code) and `ENABLED_RETUNE` (until the 852 MHz point) | Record admits disabled or locked-at-492 MHz only |
| Quiescent-ON adoption: the `LIVE_ENABLE` branch in `bind_pll`, `a733_npu_platform_adopt_on`, `adopt_on` | Domain 4 required OFF by the register pass |
| PLL controller `acquire` callback, `struct a733_npu_pll_resources`, token transfer through `a733_clock_publish_stable`, successor binding | Startup's single token from step 1 |
| Startup fields `pll_source`, `pll_binding_self`, `pll_config`, `pll_operations`, `pll_cookie`, `pll_transferred`, `pll_whole_platform`; `pll_integrity`, `pll_config_equal`, `pll_ops_equal`; `a733_npu_startup_bind_pll` | PLL config read from the record copy, which nothing writes after the copy |
| The no-PLL acquisition path (generic CLOCK stage) | Same token; every image has a PLL step |
| `apps/npu-convolution/clock.c`: `context()`, `parents()`, `same_config()` and the adapter | Register pass; ownership checks below |
| `task_owner()` in `read_reg`, `write_reg`, `enter`; `exact_config` per acquire | One ownership and EL check per startup call |
| `bind_pmic`, `supply_sample`, PMIC consumer and health query; `rail_snapshot`, the I2C bus object, `i2c_held`, `rail_step`, `rail[]` | Rail token and the domain row's supply read |
| PCK600 lease `pck_token`, `struct a733_npu_startup_resources` and its GPU resource-manager wrapper | Domain owner holds the PCK600 bus-clock lease |
| Handoff flags, `A733_NPU_HANDOFF_*`, the `A733_NPU_SUPERVISED` option, the multicore `A733_NPU_HANDOFF_SOURCE` file, the page's evidence, clock, power and supervision digests and supervisor field | Authenticated record and register pass |
| `prepared`'s backend comparison and live token status read | Nothing revokes the tokens before activation; only the bootstrap task holds them |
| Hash of every model, constant and stream on each startup poll and at activation (`unchanged(c,0)` in `npu_convolution_startup_poll` and `prepared`; `all_intact` in `npu_session.c` `platform_step`) | First bootstrap publication hashes the same bytes before the device reads them; a startup poll touches only CCU and domain registers |

**Progress per call.** The bootstrap task runs steps until one waits on
hardware. The only tick wait on the normal path is the 2 ms domain settle,
which section 1.3 permits.

## The service worker owns the engine after activation

After activation the NPU service worker is the only owner of the engine
controller, the local CCU fields and idle gating; the bootstrap task blocks.
Controller, startup and service state live in kernel `.bss` (Normal
Write-Back, Inner Shareable), written by their owning task only, and read by
no DMA master.

**Placement and priority.** EL3 routes SPI 65 (GIC interrupt 97) only to
CPU0, so the worker runs on CPU0 at priority 4 of 8, and a completion
notification wakes it with no inter-processor interrupt. The single-core
images bind the interrupt today. The multicore build does not: its worker runs
on core 1 and polls. It gains the same binding (the `apps/npu-interrupt` route
installer and a CPU0 worker), after which the unbound poll branch in
`npu_service.c` has no consumer and is removed. In single-core images the
client also runs on CPU0. In the multicore build it runs on core 1, and on
core 6 or 7 once those cores are admitted.

**Cross-core synchronization.** Requests and replies travel through FreeRTOS
queues, whose kernel critical sections order the data. The worker reads the
thermal level with one acquire load per wake. The bootstrap task signals
readiness and local stop with a release store followed by a task notification
to the client.

**Client waits.** The convolution client stops polling on ticks. It blocks on
the bootstrap task's notification for readiness and for local stop, holds an
`NPU_SERVICE_WAIT` after OPEN until READY and after SUBMIT until DONE, and
sends no STATUS request. REAP folds into the owner's next SUBMIT or CLOSE,
which reap a DONE job first, because sending either states that the owner has
finished reading the output. A job then costs two round trips, SUBMIT and the
completion reply, as section 3 requires. The finite and runtime bootstrap
loop that waits for `closed` once per tick is replaced by the same
notification.

**Completion.** The interrupt is the completion path. Delivery is unqualified
on the board, so the worker waits on its notification for at most 10 ms or
until the job deadline, whichever is sooner. A fallback wake and the deadline
sample are each one call to `a733_npu_poll`, the same pass an interrupt
runs: read `0x10`; on DONE, require full idle (`0x7fffffff`), read `0x10`
again with no other bit set, and read back the configuration, then the
barrier and `finish()`. DONE without full idle stays latched in `irq_seen`
until the next wake. A completion found without its interrupt increments one
missed-interrupt counter. The fallback is removed when the board shows one
interrupt per job over the gate-and-wake run. The convolution application sets
a 1 s job deadline.

**Per-job register work.** The configuration readback (`0x3b4`, `0x388`,
`0x14`) runs at the start trigger and at completion only. Both stay, because
other EL1 code using the shared device mapping can write these registers
between jobs, and the completion readback is the only proof, before output
returns to the CPU, that the MMU stayed on with the same page directory.
The per-access `DSB SY` accessors stay until the qualification that
[docs/shared-helpers.md](../shared-helpers.md) requires; then each job keeps
`DMB OSHST` before the start write at `0x3a4` and `DMB OSHLD` after the
full-idle read. The `DSB SY` that completes cache maintenance stays.

**Per-job CPU0 work beyond registers** happens in `publish` and `finish`
(owned by `npu_convolution.c`, `npu_session.c` and `a733_npu_memory.c`):

| Work per job, accelerator image | Today, counted | After this design |
| --- | --- | --- |
| SHA-256 of NN and coefficient buffers (`unchanged(c,1)`, `model_intact(s,m,0)`) | 917,504 bytes, plus the stream payload and an END scan of the rest of the 524,288-byte command allocation | Kept until the read-only mappings exist; then 0 |
| `AT` translations (`live_mappings` in publish and finish, `register_access`) | 1,063 per walk × 2 + 13 = 2,139 | 0: the page tables are sealed before admission, which still translates each mapping once; the register window is checked at initialization |
| MMU table scan (`canonical`) | 4,352 words and 3,072 padding bytes | Kept until the arena is mapped read-only; then 0 |
| Cache maintenance | Clean 151,552 (input); clean-invalidate and later invalidate 1,097,728 each (pool and output): 2,347,008 bytes | CPU0 invalidates the output bytes only (4,096) |

The estimated digest cost is 7.6–12.7 ms per job at 1.8 GHz and 28–46 ms at
494 MHz (section 1.4). It serializes every client behind CPU0 until the
read-only mappings remove it. With the large-graph build the digest is 8,192
bytes and the maintenance 3,153,920 bytes.

**Maintenance placement.** The client cleans the input bytes it wrote, on its
own core, followed by `DSB` before the SUBMIT send; `DC CVAC` by address
reaches the point of coherency from any core. The worker keeps the output
invalidation, because it runs after the full-idle read and its barrier, in
the same sequence that observed completion. The CPU never writes the output
buffer after admission, so it needs no clean before a job. In single-core
images the client also runs on CPU0, so the move changes which task pays, and
the CPU0 total is unchanged there. The pool is device scratch that
the CPU never reads. Large builds clear it on every SUBMIT
(`npu_convolution_send`); while that clear exists, the client cleans the
1 MiB it cleared and nothing invalidates the pool. The clear is removed when
the board shows identical outputs from a pool prefilled with zeros and with
`0xa5`. Then the pool is cleaned and invalidated once at admission and
receives no per-job maintenance.

## Idle gating closes the engine and its local clocks

Idle gating is enabled only after the primary agent maps admitted model,
coefficient, stream and MMU-table regions read-only at EL1 and EL0. Under
those mappings no CPU code can write a region the device reads, and gating
clocks does not change DRAM. Each writer cleans its own range where it
publishes it: admission cleans the constant regions and tables once, and the
client cleans its input per job. A wake therefore runs no hash, no
translation, no table scan, no clean and no invalidation.

Today every wake would run the full bootstrap publication. `unchanged(c,0)`
hashes 1,881,936 bytes plus the stream payload (model copy, NN, coefficients,
both streams) and END-scans two 524,288-byte allocations. The memory layer
adds 2,126 translations, the table scan, 3,235,840 bytes cleaned or
clean-invalidated, and 1,097,728 bytes invalidated by `finish`. The estimated hash alone costs
16–26 ms at 1.8 GHz. The wake budgets below hold only without that work.

Worker waits: while ungated and idle, a 100 ms timeout; once gated, it blocks
until a request arrives. The 1000 ms counter-extension wake is removed,
because `counter_clock_sample` accepts samples up to 2^55 ticks apart (about
47 years at 24 MHz). After 100 ms without a job, the worker closes the
controller (interrupt reporting and MMU disabled with readback, full idle
verified) and masks interrupt 97 without stopping its route. It then gates
the NPU under one lock acquisition: module gate, bus gate, then AHB, AXI and
core resets, the stop sequence of the pinned `npu_clk_uninit`. Domain 4, the
PLL and the rail stay on. The final CLOSE in the finite and returning images
still stops the route and waits for `npu_service_irq_stopped` before it
reports quiescence.

The next submit reverses the sequence: six local writes, then controller open,
which runs the two-pass soft reset (280 µs of spin delays) and the 208-byte
bootstrap stream. Submits that arrive during a wake are queued as submits
during a running job are queued today (`MEMBER_QUEUED`, `dispatch`) and start
when the bootstrap completes. Refusal before device access remains only for
invalid commands and the Critical thermal level.

**CPU0 occupancy.** The reset spins and the cancel spins below run at worker
priority with interrupts enabled and without the platform register mutex; the
engine registers are not CCU registers. CPU0 is unavailable to tasks at
priority 4 or lower for at most 0.35 ms per wake and 1.1 ms per cancel.

## A hung job is cancelled and the engine restarted

The worker owns the job and cancel deadlines. `a733_npu_check_timeout` and the
elapsed test in `a733_npu_advance` are deleted; the open sequence keeps its
own bound.

1. **Job deadline.** The worker masks interrupt 97 and runs one
   `a733_npu_poll` pass. DONE with full idle finishes the job normally and
   increments the missed-interrupt counter. Otherwise the worker calls
   `a733_npu_cancel`. A DONE that arrives after this pass is reported as
   TIMEOUT.
2. **Cancel budget, a hard fault.** The worker spins through the 100 µs
   cancel assertion and then for up to 1 ms until both the acknowledgement
   (`0x3a8` bit 4) and full idle are seen. Failing that, it polls once per
   tick up to 20 ms from assertion, the vendor's 200 × 100 µs bound in
   `vipdrv_task_recover`, and then faults.

After both are seen: CANCELED (`finish` invalidates the output and hands the
buffers back), `a733_npu_reap`, `a733_npu_close`, then `a733_npu_open` with the
soft reset and bootstrap stream. The vendor recovery resubmits the init
command without a reset; this design keeps the reset, because open is the
only qualified path from an unknown engine state. The job reports TIMEOUT.
One per-session cap of four recoveries bounds hangs; the fifth timeout
retains. At a 1 s deadline a model that always hangs retains after about 5 s.

| Failure | Class (section 1.8) | Retained |
| --- | --- | --- |
| Invalid command, Critical thermal level | Refused before device access | Nothing |
| Deadline, cancel acknowledged with full idle | Known terminal state | Nothing; recovery runs |
| No acknowledgement within 20 ms, an event bit other than DONE, MMIO failure, recovery cap, failed CCU readback | Unknown | Session memory and MMU tables, clock and rail tokens, domain row; the register lock only if a CCU sequence stopped midway |

## Operating points: refuse under Critical until the speed bin is known

The worker refuses submits while the thermal level is Critical and runs at
492 MHz otherwise. The 852 MHz point is implemented only after the board's
SID speed bin is read and qualifies. In the 24 MHz reference columns of the
pinned `sun60iw2p1-cpu-vf.dtsi`, 852 MHz needs 800 mV for bins vf0200, vf0201,
vf0400, vf0500 and vf0501 (vf0000 also, but an unprogrammed fuse decodes to
it). Points above 800 mV are refused, because DCDC2 also supplies eDP and
HDMI. The point is a 1,704 MHz oscillator (multiplier 71), P0 = 2 and module
divider 1. The retune sequence returns with it, runs only with the controller
CLOSED and the local gates closed (the idle-gating path), and a failed
readback retains the PLL and clock tokens.

## Model loading from the card is deferred

No catalogue model needs the card; the largest, ShuffleNet V2, is 964,224
bytes. When a model set exceeds the image's linked budget:

- **CPU copy.** The storage owner replaces the byte loop in `sunxi_mmc.c`
  `fifo_step` with word stores, and the SHA-256 runs per 4 KiB block during
  the copy into the model slot.
- **SMHC internal DMA** straight into the slot: clean and invalidate the slot
  before the transfer, invalidate after it, then hash the final bytes once the
  last write completes and the slot has no writer.
- **Ownership.** The slot is cache-line aligned, shares no line with another
  owner's data, is unmapped from the NPU during the write, and passes to the
  session by a release store that the session acquires.
- **Hashing.** The portable SHA-256 stays until `ID_AA64ISAR0_EL1` is recorded
  on the A55 and A76 cores; if every core reports SHA2, the build selects the
  instruction implementation.

## Budgets

*Measured* baselines come from the host models at commit `041c09a` with a
counting wrapper (one poll per modeled millisecond); *counted* ones from the
code. The implementation adds these counters to `checks/npu-platform.c`,
`checks/npu-pll.c`, `checks/npu-controller.c` and `checks/npu-startup.c`.

| Metric | Baseline | Target |
| --- | --- | --- |
| Startup calls, acquisition to READY | Platform 17 measured; PLL 13 counted | ≤ 3; one tick wait |
| Startup register reads and writes | Platform 51 and 12 measured; PLL and adapter about 58 and 7 counted | ≤ 60 reads excluding LOCK samples; ≤ 12 writes (6 when adopted) |
| Counter-clock reads at startup | Platform 190 measured; PLL about 110 counted | ≤ 16 plus one per LOCK sample |
| Register-lock acquisitions at startup | Platform 15 measured; PLL 13 counted | ≤ 3 |
| Ownership checks per register access | `task_owner` (startup); EL read, scheduler query, task compare and health query (adapter) | 0; one per startup call |
| Bytes hashed before activation | About 30 calls × 1,881,936 bytes, about 56 MB, counted | 0; the first bootstrap publication hashes once |
| Startup PMIC traffic | 12 register reads, 6 bus-state reads, 5,160 µs wait | None |
| Source size | Startup 436 + 131 lines; PLL 204 + 109; adapter 130 | Startup ≤ 250 + 70; PLL ≤ 120 + 50; adapter deleted; `sizeof(struct a733_npu_startup)` smaller by the removed fields |
| Bootstrap ticks to activation | About 42 counted | ≤ 4 without PENDING |
| Local gate | 6 polls, 102 reads, 5 writes, 238 clock reads, 6 lock acquisitions, measured | 1 call, ≤ 12 reads, 5 writes, ≤ 6 clock reads, 1 lock |
| Per-job digest | 917,504 bytes plus stream | Same until read-only mappings; then 0 |
| Per-job `AT` | 2,139 | 0 |
| Per-job table scan | 4,352 words | Same until read-only arena; then 0 |
| Per-job cache maintenance on CPU0 | 2,347,008 bytes | 4,096 bytes (output) |
| Per-job maintenance on the client core | 0 | Input bytes written (151,552); plus 1 MiB pool while the clear exists |
| Engine register accesses per job | 11 reads and 2 writes, plus 5 reads per poll, measured | 11 and 2; one `a733_npu_poll` pass per 10 ms fallback |
| `DSB SY` per job | 17 counted | 14; 2 `DMB` after qualification |
| Client round trips and tick polls per job | 3 round trips plus one STATUS poll per tick | 2 round trips; 0 tick polls |
| Client tick polls during startup and local stop | One per tick | 0 |
| Wake publication work | 1,881,936 bytes hashed, 2,126 `AT`, 3,235,840 bytes cleaned, 1,097,728 invalidated | 0 of each |
| Wake, gated to bootstrap complete | Not possible | ≤ 0.6 ms modeled, no tick wait |
| CPU0 busy per wake | Not possible | ≤ 0.35 ms |
| Worker wakes per second while gated | 1 | 0 |
| Two clients submitting during a wake | Not possible | 0 refusals, 0 tick waits |
| Missed notification, completed job | TIMEOUT | Finished; counter 1; no cancel |
| Deadline to recovered READY, acknowledgement within 1 ms | Not possible | ≤ 1.5 ms, 0 tick waits; before the read-only mappings, plus one bootstrap publication |
| Cancel on an already idle engine | Not modeled | Completes or faults within 20 ms |

## Changes owned elsewhere

- **Power authority:** the domain-4 row accepts a request made after the NPU's
  local clocks are open (its design powers the domain before the consumer
  ungates), and reads DCDC2 only after the settle deadline. Until that branch
  accepts this order, NPU startup keeps its current path.
- **Session runtime and service** (`npu_service.c`, `npu_session.c`,
  `npu_convolution.c`, `a733_npu_memory.c`): the 10 ms fallback and worker
  deadlines; removal of the unbound poll branch and the 1000 ms wake;
  blocking while gated; queuing during a wake; REAP folded into SUBMIT and
  CLOSE; no hash per startup poll or at activation; `live_mappings` at
  admission only; `unchanged`, `stream_intact`, `model_intact` and
  `canonical` per job until the read-only mappings, then removed; client
  clean of input; output-only invalidation; no per-job pool maintenance.
- **Session page users:** `apps/accelerators/prepare.py`, `apps/npu-runtime`,
  `checks/npu-supervised-host.py`, `checks/npu-application.py`,
  `checks/a733-pmic-consumers.c` and the root `CMakeLists.txt`.
- **Native adapter** (`platform/a733/npu_native.c`): verify the register
  window once at initialization.
- **Primary agent:** read-only EL1 and EL0 mappings of admitted regions, and
  any change of the SPI 65 route.
- **Storage owner:** the card-loading conditions.

## Not included, with reasons

- **Domain 4 power-off:** the shared SRAM loses its contents, and the power
  authority keeps the domain on.
- **DCDC2 writes and points above 800 mV:** the rail is shared with the display.
- **Disabling PLL_NPU at idle:** each wake would add a lock wait for a saving
  that local gating mostly provides.
- **Load-driven frequency changes:** the thermal level is the only consumer
  that needs a lower point.
- **CCU reset of an engine that is not idle:** no evidence shows that its bus
  transactions have drained, so this case retains.
- **Larger NPU MMU pages:** the table builder belongs to the memory owner.

## Board evidence that would confirm the design

- PLL_NPU control, `0x1920`, `0x19a0`, CCU `0xb00` and `0xb04` and domain-4
  status after boot, against the record.
- The SID speed-bin code and `ID_AA64ISAR0_EL1` on each core type.
- Startup to READY and conformance outputs at 492 MHz, and at 852 MHz if the
  bin qualifies.
- One interrupt per job across the gate-and-wake run.
- A job with a short deadline that is cancelled, recovered and followed by a
  correct inference. Its input and output ranges, filled with a known pattern
  after `finish`, stay unchanged through close, reset and bootstrap until the
  next SUBMIT writes them.
- Identical outputs with the pool prefilled with zeros and with `0xa5`.
- One hundred gate-and-wake cycles with unchanged outputs, and the measured
  wake time.
