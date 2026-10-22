# Design: NPU cold boot, recovery and power management

Status: **partly implemented**. The controller passes, local stop,
per-call startup progress, per-call ownership checks and single-copy
admission are implemented and host-modeled; the
[measurements](npu-platform-measurements.md) list the parts that remain and the
owners they need. Physical behavior is unqualified until the board evidence at
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

## Admission copies the session page once and validates the copy

The host launcher places a session page at `0x41f00000`, and the EL3 contract
carries its SHA-256. The EL3 step (`a733_smp_contract_valid`) keeps only the
contract-side checks: magic, version, build ID, nonzero epoch, generation and
lease, the window, the deadlines, and the watchdog and redirect flags. It
records the contract's page digest and supervisor identity in the retained
receipt and reads nothing from the live page. The copy step
(`npu_single_core_seal` at EL3, `npu_session_copy` at EL1 in the multicore
build) copies the page into retained private memory, hashes the copy once and
compares the hash with the receipt; that comparison authenticates the copy.
One combined compare then requires the copy's image digest to equal the build
ID, its epoch, generation and lease to equal the contract's, and its
supervisor to equal the receipt's. The digest already binds those values, so
the compare only catches a preparer that wrote a page for another session.
Startup reads only that copy. The live
page is hashed zero times instead of once, and a writer that changes it after
the copy cannot affect startup.

**The record holds identities and evidence digests.** After this design the
page carries:

- magic, version and size; the image digest; epoch, generation and lease; the
  whole-platform reservation;
- the four reviewed-evidence digests: admission (platform and DDR
  conditions), clock, power and supervision; and the supervisor identity;
- the owner identities that still have a consumer: `npu_owner` (clock-registry
  token owner), `usb_owner` and `usb_evidence` (the USB platform configuration
  in `native.c`), `bridge_owner` (the rail binding) and `command_admission`
  (`npu_convolution_handoff` and startup);
- one fabric copy (`fabric`, `rate_hz`), which only the USB platform
  configuration reads. NPU startup does not read it.

Removed from the page: the 13 PLL_NPU consumer snapshots, the PLL control,
`0x1920` and `0x19a0` words, the PLL config's owner and handoff, the second
fabric copy and the check that the two copies agree, the PMIC configuration,
the pin handoff, and the profile fields `pmic_owner`, `pmic_evidence`,
`pin_evidence`, `npu_evidence`, `npu_established` and `usb_established`.
Every removed field either describes a fixed board fact or has no consumer
once the PMIC consumer and handoff flags go.

**Supervision stays required.** The contract requires the watchdog off, so the
only recovery contract is supervised whole-board power removal. The supervision
digest is therefore required on every session, and the host preparer
(`apps/npu-convolution/prepare-supervised.py`) keeps its three supervision
conditions: `manual-whole-board-power-removal`,
`retain-all-resources-until-power-removal` and `stop-after-first-failure`.
Every session also requires the clock condition
`supervised-cross-pll-disturbance-risk`, because startup always enables
PLL_NPU. Its reason is the reference manual's warning that enabling a PLL can
disturb other PLLs, which no register read can rule out for a live DRAM PLL.

**Handoff flags map to evidence conditions.** The numeric flag word
(`A733_NPU_HANDOFF_*`) proves nothing by itself, as its header states. Each
flag's fact is carried by a reviewed-evidence condition that the preparer
checks before it writes the digest:

| Flag | Evidence condition that carries it |
| --- | --- |
| `QUIET`, `SRAM_FREE` | Admission: `npu-startup-sram-confinement-drain-ownership` |
| `MEMORY` | Admission: DDR reservation record and `exclusive-cpus-memory-dma` |
| `REMOTE` | Admission: `remote-writers-excluded` |
| `I2C` | Admission: `pmic-bus-rail-pin-ownership`; power: `authenticated-pmic-pin-inheritance` |
| `RECOVERY`, `SUPERVISED` | Supervision: the three conditions above |
| `IRQ_MASKED` | Admission: new condition `npu-irq97-disabled-inactive-clear`; the EL3 route installer also reads it back where it runs |

No flag is covered by the register pass below.

**Fixed board rules replace snapshots.** The admitted PLL_NPU state and the
local CCU baseline become compile-time constants taken from the pinned
Allwinner driver and confirmed by the first board-evidence item. The 13 exact
consumer snapshots become masked field checks, the pattern that
[a733_i2s_platform.c](../../drivers/audio/a733_i2s_platform.c) uses. The
masks keep every refusal that `a733_npu_pll_scopes` and `baseline()` apply
today, including the live-DRAM enable conditions in
[docs/npu-pll.md](../npu-pll.md):

- AHB, NSI and MBUS (`0x500`, `0x580`, `0x588`) select SYS24 or an admitted
  PERI0 output, and NSI and MBUS have no DFS update pending (bits 27 and 28
  clear);
