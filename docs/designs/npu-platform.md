# Design: NPU cold boot, recovery and power management

Status: **design**; no code has changed. Every item below ends implemented and
host-modeled, with physical behavior unqualified until the board evidence at
the end exists. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

The design covers NPU startup ([a733_npu_startup.c](../../drivers/npu/a733_npu_startup.c)),
the local clock and reset controller ([a733_npu_platform.c](../../drivers/npu/a733_npu_platform.c)),
the engine controller ([a733_npu_controller.c](../../drivers/npu/a733_npu_controller.c)),
the PLL controller ([a733_npu_pll.c](../../drivers/npu/a733_npu_pll.c)) and the
convolution application in `apps/npu-convolution`. It adds three behaviors:
startup that takes its supply and power domain from the power authority, local
clock gating while the NPU is idle, and bounded recovery from a hung job. A
second operating point and model loading from the microSD card are designed
here but wait for the conditions stated in their sections.

## One cold-boot record replaces the handoff evidence

Every image that runs NPU startup is built from `apps/npu-convolution`: the
multicore build, `npu-convolution-single-core`, and the images built on the
single-core one (`npu-runtime`, `npu-runtime-large`, `npu-finite` and the
accelerator image). The host launcher places a session page at `0x41f00000`
before the image runs. That page becomes the only admission input. It carries
the **cold-boot record**: the image digest, the application boot profile, the
PLL configuration, the whole-platform reservation, and the register state that
boot leaves. That state is: local gates closed; core, AXI and AHB resets
asserted; SRAM reset released; domain 4 off, or on and quiescent; PLL_NPU
disabled or locked.

Startup reads the local CCU fields, PLL_NPU and the domain-4 status once,
before its first write, and compares them with the record. This one pass is
also the local baseline check of the startup sequence below; there is no
second check.

Removed, because the record and its register check do the same job:

- the numeric handoff evidence, the `established` flag word, and the
  `A733_NPU_HANDOFF_REQUIRED` and `SUPERVISED_REQUIRED` values, which the
  header itself states prove no physical fact;
- the `A733_NPU_SUPERVISED` build option: every build uses the launched page;
- the multicore build's external `A733_NPU_HANDOFF_SOURCE` file;
- the session page's evidence, clock, power and supervision digests and its
  supervisor field, which only the supervision contract reads;
- the pre-DRAM PLL enable mode, which needs SRAM-resident code that no image
  has, and the quiescent-ON adoption mode (LIVE_ENABLE), which the domain owner
  now covers by reporting the domain state.

Two PLL entry modes remain: enabling a disabled PLL while DRAM runs, and
retuning a locked PLL.

## Startup takes its rail and domain from the power authority

The power-authority design (`docs/designs/power-authority.md`, on its own
branch) gives startup a DCDC2 rail token from `a733_rail_acquire` and a
domain-4 row on the power-domain owner. Startup becomes:

1. Acquire the DCDC2 token and the clock-registry token (exclusive PLL_NPU and
   NPU-local scopes; stable fabric and reference).
2. Check the cold-boot record against the registers.
3. If the record says PLL_NPU is disabled, enable it at 492 MHz.
4. Release the AHB, AXI and core resets, set the module divider and open the
   bus and module gates, under one register-lock acquisition. Release the lock.
   This keeps the clock-before-power order of the pinned Allwinner driver.
5. Request domain 4 from the owner, wait 2 ms after it reports ON, and check
   the final local state.
6. Open the engine controller, which runs the 208-byte bootstrap stream, and
   activate the service.

The domain request in step 5 is made only after the register lock is
released, because the owner spins up to 50 µs under its own ledger lock.

**Supply settling.** Startup no longer reads the PMIC over S_TWI0 or waits
5,160 µs between two DCDC2 snapshots. The replacing check is the power
authority's first-snapshot settle deadline, no earlier than provider start
plus 5,160 µs. The domain owner reads the DCDC2 enable and selector only after
that deadline (a change requested from the power authority), and startup
continues only when the owner reports ON.

**Removed from startup**, with the check that still covers each:

- `a733_npu_startup_bind_pmic`, `a733_npu_startup_supply_sample`, the PMIC
  consumer field and its health query: covered by the rail token and the
  domain row's supply read.
- `rail_snapshot`, the isolated I2C bus object, `i2c_held`, `rail_step` and
  `rail[]`: covered by the same supply read.
- The PCK600 lease (`pck_token`), `struct a733_npu_startup_resources` and its
  GPU resource-manager wrapper: the domain owner holds the PCK600 bus-clock
  lease.
- Per-call re-checks of copies that never change after pre-scheduler binding
  and that only the bootstrap task writes: `pll_integrity` in every poll, in
  `ready` and in `prepared`; `exact_config` on each acquire; the backend
  function-pointer and command-table comparison in `prepared`. The checks in
  `a733_npu_startup_init` and `a733_npu_startup_bind_pll` still run, and
  `prepared` keeps one live status read of the clock and rail tokens at
  service activation.

**Progress per call.** The bootstrap task performs one step per tick today, so
the platform alone takes 17 polls. A startup call now runs steps until one
waits on hardware:

- the PLL lock: a spin of three observations 3 µs apart plus 20 µs;
- the domain acknowledgement: the owner spins 50 µs. If it returns PENDING,
  startup retries once per tick for up to 10 ms (the slow path);
- the 2 ms domain settle, the only tick wait on the normal path (section 1.3
  permits it, because the hardware wait is at least one tick).

## The service worker owns the NPU after activation

After activation the NPU service worker is the only owner of the engine
controller, the local CCU fields and gating; the bootstrap task suspends. All
state lives in kernel `.bss` (Normal Write-Back, Inner Shareable), only the
owning task writes it, and no DMA master reads it.

The worker runs on CPU0 because EL3 routes SPI 65 (GIC interrupt 97) only to
CPU0; moving the route belongs to the primary agent. A completion notification
therefore wakes a task on the same core with no inter-processor interrupt.
Clients on the A76 cores reach the worker through the service queue and get
replies by task notification. The CPU side of each job (input preparation,
reading the output) stays in the client, on cores 6 and 7 once admitted
(default 7). The worker's CPU0 work per job is the register sequence and the
range cache maintenance. Cross-core reads are the thermal level (one acquire
load per wake) and lease status at admission. Nothing revokes the DCDC2 token
or domain 4, because the power authority has no DCDC2 writer and never turns
domain 4 off.

**Completion.** Every image that runs the service binds SPI 65, so the
interrupt is the only completion path and the unbound poll branch in
`npu_service.c` is removed. Host models model the interrupt. Interrupt
delivery is unqualified on the board, so while a job runs the worker waits on
its notification for at most 10 ms (the fallback period) or until the job
deadline, whichever is sooner. Each fallback wake reads the event register
`0x10` once; a completion found this way finishes the job and counts as a
late interrupt. The fallback is removed when the board shows one interrupt
per job over the gate-and-wake run listed at the end. The convolution
application sets a 1 s job deadline.

**Per-job register work.** The configuration readback (page-directory entry
`0x3b4`, MMU control `0x388`, interrupt enable `0x14`) runs once at the start
trigger and once at completion, before the barrier and `finish()`. It is
removed from every other poll. Both readbacks stay: `register_access` in
`npu_native.c` checks only readiness, alignment, offset and the mapping, so
other EL1 code using the shared device mapping can write these registers
between jobs, and the engine's own behavior under reset is unqualified. The
completion readback is the only proof, before the output returns to the CPU,
that the MMU stayed on with the same page directory.

**Barriers.** Each job keeps two ordering points: before the start write at
`0x3a4`, publishing descriptors in normal memory (`DMB OSHST` once qualified),
and after the full-idle read, before output invalidation (`DMB OSHLD`). The
`DSB SY` that completes cache maintenance stays in `publish` and `finish`. A
`DSB SY` precedes each timed delay in the reset sequence, because the delay
assumes the write reached the device. The per-access `DSB SY` accessors stay
until the qualification that [docs/shared-helpers.md](../shared-helpers.md)
requires.