- DRAM0 (`0xc00`) has its gate on, DDRPLL selected (select field 0), bit 16
  clear and no update pending (bit 27 clear), and the DRAM bus (`0xc0c`) has
  bit 16 set: the external memory clock comes from the separate PHY PLL;
- the video encode and decode clocks (`0xa80`, `0xa88`) have their gates
  closed and a select field of at most 6, and their bus word (`0xa8c`) has the
  gates closed and the resets asserted (`0x50005` clear);
- the autonomous clock modes (`0x1b10`, `0x1b14`, `0x1b1c` bit 0) are off;
- `0xb00` and `0xb04` hold the local baseline: module and bus gates closed,
  core, AXI and AHB resets asserted, SRAM reset released;
- PLL_NPU is disabled: enable, LDO, lock detector, output and LOCK clear,
  and the rest of the control word equal to its constant;
- the four PLL_NPU tuning words hold modulation, fractional interpolation,
  smooth update and dithering off and both biases at reset: `0x2a4` bit 0
  and `0x2a8` bit 31 clear, `0x2ac` under `0xff100000` equal to `0x30000000`,
  and `0x2b0` under `0x001f0000` equal to `0x000b0000`. The unlock-interrupt
  enable, which [docs/npu-pll.md](../npu-pll.md) requires off and no current
  check reads, joins the same compare.

Only the divider fields of these words are not compared. They belong to the
DRAM, video and fabric drivers and do not affect whether enabling PLL_NPU is
safe, so a divider change by those drivers does not stop NPU startup.

**One register pass.** Before its first write, startup reads the 24 MHz
reference (two words), the PERI0 parent and its gate words (five), the 13
consumer words, PLL_NPU control and its four tuning words, `0x1920`, `0x19a0`
and the two domain-4 status words. That is 29 reads, or 30 if the
unlock-interrupt enable sits in another word, under one register-lock
acquisition, each compared under its mask with the constants. The pass
replaces three overlapping passes: the PLL controller's `baseline()`, the application
adapter's `parents()` and the platform's `parents_stable()`.

**Entry state.** Startup admits only domain 4 OFF with PLL_NPU disabled. Any
other state is refused before the first write. Domain 4 found ON is refused: the shared SRAM aliases
(`0x74000`–`0xf3fff`) are powered by domain 4, and with the domain OFF no
bootloader, FEL continuation, CPU or DMA user can hold data in them. Two
consequences follow:

- After a successful return, domain 4 stays ON and PLL_NPU stays locked.
  Today's admission refuses that state too, so a second session needs
  whole-board power removal. The return receipt predates the next FEL load,
  and nothing records which SRAM that loader uses, so the receipt cannot admit
  the post-return state.
- If the first board-evidence item shows that cold boot leaves domain 4 ON,
  the current quiescent-ON profile returns unchanged, with its clock
  conditions `live-disabled-pll-quiescent-on-snapshot` and
  `native-dram-continuations-and-shared-sram-abandonment`. If boot leaves
  PLL_NPU locked, an adopt entry (at 492 MHz) or the existing retune sequence
  (at another rate) is designed then, with that evidence.

## Startup is the only sequencer

The power-authority design (`docs/designs/power-authority.md` on its own
branch) gives startup a DCDC2 rail token and a domain-4 row. Startup has one
operations table (read, write, now, register-lock enter and leave) that host
checks replace, and one retain path. It runs these steps:

1. Acquire the DCDC2 token and one clock-registry token: exclusive PLL_NPU and
   NPU-local scopes, shared reference, oscillator, fabric and PERI0.
2. Run the register pass.
3. Apply the method-2 enable step table under the same lock
   acquisition (the sequence of the mode now named `LIVE_ENABLE`), then
   release the lock and spin on LOCK: three observations 3 µs apart, then
   20 µs apart, at most 100 observations. The spin holds no lock, because the
   exclusive PLL_NPU token makes startup the only writer of the control word.
   A spin that ends without LOCK retains: the output gate stays closed, the
   PLL is not disabled or retried, and the clock and rail tokens and the
   reservation stay held.
4. Take the lock once and apply the local open table: PLL output open, AHB,
   AXI and core resets released, module divider set, bus and module gates
   open, each with readback. This keeps the clock-before-power order of the
   pinned Allwinner driver.
5. Request domain 4 from its owner, wait 2 ms after it reports ON, and check
   the final local state under a third lock acquisition.
6. Open the engine controller and activate the service.

The PLL module keeps only a `const` enable step table and a LOCK predicate.
The platform module keeps `const` open, gate and stop step tables. Both use
one shared apply-with-readback helper. Neither keeps an operations table,
init, poll, ready, acquire, retain, resources or observed state.

**Supply.** Startup no longer reads the PMIC over S_TWI0 or waits 5,160 µs
between two DCDC2 snapshots. The domain owner reads DCDC2 enable and selector
after the power authority's settle deadline (provider start plus 5,160 µs)
and reports ON. It spins up to 50 µs for the PCK600 acknowledgement with no
lock held, then returns PENDING; startup retries once per tick for up to
10 ms. The PMIC-path removal lands with or after the power authority's domain
row and its supply read; until then startup keeps its current DCDC2 check.

**Removed, with the check that still covers each:**

| Removed | Covered by |
| --- | --- |
| PLL modes `COLD_ENABLE` (no image has the SRAM-resident pre-DRAM code) and `ENABLED_RETUNE` (until the 852 MHz point) | Constants admit only a disabled PLL_NPU |
| Exact equality of the 13 consumer words with the record's snapshots | The masked compare, which keeps every refusal listed above |
| `a733_npu_platform_adopt_on` and the `adopt_on` field | Register pass requires domain 4 OFF |
| Platform `acquire` operation, `struct a733_npu_platform_resources`, `resources` and `observed` copies, ACQUIRE stage | Startup step 1: one registry call grants every scope or the step retains |
| Platform BASELINE stage, `parents_stable()` | Register pass |
| `a733_npu_platform_init`, `_poll`, `_ready`, the platform operations table, `timely`, `fault`, `rd`, `check`, `update` | Startup's operations table, one deadline check per step, the shared apply-with-readback helper |
| `a733_npu_pll_init`, `_poll`, `_ready`, `_bind_registry`, the PLL operations table and stage machine (ACQUIRE, BASELINE, PROGRAM, wait, SETTLE, COMPLETE), `baseline`, `clock_equal`, `preserved`, `equal` | Step 3 with readback, the LOCK predicate, and the register pass |
| PLL `acquire` and `retain` callbacks, `struct a733_npu_pll_resources`, token transfer through `a733_clock_publish_stable`, successor binding | Startup's single token and single retain path |
| Startup fields `pll_source`, `pll_binding_self`, `pll_config`, `pll_operations`, `pll_cookie`, `pll_transferred`, `pll_whole_platform`; `pll_integrity`, `pll_config_equal`, `pll_ops_equal`; `a733_npu_startup_bind_pll` | PLL rules are constants; the record copy has no writer |
| The no-PLL acquisition path (generic CLOCK stage) | Same token; every image has a PLL step |
| `apps/npu-convolution/clock.c` and `clock.h`: `context()`, `parents()`, `same_config()` and the adapter | Register pass; one ownership check per startup call |
| `task_owner()` in `read_reg`, `write_reg`, `enter`; `exact_config` per acquire | One ownership and EL check per startup call |
| `bind_pmic`, `supply_sample`, PMIC consumer and health query; `rail_snapshot`, the I2C bus object, `i2c_held`, `rail_step`, `rail[]` | Rail token and the domain row's supply read |
| PCK600 lease `pck_token`, `struct a733_npu_startup_resources` and its GPU resource-manager wrapper | Domain owner holds the PCK600 bus-clock lease |
| `A733_NPU_HANDOFF_*`, `a733_npu_handoff_established`, `handoff.established`, `A733_NPU_APPLICATION_USB_REQUIRED`, `A733_NPU_APPLICATION_USB_SUPERVISED` | Evidence conditions in the table above |
| The `A733_NPU_SUPERVISED` option and the `A733_NPU_HANDOFF_SOURCE` file path | The page path, which every image already uses, builds unconditionally |
| Live token status read in `prepared` | Nothing revokes the tokens before activation; one acquire load of the registry's `failed` flag in `prepared` catches a registry fault |
| `backend`, `backend_cookie`, `image`, `commands` and `command_count` in `struct a733_npu_handoff`; `backend_equal` and the metadata loop in `a733_npu_startup_prepared`; `metadata()` and its calls at bind, in each startup and stop poll and in `prepared` | One copy in the service, fixed before the scheduler starts in private memory and checked once by `npu_service_init_deferred` with the `valid_backend` conditions |
| Hash of every model, constant and stream on each startup poll and at activation (`unchanged(c,0)` in `npu_convolution_startup_poll` and `prepared`; `all_intact` in `npu_session.c` `platform_step`) | The first bootstrap publication hashes the same bytes before the device reads them |

**One copy of the activation metadata.** The operations, backend cookie, MMU
image and command table exist today in three copies: the convolution object,
the startup handoff and the service. `npu_convolution_handoff` fills the
handoff's copy from the convolution object, so no boot authority stands behind
it, and comparing the copies proves nothing; today they are compared at bind,
twice per startup poll, twice per stop poll and twice at activation. The
service keeps the only copy and checks it once at initialization. `prepared`
then checks only startup's own state: not failed, the register lock not
retained, and the counter clock's and the clock registry's `failed` flags
clear. The page-path files (`prepare-supervised.py`,
`boot-supervised.patch`, `checks/npu-supervised.py`,
`checks/npu-supervised-host.py`) implement the only admission path left and
produce the supervision evidence above, so they stay, reduced to the new
record.