**Submits during a wake** are queued the way submits during a running job are
queued today (`MEMBER_QUEUED` and `dispatch` in `npu_service.c`). The worker
starts them when the bootstrap completes and replies by notification. In the
single-client mode the deferred request is retried on the bootstrap
completion notification instead of after one tick. Refusal before device
access remains only for invalid commands and the Critical thermal level.

## Idle gating closes the engine and its local clocks

Worker waits: while ungated and idle, a 100 ms timeout; once gated, it blocks
until a request arrives. The 1000 ms wake that keeps the counter extension
current is removed: `counter_clock_sample` accepts samples up to 2^55 counter
ticks apart, about 47 years at 24 MHz.

After 100 ms without a job, the worker closes the controller: interrupt
reporting and the MMU are disabled with readback, and full idle is verified.
It masks the interrupt without stopping its route and gates the NPU: module
gate off, bus gate off, then AHB, AXI and core resets asserted. This is the stop sequence of
the pinned `npu_clk_uninit`, run as one call under one register-lock
acquisition. Its parent-clock checks leave the gate path, because closing a
gate does not depend on the parent rate. Domain 4, the PLL and the rail stay
on, so the shared SRAM keeps its contents.

The next submit reverses the sequence: local release (six writes), then
controller open, which runs the two-pass soft reset (280 µs of spin delays)
and the bootstrap stream. The 100 ms threshold keeps the modeled wake cost,
about 0.5 ms, under 1% of the idle time before it.

**Publication at wake.** Bootstrap publication is today the only point that
cleans the device-read regions (MMU arena, command streams, constant and
coefficient buffers), and `npu_session.c` runs `all_intact`, a SHA-256 of every
model, constant and stream, at that point. A wake skips the clean only when a
**publication epoch** is unchanged. Every CPU write to device-read memory
advances the epoch: a model, coefficient, stream or table write, and every
session admission. A mapping generation alone is insufficient, because
writing new bytes into an already-mapped slot leaves it unchanged.
`all_intact` runs at every wake until the page-table owner maps admitted
regions read-only at EL1 and EL0 (section 1.4); then it runs at admission
only. Images turn idle gating on only after the epoch and the read-only
mappings exist. Without them, each wake adds the arena translation, the cleans
and a full model hash, and the 0.6 ms budget does not hold.

## A hung job is cancelled and the engine restarted

Today the job deadline is terminal: `a733_npu_check_timeout`, which
`irq_progress` calls first, and `a733_npu_advance` both fault with
`A733_NPU_TIMEOUT`. The controller gains two separate limits:

1. **Job deadline.** The worker masks interrupt 97 and runs one controller
   pass that reads `0x10` and full idle without the deadline test. DONE with
   full idle finishes the job normally and counts a missed interrupt. Only
   otherwise does the worker call `a733_npu_cancel`.
2. **Cancel budget**, which stays a hard fault. The worker spins on the
   counter through the 100 µs cancel assertion and then for up to 1 ms for the
   acknowledgement (`0x3a8` bit 4) and full idle (`0x7fffffff`); both waits are
   shorter than a tick, so section 1.3 forbids tick waits there. If neither
   appears, it polls once per tick up to 20 ms from assertion, the vendor's
   200 × 100 µs bound in `vipdrv_task_recover`, and then faults.

After acknowledgement the existing transitions run: CANCELED (`finish`
invalidates the output range and hands the buffers back), `a733_npu_reap` to
DRAINED, `a733_npu_close`, then `a733_npu_open` with the two-pass soft reset
and the bootstrap stream. The vendor recovery resubmits the init command
without a reset. This design keeps the reset, because open is the only
qualified path from an unknown engine state and the reset costs 280 µs. The
job reports TIMEOUT. Two consecutive recoveries are allowed and the third
timeout retains; a completed job resets that count. A per-session cap of
eight recoveries also retains, so a model that hangs on alternate jobs stays
bounded (section 1.8).

Failure classes (section 1.8):

- **Refused before device access** (invalid command, Critical thermal level):
  an error; the session stays usable.
- **Known terminal state** (deadline with cancel acknowledged and full idle):
  the recovery above; nothing is retained.