**Progress per call.** The bootstrap task runs steps until one waits on
hardware. The only tick wait on the normal path is the 2 ms domain settle,
which section 1.3 permits.

## The service worker owns the engine after activation

After activation the NPU service worker is the only owner of the engine
controller, the local CCU fields and idle gating; the bootstrap task blocks.
Controller, startup and service state live in kernel `.bss` (Normal
Write-Back, Inner Shareable), written by their owning task only and read by
no DMA master.

**Placement and interrupts.** The single-core interrupt profiles (runtime,
large graph, accelerator and the finite interrupt variant) route SPI 65 (GIC
interrupt 97) only to CPU0 through the EL3 installer in `apps/npu-interrupt`.
Their worker runs on CPU0 at priority 4 of 8, and a completion notification
wakes it with no inter-processor interrupt. The multicore image defines no
`a733_irq_routes` table, so SPI 65 stays as common boot leaves every SPI:
disabled, inactive and not pending (the precondition that
`platform/a733-smp-cached/irq_table.c` checks). Its worker runs on core 1
with the service unbound and keeps polling in this design. While the engine
is active (job, bootstrap or cancel), the worker runs one `a733_npu_poll` pass
per 1 ms tick (`wait = 1` in `npu_service.c`): completion is seen up to one
tick late, and the worker wakes 1,000 times per second while a job runs.

The step that removes that poll is a separate change with its own design
gate: an `a733_irq_routes` table in `apps/npu-convolution/irq.c` that routes
SPI 65 to core 1, and the route call in the application's boot patches, as
`apps/os/irq.c` and `apps/os/boot.patch` do. The multicore convolution image
never returns (its client suspends itself at the end of `main.c`), so the step
needs no GIC restore path. It changes no reserved code: the installer and
dispatcher in `platform/a733-smp-cached/irq_table.c` already serve the OS and
I2S images. Its target is the bound
figures in the budget table: no tick polls per job and one poll pass per 10 ms
fallback. The unbound poll branch in `npu_service.c` stays while the
multicore image and the polled single-core profile use it.

**Cross-core synchronization.** Requests and replies travel through FreeRTOS
queues, whose kernel critical sections order the data. The worker reads the
thermal level with one acquire load per wake. The bootstrap task signals
readiness and local stop with a release store followed by a task notification
to the client.

**Client waits.** The convolution client blocks on the bootstrap task's
notification for readiness and local stop, holds an `NPU_SERVICE_WAIT` after
OPEN until READY and after SUBMIT until DONE, and sends no STATUS request.
REAP folds into the owner's next SUBMIT or CLOSE, which reap a DONE job
first. The client owns the output buffer from the completion reply until it
sends either request, so it reads the output in place and the output copy in
`npu_convolution_output` is removed. A job costs two round trips, SUBMIT and
the completion reply, as section 3 requires. The finite and runtime bootstrap
loop that waits for `closed` once per tick is replaced by the same
notification.

**Completion.** Where the interrupt is bound, it is the completion path.
Delivery is unqualified on the board, so the worker waits on its notification
for at most 10 ms or until the job deadline, whichever is sooner. A fallback
wake and the deadline sample each make one call to `a733_npu_poll`, the same
pass an interrupt runs: read `0x10`; on DONE, require full idle
(`0x7fffffff`), read `0x10` again with no other bit set, read back the
configuration, then the barrier and `finish()`. DONE without full idle stays
latched in `irq_seen` until the next wake. A completion found without its
interrupt increments one missed-interrupt counter. The fallback is removed
when the board shows one interrupt per job over the gate-and-wake run. The
convolution application sets a 1 s job deadline.

**Per-job register work.** The configuration readback (`0x3b4`, `0x388`,
`0x14`) runs at the start trigger and at completion only. Both stay, because
other EL1 code using the shared device mapping can write these registers
between jobs, and the completion readback is the only proof, before output
returns to the CPU, that the MMU stayed on with the same page directory. The
per-access `DSB SY` accessors stay until the qualification that
[docs/shared-helpers.md](../shared-helpers.md) requires; then each job keeps
`DMB OSHST` before the start write at `0x3a4` and `DMB OSHLD` after the
full-idle read.

**Per-job CPU work** happens in `npu_convolution_send`, `publish` and `finish`
(owned by `npu_convolution.c`, `npu_session.c` and `a733_npu_memory.c`):

| Work per job, accelerator image | Today, counted | After this design |
| --- | --- | --- |
| SHA-256 of NN and coefficient buffers (`unchanged(c,1)`, `model_intact(s,m,0)`) | 917,504 bytes, plus the stream payload and an END scan of the rest of the 524,288-byte command allocation, in the worker's `publish` | Same bytes and place, with SHA2 instructions where every core reports them; 0 after the read-only mappings |
| `AT` translations (`live_mappings` in publish and finish, `register_access`) | 1,063 per walk × 2 + 13 = 2,139 | Same until the EL1 table pages are read-only; then 0 |
| MMU table scan (`canonical`) | 4,352 words and 3,072 padding bytes | Same until the arena is read-only; then 0 |
| Input copy (`memcpy` in `npu_convolution_send`) | 151,552 bytes, one copy per byte | At most one copy per byte |
| Output copy (`npu_convolution_output`) | Up to 4,096 bytes, one copy per byte | 0 |
| Pool clear (`memset` in `npu_convolution_send`) | 1,093,632 bytes | Same, in the client, until the pool evidence; then 0 |
| Cache maintenance | Clean 151,552 (input); clean-invalidate and later invalidate 1,097,728 each (pool and output): 2,347,008 bytes, in the worker | Client: clean of the input, plus the pool while its clear exists. Worker: clean-invalidate 4,096 output bytes before the job and invalidate them after |

**Digest placement and cost.** In every build the client and the worker
share a core: core 1 in the multicore build, CPU0 in the single-core images.
The constant-buffer digest therefore stays in the worker's `publish`, which
every job passes through; moving it to the client would save no time on any
core and would let a SUBMIT sent directly through `npu_service_send` skip it.
The build selects the SHA2-instruction
implementation for `unchanged`, `model_intact`, `all_intact` and the
bootstrap-publication digest when `ID_AA64ISAR0_EL1` reports SHA2 on every
core type. The estimates use 15–25 cycles per byte for portable SHA-256
(section 1.4) and 2–4 cycles per byte with SHA2 instructions:

| Digest | Portable, 1.8 GHz / 494 MHz | SHA2 instructions, 1.8 GHz / 494 MHz |
| --- | --- | --- |
| Per job, 917,504 bytes | 7.6–12.7 ms / 28–46 ms | 1.0–2.0 ms / 3.7–7.4 ms |
| Bootstrap publication, 1,881,936 bytes | 15.7–26.1 ms / 57–95 ms | 2.1–4.2 ms / 7.6–15.2 ms |

CPU0 pays this digest only in the single-core images.

**Maintenance placement.** The client cleans the bytes it wrote (`DC CVAC`), then issues `DSB SY` before the
SUBMIT send. The device that reads the data sits outside the inner-shareable
domain, so the barrier must complete the clean to the point of coherency
before the queue send releases the buffer; the queue's ordering does not wait
for that completion. `DSB OSH`
replaces it if the shared-helpers qualification shows it sufficient. The
worker clean-invalidates the 4,096 output bytes before each job. No mapping
stops a CPU write to the output buffer, and a dirty line left by such a write
could be evicted over device output. After completion it invalidates the same
bytes, after the full-idle read and its barrier. The pool is device scratch
that the CPU never reads. While the per-SUBMIT clear exists, the client
cleans the cleared pool and nothing invalidates it. The clear is removed when
the board shows identical outputs from a pool prefilled with zeros and with
`0xa5`; then the pool is cleaned and invalidated once at admission.

## Idle gating closes the engine and its local clocks

Idle gating is off until two conditions hold. One `const` in the worker
switches it, with no build option or per-image variant; the gate and wake code
is reachable only from the 100 ms idle branch that the constant guards. First, the primary agent maps
the admitted model, coefficient, stream and MMU-table regions, and the EL1
translation-table pages, read-only at EL1 and EL0, with no writable EL1 or
EL0 alias of any of them in the linear map, the uncached pool or any other
window. Second, the board shows a measured saving (below). Under those mappings the
per-job and wake digests, translations and table scans check nothing that the
mappings do not enforce, and they are removed. Each writer then cleans its own
range where it publishes it: admission cleans the constant regions and tables
once, and the client cleans its input per job.

Without those mappings every wake would run the full bootstrap publication:
1,881,936 bytes hashed plus the stream payload, END scans of two 524,288-byte
allocations, 2,126 translations, the table scan, 3,235,840 bytes cleaned or
clean-invalidated and 1,097,728 bytes invalidated by `finish`.

**Gate.** While ungated and idle, the worker waits with a 100 ms timeout; once
gated, it blocks until a request arrives. The 1000 ms counter-extension wake
is removed, because `counter_clock_sample` accepts samples up to 2^55 ticks
apart (about 47 years at 24 MHz). After 100 ms without a job, the worker
closes the controller (interrupt reporting and MMU disabled with readback,
full idle verified) and masks interrupt 97 without stopping its route. It then
gates the NPU under one lock acquisition: module gate, bus gate, then AHB, AXI
and core resets, the stop sequence of the pinned `npu_clk_uninit`. Domain 4,
the PLL and the rail stay on. The 100 ms threshold keeps the 0.6 ms wake cost
below 1% of the idle period it follows; the board current measurement sets
the final value. The final CLOSE in the finite and returning images still
stops the route and waits for `npu_service_irq_stopped` before it reports
quiescence.