- **Unknown state** (no acknowledgement within 20 ms, an event bit other than
  completion, an MMIO failure, a recovery-cap overrun, a failed CCU
  readback): the controller faults and retains the session memory and MMU
  tables, the clock and rail tokens and the domain row, and the register lock
  only if a CCU read-modify-write stopped midway.

## Operating points: refuse under Critical until the speed bin is known

The worker refuses submits while the thermal level is Critical, and runs at
492 MHz otherwise. The 852 MHz point is implemented only after the attached
board's SID speed bin is read and qualifies. In the 24 MHz reference columns
(`opp-microvolt-vf*`) of the pinned `sun60iw2p1-cpu-vf.dtsi`, 852 MHz needs
800 mV for bins vf0000, vf0200, vf0201, vf0400, vf0500 and vf0501. Bin vf0000
is excluded, because an unprogrammed fuse also decodes to it. Points above
800 mV are refused, because DCDC2 also supplies eDP and HDMI.

When implemented, the 852 MHz point is a 1,704 MHz oscillator (multiplier 71
on 24 MHz), P0 = 2 and module divider 1. The multiple-of-492 MHz test in
`parents_stable` is replaced by an exact table of admitted (N, P0, divider)
triples. The enabled-retune sequence may run repeatedly after startup while
the whole-platform exclusion lease is held, and a failed retune readback
retains the PLL and clock tokens.

## Model loading from the card is deferred

No image or catalogue model needs the card today. The largest catalogue model,
the published ShuffleNet V2 graph, is 964,224 bytes. Card loading is
implemented when a model or model set exceeds the image's linked budget, and
must then meet these conditions:

- **Copies.** Today `sunxi_mmc.c` `fifo_step` reads each FIFO word over MMIO
  into a stack array and copies it byte by byte into the port's bounce buffer;
  `fatfs_port.c` then copies the bounce buffer into the destination. Each
  command moves at most 8 blocks (4 KiB) on a polled core. The storage owner
  replaces the byte loop with word stores and, preferably, reads multi-block
  runs with the SMHC internal DMA straight into the model slot, which is
  device-pool memory and needs no bounce.
- **Ownership.** The slot is cache-line aligned and shares no line with data
  another owner holds; no published NPU mapping reads it during the write; the
  storage task hands it over with a release store that the session acquires,
  and keeps no reference afterwards. Loading advances the publication epoch.
- **Hashing.** The catalogue SHA-256 is computed per 4 KiB block during the
  copy into the slot, one pass in total, using the Armv8 SHA-2 instructions
  when `ID_AA64ISAR0_EL1` reports them.
- **Budget.** Load throughput in MiB/s and SD commands per MiB, measured with
  `checks/fatfs-port.c` on a RAM device.

## Budgets

*Measured* baselines come from the host models (`checks/npu-platform.c`,
`checks/npu-controller.c`) at commit `041c09a`, with a counting wrapper that
runs one poll per modeled millisecond. *Counted* baselines come from reading
the code. The implementation adds these counters to the existing models.