**Wake.** The next submit reverses the sequence: six local writes, then
controller open, which runs the two-pass soft reset (280 µs of spin delays)
and the 208-byte bootstrap stream. Submits that arrive during a wake are
queued as submits during a running job are queued today (`MEMBER_QUEUED`,
`dispatch`) and start when the bootstrap completes. Refusal before device
access remains only for invalid commands and the Critical thermal level. The
worker unmasks interrupt 97 after controller open returns, as recovery does;
a pending state left from before the gate is consumed only through an
`a733_npu_poll` pass. The
soft reset stays on the wake path until the board shows that a wake with only
the CCU reset release and the bootstrap stream gives identical outputs; recovery
keeps it in any case.

**Worker core occupancy.** The reset spins and the cancel spins run at worker
priority with interrupts enabled and without the platform register mutex; the
engine registers are not CCU registers. The worker's core (CPU0 in the
single-core images, core 1 in the multicore build) is unavailable to tasks at
priority 4 or lower for at most 0.35 ms per wake and 1.1 ms per cancel. That
core also runs the per-job digest until
the read-only mappings exist: 7.6–12.7 ms per job at 1.8 GHz portable, or
1.0–2.0 ms with SHA2 instructions (28–46 ms and 3.7–7.4 ms at 494 MHz).

## A hung job or bootstrap is cancelled and the engine restarted

The worker owns three deadlines: the job deadline (1 s for the convolution
application), the bootstrap deadline (10 ms from the bootstrap trigger, for
the 208-byte stream at cold open, on wake and after recovery) and the cancel
deadline. `a733_npu_check_timeout` and the elapsed test in
`a733_npu_advance` are deleted once the worker tests these. A bootstrap that
misses its deadline follows the same path as a job:

1. **Deadline.** The worker masks interrupt 97 and runs one `a733_npu_poll`
   pass. DONE with full idle finishes normally and increments the
   missed-interrupt counter. Otherwise the worker calls `a733_npu_cancel`,
   which accepts BOOTING and RUNNING. A DONE that arrives after this pass is
   reported as TIMEOUT.
2. **Cancel budget, a hard fault.** The worker spins through the 100 µs
   cancel assertion and then for up to 1 ms until both the acknowledgement
   (`0x3a8` bit 4) and full idle are seen. Failing that, it polls once per
   tick up to 20 ms from assertion, the vendor's 200 × 100 µs bound in
   `vipdrv_task_recover`, and then faults.

After both are seen: CANCELED (`finish` invalidates the output and hands the
buffers back), `a733_npu_reap`, `a733_npu_close`, then `a733_npu_open` with the
soft reset and bootstrap stream. The worker unmasks interrupt 97 after
`a733_npu_open` returns. The source is level-high, so a completion that
asserts while masked stays pending and is taken on unmask. The vendor recovery
resubmits the init command without a reset; this design keeps the reset,
because open is the only qualified path from an unknown engine state. One
per-session cap of four recoveries, counting bootstrap and job deadlines
together, bounds hangs; the fifth retains. At a 1 s deadline a model that
always hangs retains after about 5 s.

| Failure | Class (section 1.8) | Retained |
| --- | --- | --- |
| Invalid command, Critical thermal level | Refused before device access | Nothing |
| Job or bootstrap deadline, cancel acknowledged with full idle | Known terminal state | Nothing; recovery runs |
| LOCK not seen within 100 observations | Unknown | Clock and rail tokens, reservation; output gate closed; PLL left enabled |
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
bytes. The change that adds the first model larger than the image's linked
budget chooses one load path and meets these conditions:

- The model slot is cache-line aligned, shares no line with another owner's
  data, is unmapped from the NPU during the write, and passes to the session
  by a release store that the session acquires.
- Cache maintenance covers the slot bytes the writer produced, by address.
- The SHA-256 reads the slot bytes after the last write completes and the
  slot has no writer.

## Budgets

*Measured* baselines come from the host models at commit `041c09a` with a
counting wrapper (one poll per modeled millisecond); *counted* ones from the
code. The implementation adds these counters to `checks/npu-platform.c`,
`checks/npu-pll.c`, `checks/npu-controller.c` and `checks/npu-startup.c`.
Digest instruction counts per byte, for the portable and SHA2 forms, are
measured by running the compiled digest under Unicorn, as
`checks/lradc-native.py` runs native code. Rows marked *bound* apply to the
single-core profiles that route interrupt 97; rows marked *multicore* apply to
the multicore image until the SPI 65 binding step, and the host model of
`npu_service.c` measures their baselines.