| Metric | Baseline | Target | Measured in |
| --- | --- | --- | --- |
| Platform polls, acquisition to READY | 17, measured | ≤ 3; one tick wait (2 ms settle) | `npu-platform.c` |
| Platform register reads and writes at startup | 51 and 12, measured | ≤ 36 and 6 | `npu-platform.c` |
| Counter-clock reads at startup | 190, measured | ≤ 12 | `npu-platform.c` |
| Register-lock acquisitions at startup | 15, measured | 2 | `npu-platform.c` |
| Startup PMIC traffic | 12 register reads, 6 bus-state reads, 5,160 µs wait, counted | None; one token status read | `npu-startup.c` |
| Startup source size | 436 lines in `a733_npu_startup.c`, 131 in its header, counted; `sizeof(struct a733_npu_startup)` recorded before the change | ≤ 250 and ≤ 70 lines; smaller struct | `wc -l`; `npu-startup.c` prints the size |
| Bootstrap ticks to activation, after PMIC READY | About 29, counted | ≤ 4 without PENDING; ≤ 14 with | `npu-startup.c` tick counter |
| Local gate | 6 polls, 102 reads, 5 writes, 238 clock reads, 6 lock acquisitions, measured | 1 call, ≤ 12 reads, 5 writes, ≤ 6 clock reads, 1 lock | `npu-platform.c` |
| Wake, gated to bootstrap complete | Not possible | ≤ 0.6 ms modeled, no tick wait, no clean when the epoch is unchanged | `npu-platform.c` with `npu-controller.c` |
| CPU0 busy per wake | Not possible | ≤ 0.35 ms: 280 µs of reset spins plus the open sequence | Same |
| Worker wakes per second while gated | 1 (1000 ms timeout), counted | 0 | Service case in `npu-startup.c` |
| Engine register accesses per job | 11 reads and 2 writes, plus 5 reads per extra poll, measured | 11 reads and 2 writes; between start and interrupt only one `0x10` read per 10 ms fallback wake | `npu-controller.c` |
| CPU0 work per job outside cache maintenance | 13 register callbacks, 13 `AT` translations, 17 `DSB SY`, counted | 13 callbacks, 0 `AT` after the register-window change; 14 `DSB SY` now, 2 `DMB` after qualification | `npu-controller.c` access counts |
| Client wake-ups per job, convolution application | One per tick of execution plus 2 replies, counted | 3: submit, completion, reap | Job case in `npu-startup.c` |
| Two clients submitting during a wake | Not possible | 0 refusals, 0 tick waits | Service case in `npu-startup.c` |
| Missed notification with a completed job | Faults as TIMEOUT | Finished normally; 1 missed interrupt counted; no cancel | `npu-controller.c` and the service case |
| Deadline to recovered READY, acknowledgement within 1 ms | Not possible; timeout retains everything | ≤ 1.5 ms modeled, 0 tick waits, nothing retained | `npu-controller.c` |
| Cancel on an already idle engine | Not modeled | Completes or faults within 20 ms; a good result is never reported as TIMEOUT | `npu-controller.c` |

## Changes owned elsewhere

- **Power authority:** the domain-4 row accepts a request after the NPU's
  local clocks are on, admits a quiescent domain 4 found on at boot, and reads
  DCDC2 only after the first-snapshot settle deadline.
- **Session runtime and service** (`npu_service.c`, `npu_session.c`,
  `npu_convolution.c`, `a733_npu_memory.c`): the 10 ms fallback wait and
  deadline handling above; removal of the unbound poll branch and the 1000 ms
  counter wake; blocking while gated; queuing submits during a wake;
  the publication epoch and `all_intact` at wake.
- **Session page users:** `apps/accelerators/prepare.py`, `apps/npu-runtime`,
  `checks/npu-supervised-host.py`, `checks/npu-application.py`,
  `checks/a733-pmic-consumers.c` and the root `CMakeLists.txt` move to the
  reduced page and startup interface.
- **Native adapter** (`platform/a733/npu_native.c`): verify the register window
  once at initialization in place of an `AT` translation per access.
- **Primary agent:** read-only EL1 and EL0 mappings of admitted regions, and
  any change of the SPI 65 route.
- **Storage owner:** the conditions listed for card loading.

## Not included, with reasons

- **Domain 4 power-off:** the shared SRAM alias (`0x74000`–`0xf3fff`) loses its
  contents, and the power authority keeps the domain on.
- **DCDC2 writes and points above 800 mV:** the rail is shared with the display.
- **Disabling PLL_NPU at idle:** each wake would add a lock wait and a consumer
  recheck for a saving that local gating mostly provides.
- **Load-driven frequency changes:** the thermal level is the only consumer
  that needs a lower point.
- **CCU reset of an engine that is not idle:** no evidence shows that its bus
  transactions have drained, so this case retains.

## Board evidence that would confirm the design

- Register dumps after boot of PLL_NPU, CCU `0xb00` and `0xb04` and domain-4
  status, matching the cold-boot record.
- The SID speed-bin code of the attached board.
- Startup to READY and conformance outputs at 492 MHz, and at 852 MHz if the
  bin qualifies.
- One interrupt per job across the gate-and-wake run, which removes the
  fallback wait.
- A job given a short deadline that is cancelled, recovered and followed by a
  correct inference.
- One hundred gate-and-wake cycles with unchanged outputs, and the measured
  wake time.