| Metric | Baseline | Target |
| --- | --- | --- |
| Session-page hashes at admission | 2 (live page, copy), counted | 1 (copy) |
| Startup calls, acquisition to READY | Platform 17 measured; PLL 13 counted | ≤ 3; one tick wait |
| Startup register reads and writes | Platform 51 and 12 measured; PLL and adapter about 58 and 7 counted | ≤ 60 reads excluding LOCK samples; ≤ 12 writes |
| Counter-clock reads at startup | Platform 190 measured; PLL about 110 counted | ≤ 16 plus one per LOCK sample |
| Register-lock acquisitions at startup | Platform 15 measured; PLL 13 counted | ≤ 3 |
| Ownership checks per register access | `task_owner` (startup); EL read, scheduler query, task compare and health query (adapter) | 0; one per startup call |
| Bytes hashed before activation | About 30 calls × 1,881,936 bytes, about 56 MB, counted | 0; the first bootstrap publication hashes once |
| Startup PMIC traffic | 12 register reads, 6 bus-state reads, 5,160 µs wait | None |
| Source size, lines (code + header) | Startup 436 + 131; platform 225 + 96; PLL 204 + 109; adapter 130 + 19 | Startup ≤ 260 + 60; platform ≤ 90 + 30; PLL ≤ 60 + 25; adapter deleted |
| Bootstrap ticks to activation | About 42 counted | ≤ 4 without PENDING |
| Local gate | 6 polls, 102 reads, 5 writes, 238 clock reads, 6 lock acquisitions, measured | 1 call, ≤ 12 reads, 5 writes, ≤ 6 clock reads, 1 lock |
| Per-job digest, bytes and place | 917,504 plus stream, worker `publish` | Same until read-only mappings; then 0 |
| Per-job digest time, 1.8 GHz | 7.6–12.7 ms estimated, portable | 1.0–2.0 ms with SHA2; 0 after read-only mappings |
| CPU0 digest time per job, single-core images | 7.6–12.7 ms at 1.8 GHz, 28–46 ms at 494 MHz | 1.0–2.0 ms and 3.7–7.4 ms with SHA2; 0 after read-only mappings |
| Per-job `AT` | 2,139 | Same until read-only EL1 tables; then 0 |
| Per-job table scan | 4,352 words | Same until read-only arena; then 0 |
| Copies per input byte | 1 | ≤ 1 |
| Copies per output byte | 1 | 0 |
| Bytes written by the pool clear per job | 1,093,632 | Same until the pool evidence; then 0 |
| Per-job cache maintenance in the worker | 2,347,008 bytes | 8,192 (output clean-invalidate and invalidate) |
| Per-job maintenance in the client | 0 | 151,552 input bytes; plus 1,093,632 pool bytes while the clear exists |
| Engine register accesses per job, bound | 11 reads and 2 writes, plus 5 reads per poll, measured | 11 and 2; one `a733_npu_poll` pass per 10 ms fallback |
| Worker poll passes while the engine is active, multicore | One per 1 ms tick, counted | Same; after the binding step, one per 10 ms fallback |
| Completion latency after DONE, multicore | Up to 1 tick | Same; after the binding step, interrupt delivery |
| Worker wakes per second while a job runs, multicore | 1,000 | Same; after the binding step, one per interrupt plus at most 100 fallback wakes |
| Tick waits per bootstrap and per cancel, multicore | One poll per tick until done | Same; after the binding step, 0 |
| `DSB SY` per job | 17 counted | ≤ 17 across both cores; after qualification, 2 `DMB` plus the barriers that complete cache maintenance |
| Client round trips and tick polls per job | 3 round trips plus one STATUS poll per tick | 2 round trips; 0 tick polls |
| Client tick polls during startup and local stop | One per tick | 0 |
| Wake publication work | 1,881,936 bytes hashed, 2,126 `AT`, 3,235,840 bytes cleaned, 1,097,728 invalidated | 0 of each |
| Wake, gated to bootstrap complete, bound | Not possible | ≤ 0.6 ms modeled, no tick wait |
| Worker core busy per wake | Not possible | ≤ 0.35 ms |
| Worker wakes per second while gated | 1 | 0 |
| Two clients submitting during a wake | Not possible | 0 refusals, 0 tick waits |
| Missed notification, completed job, bound | TIMEOUT | Finished; counter 1; no cancel |
| Deadline to recovered READY, acknowledgement within 1 ms, bound | Not possible | ≤ 1.5 ms, 0 tick waits; before the read-only mappings, plus one bootstrap publication |
| Hung bootstrap after a wake | Bounded only by the deleted elapsed test | Cancelled at 10 ms; recovered or retained within 30 ms; counts toward the cap |
| Cancel on an already idle engine | Not modeled | Completes or faults within 20 ms |

## Changes owned elsewhere

- **Power authority:** two dependencies. The domain row with its DCDC2
  supply read must land first, or with this change, because the PMIC-path
  removal relies on it. The domain request is made after the NPU's local
  clocks are open (its design powers the domain before the consumer ungates);
  if the power authority does not accept that order, startup keeps only its
  current domain-power step, and every other change here still lands.
- **Session runtime and service** (`npu_service.c`, `npu_session.c`,
  `npu_convolution.c`, `a733_npu_memory.c`): the 10 ms fallback and the job,
  bootstrap and cancel deadlines in the worker; removal of the 1000 ms wake;
  blocking while gated; queuing during a wake; REAP folded into SUBMIT and
  CLOSE; removal of the output copy in `npu_convolution_output`; no hash per
  startup poll or at activation; `metadata()` and its calls removed, with the
  `valid_backend` conditions checked once in `npu_service_init_deferred`;
  `live_mappings`, `unchanged`, `stream_intact`, `model_intact` and
  `canonical` per job until the read-only mappings, then removed; client clean
  of input; output clean-invalidate before and invalidate after each job; no
  per-job pool maintenance after the pool evidence.
- **Digest owner** (`drivers/storage/storage_digest.c`): a build-time
  SHA2-instruction implementation selected from the recorded
  `ID_AA64ISAR0_EL1` values.
- **Session page users:** `apps/accelerators/prepare.py`, `admission.c` and
  `session.h`; `apps/npu-runtime`; `apps/npu-finite/host.py` and
  `proposal.py`; `checks/npu-application.py`, `checks/npu-large-host.py`,
  `checks/npu-runtime-host.py`, `checks/npu-single-core.py`,
  `checks/npu-single-core-host.py`, `checks/a733-pmic-consumers.c`;
  `tools/check-a733-npu-supervised.py`; the root `CMakeLists.txt`.
- **Native adapter** (`platform/a733/npu_native.c`): its register-window
  translation stays per access until the EL1 table pages are read-only.
- **Memory owner:** the pinned VIPLite table format, as built by
  `a733_npu_mmu_build`, supports 4 KiB, 64 KiB, 1 MiB and 16 MiB pages per
  16 MiB root entry, and `a733_npu_memory.c` requests 4 KiB for every buffer.
  Use 1 MiB pages for the pool and coefficient buffers where physical and
  device alignment allow, and 64 KiB otherwise. That shrinks the table the
  per-job scan reads and widens NPU TLB reach.
- **Primary agent:** read-only EL1 and EL0 mappings of the admitted regions
  and of the EL1 translation-table pages, with no writable EL1 or EL0 alias;
  any reserved vector, monitor or platform change that binding SPI 65 in the
  multicore image turns out to need. `platform/a733-smp-supervised/launch.c` needs no
  change: it requires a nonzero contract `reserved[0]`, and the supervisor
  identity stays.
- **USB platform owner:** the fabric copy in the record could become a
  constant of the USB platform configuration.

## Not included, with reasons

- **Domain 4 power-off:** the shared SRAM loses its contents, and the power
  authority keeps the domain on.
- **DCDC2 writes and points above 800 mV:** the rail also supplies the
  display.
- **Disabling PLL_NPU at idle:** each wake would add a lock wait and, under
  supervision, another cross-PLL enable for a saving that local gating mostly
  provides.
- **Load-driven frequency changes:** the thermal level is the only consumer
  that needs a lower point.
- **CCU reset of an engine that is not idle:** no evidence shows that its bus
  transactions have drained, so this case retains.
- **Admitting the post-return state:** the return receipt cannot show which
  SRAM the next FEL load uses.

## Board evidence that would confirm the design

A second session needs whole-board power removal, so each item below runs
within one session, in this order:

1. PLL_NPU control, `0x1920`, `0x19a0`, the 13 consumer words and domain-4
   status after boot, against the constants. Startup's register pass reports
   them before its first write.
2. The SID speed-bin code and `ID_AA64ISAR0_EL1` on each core type.
3. Startup to READY and conformance outputs at 492 MHz, and at 852 MHz if the
   bin qualifies.
4. One interrupt per job across the gate-and-wake run.
5. A job with a short deadline that is cancelled, recovered and followed by a
   correct inference. Its input and output ranges are filled with a known
   pattern after `finish` and cleaned before the next SUBMIT; they stay
   unchanged through close, reset and bootstrap until that SUBMIT.
6. Identical outputs with the pool prefilled with zeros and with `0xa5`.
7. Identical outputs from a wake with only the CCU reset release and the
   bootstrap stream, against a wake with the soft reset.
8. Board input current, or NPU-domain current if it can be measured, with the
   NPU ungated and idle and with it gated. Idle gating stays off if gating
   saves nothing measurable.
9. One hundred gate-and-wake cycles with unchanged outputs, and the measured
   wake time.
