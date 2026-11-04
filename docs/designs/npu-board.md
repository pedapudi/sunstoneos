# Design: NPU startup on the power-domain owner, and a board image with the NPU runtime

Status: **implemented**, **host-tested** (register-model, QEMU and Unicorn runs
of the linked images). The image has not run on a board; the board session that
would qualify it is open work. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md). Figures are
in `docs/evidence/npu-board.jsonl`; no figure comes from the board.

The design has three parts:

- NPU startup runs on the power-domain owner
  ([docs/power-domains.md](../power-domains.md)) as the single startup
  sequencer with the reduced session record that
  [npu-platform.md](npu-platform.md) designed.
- `sunstone-a733-npu-board` is a supervised image derived from the board image
  that links the NPU runtime of [npu-breadth.md](npu-breadth.md) with one slot
  and one client.
- The host checks that need separately configured accelerator images build and
  pass on the single-core, all-core, accelerator and board images.

## Startup sequencer

`a733_npu_startup.c` is the only NPU startup state machine. It receives a
borrowed provider view: counter, clock registry, register lock, the image's one
GPU/PCK resource manager, the rail ledger with its power-authority word, and the
power-domain owner. The image constructs the providers. The bootstrap task calls
one step function, which runs steps until one waits and returns PENDING while it
waits; the bootstrap calls it again after one tick. Each wait has a counter
deadline.

1. Acquire one shared DCDC2 rail token, one clock-registry token (exclusive
   PLL_NPU and NPU-local; shared reference, oscillator, fabric and PERI0) and
   one stable OSC and PCK lease from the image's manager.
2. **Supply.** One acquire load of the authority word must show
   `A733_POWER_SUPPLY_DONE` and `A733_POWER_DCDC2_VERIFIED` with no provider,
   clock, ledger or domain-4 fault bit. While the done bit is clear the step
   returns PENDING. The owner verifies DCDC2 at its first step, with no
   `A733_POWER_SUPPLY_SETTLE_US` wait, because no NPU image makes a supply
   transition. The wait's deadline is the later of the token step and the PMIC
   provider's startup deadline, plus `A733_POWER_SUPPLY_BUDGET_US` (100 ms) and
   one tick, so a slow provider cannot cause a refusal. A refusal or an expired
   deadline releases the tokens before any NPU write.
3. Under one register-lock acquisition, `a733_power_domain_check(domains, 4,
   &state)` and the register pass of npu-platform.md. The owner's check reads
   the PCK600 words once and returns the coherent domain state; startup admits
   state 0 (OFF) or 8 (ON). Both states require the same closed local gates and
   asserted engine resets. The pass reads 20 words, or 25 when a fabric clock
   selects PERI0, each compared under a mask with a constant, and requires
   PLL_NPU disabled or adoptable at the selected profile
   ([npu-852mhz-plan.md](npu-852mhz-plan.md)). An inherited ON domain is
   adopted with a rail pin and no PCK600 write; [startup ownership and
   lifecycle limits](../npu-startup.md) apply.
4. Under the same acquisition, the PLL_NPU method-2 enable table. The lock is
   then released and LOCK is read without it: up to 100 observations 3 µs apart
   until three in a row read locked, then 20 µs, spacing read from `CNTPCT_EL0`
   (section 1.7). The worst case is about 320 µs, below one tick, so the spin
   does not yield.
5. Under a second acquisition, the local open table with a readback of each
   write: PLL output gate, AHB, AXI and core resets released (`0xb04` bits 18,
   17, 16), module selector and divider (`0xb00`), bus gate (`0xb04` bit 0),
   module gate (`0xb00` bit 31).
6. `a733_power_domain_request(row, 1)`. The step returns PENDING while
   `a733_power_domain_state(row)` is pending. The deadline is the request plus
   `A733_POWER_PCK_BOUND_US` (10 ms) plus two ticks of service latency.
7. **Settle.** When the row reads ON, sample the counter and set the settle end
   to that sample plus 2,000 µs. The step returns PENDING until a later counter
   read reaches it; a tick count alone is not used, because two ticks can end
   just over 1 ms after the sample. Then read back the final local state under
   a third acquisition.
8. Open the engine controller and activate the service.

Steps 5 and 6 keep the vendor order, clocks before domain power
(`vip_drv_device_platform.c` lines 419–428), which the owner's interface also
prescribes. Step 2 removes the supply wait from the time that the clocks run
into an unpowered domain; what remains is bounded by the step 6 deadline.

The domain-4 row lives in startup storage. The bootstrap task writes its
request field and the service task writes its answer field, each with release
order; each reads the other's field with acquire order. The owner writes the
domain-4 delay row and policy word; the NPU platform does not write PCK600 or
read the delay row. Its native allowlist keeps the policy and status reads
(`PCK+0x0`, `PCK+0x8`) for the register pass, the final readback and the stop
check. The sequencer holds the PLL_NPU enable table, the local open and stop
tables, their LOCK predicates and one apply-with-readback helper as statics.
The platform module keeps the fabric-record validation and its tables, which
the sequencer and the session validators share. `a733_power_domain_check`
returns the domain state it read, so startup reads PCK600 once.

**One resource manager.** The startup constructs no GPU/PCK manager of its own.
`apps/npu-convolution/native.c` constructs its image's manager as one provider
among the others, and the board image passes `providers.shared.gpu`. Every image
has one manager, because a board-derived image would otherwise hold two managers
of one PCK, and its rail ledger is bound to the board's. The owner reads the PPU
clock bit but holds no lease on it, so the NPU keeps its PCK lease, as G2D and
the framebuffer do. Moving that lease into the owner is open work for the
power-domain owner.

**Reduced session record.** `struct npu_session` is 456 bytes. It keeps one
fabric copy, the PMIC configuration and the pin handoff, because every NPU image
runs the power service whose PMIC provider reads them. `NPU_SESSION_VERSION` is
3 for the single-core profiles and 2 for the multicore profile; the combined
accelerator session is version 2.

## Board image

`cmake/a733-npu-board.cmake` calls `sunstone_board_application(npu-board …)`
the way `cmake/a733-g2d.cmake` does: `apps/npu-board/boot.patch` and
`extended-boot.patch` over the board's supervised boot, `extended-link.patch`
over `link-devices.ld`, and `SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1` and
`SUNSTONE_A733_IRQ_TABLE=1`. `apps/npu-board/` supplies:

- `board_application_start`, which takes the runtime's parent, prepares the
  runtime, constructs the startup and its row, creates the bootstrap and the EL0
  client task, binds the service and the interrupt, pins the board's USB0 task
  to core 0 and creates the thermal monitor on core 0;
- `board_application_task_privileged`, which admits the NPU worker, bootstrap
  and thermal tasks at EL1t, because `npu_runtime_startup_poll` refuses a caller
  below EL1 and the bootstrap writes CCU registers;
- `admission.c`, which validates the 1,000-byte record (`npu-board-session.h`,
  version 2). It extends `struct a733_board_session`, as `struct a733_g2d_session`
  does, with NPU owner, command admission, supervisor, PCK client, catalogue
  digest, four NPU evidence digests and the thermal payload. The board workload
  is 2: USB0 and the providers, no USB2 host and no radio. Admission requires
  the owner to differ from the board's owners and the PCK client to differ from
  the board session's. Older 808-byte sessions are refused;
- `prepare.py` and `launch.c`, which follow their G2D counterparts, and the
  supervised capture `tools/check-a733-npu-board-supervised.py`. The launcher
  authenticates the physical-memory and remote-firmware exclusion that
  [docs/device-memory-capacity.md](../device-memory-capacity.md) requires for the
  version-2 extended boot record.

The preparer requires these conditions, each bound into its evidence digest:

| Digest | Conditions |
| --- | --- |
| Admission | `npu-startup-sram-confinement-drain-ownership`, `remote-writers-excluded`, `exclusive-cpus-memory-dma`, `pmic-bus-rail-pin-ownership`, `npu-irq97-disabled-inactive-clear`, the DDR reservation record for `0x48000000`–`0x79000000` |
| Clock | `supervised-cross-pll-disturbance-risk`, `exclusive-pll-npu-consumers-and-writers`, `independent-dram-phy-sources`, `no-autonomous-parent-selector-writers` |
| Power | `dcdc2-stable-800mv`, `authenticated-pmic-pin-inheritance` |
| Supervision | `manual-whole-board-power-removal`, `retain-all-resources-until-power-removal`, `stop-after-first-failure` |

The thermal payload carries a distinct owner, an authority identifier, APB1 and
GPADC settings, five hardware threshold rows and four evidence digests. Host
preparation reuses `apps/thermal/prepare.py` for the THS/IRQ71 ownership, cold
controller state, SID calibration access, analog and digital supplies, mappings,
stable clocks and supervised retention requirements. The thermal facts sit
under `thermal` in the admission record and must carry physical authority; a
model record cannot authorize a board launch.

The catalogue is the five models of `apps/npu-board/npu-models.json`, from which
the build generates the image's admitted table. `apps/npu-board/catalogue.py`
checks each model file against `tools/npu-fixtures.json` and links it
read-only, with `npu_board_catalogue_digest`, the SHA-256 over the model digests
in list order, which the record must carry. One slot and one client serve every
board step.

**Window hand-offs** (`apps/npu-board/client.h`). After activation the bootstrap
notifies the client task, which sends OPEN itself and publishes `ready` with a
release store only after OPEN succeeds. USB0 is the only writer of the request
word. Before `ready`, USB0 answers every request without payload with result 4,
not ready, and reports the client fields as zero; the host repeats STATUS until
it reads OK. An UPLOAD before `ready` has no window to receive it and stops
USB0. USB0 reads the client's fields for a response only after an acquire load
of `ready` and of the answer word. After a LOAD the client publishes the tensor
addresses and lends the read window to USB0 with a release store of the window
word; USB0 writes the input chunks into it. Before SUBMIT, USB0 hands the window
back with a release store and a task notification, then posts the request. After
the reply the client lends the write window, and USB0 transmits the output from
it in place, hands it back the same way after the last output byte and posts
RETURN; the client then lends the read window again. The client sends SUBMIT,
LOAD or CLOSE only while it holds both windows and refuses a request that
arrives while one is lent. USB0 posts one request at a time; neither task waits
on a tick. LOAD carries a catalogue index and is answered when the slot is READY
or the load is refused. The USB0 framing is described at the top of
`apps/npu-board/usb.c`.

**USB configuration.** The image has its own TinyUSB configuration,
`apps/npu-board/tusb_config.h`, first in its include path. It sets
`CFG_TUD_EDPT_DEDICATED_HWFIFO=1` and a 4 KiB vendor transmit FIFO. The MUSB
driver moves bytes by programmed I/O (PIO) between each controller FIFO and the
TinyUSB class FIFO, and `tud_vendor_read` and `tud_vendor_write` copy bytes
between the class FIFO and the lent tensor window. Each tensor byte therefore
needs one
memory copy and one PIO move in either direction, with no endpoint staging
buffer. A READ response no larger than the FIFO enters the empty FIFO in one
exchange, so TinyUSB sends a zero-length packet only after its last byte;
`usb.c` refuses a READ whose response would exceed `CFG_TUD_VENDOR_TX_BUFSIZE`.
The catalogue's largest output, linear-i16-per-row's 2,816 bytes, gives a
2,880-byte response. The shared MUSB patch assembles a controller word with byte
loads or stores at unaligned ring addresses and keeps the word loop for aligned
ones; controller accesses are 32-bit words followed by byte tails.
No host check executes the linked USB task, TinyUSB and MUSB bulk transfer and
interrupt code at full and high speed, with odd addresses, ring wrap, the
largest catalogue response and terminal zero-length packets. An early RETURN keeps the window lent until the final
completion.

## Interrupts and polling

- **Startup.** The supply, row and settle waits are the PENDING returns above,
  one call per tick with a counter deadline. The PCK600 has no known interrupt
  wiring, so the owner polls it.
- **Jobs.** `apps/npu-board/irq.c` holds the `a733_irq_routes` entry for SPI 65
  (INTID 97): core 1, priority `0xd8`, which meets `A733_IRQ_TABLE_MIN_PRIORITY`
  (`0xb0`). `boot.patch` calls `a733_irq_table_route(admission.startup_deadline,
  a733_smp_counter)` where the OS and I2S boot patches call it. The ISR masks
  the level-high SPI with `gicv3_spi_mask` before
  `npu_service_irq_notify_from_isr`. The bind, before the scheduler, checks once
  that `GICD_IROUTER` for INTID 97 (`GICD+0x6308`) holds core 1's affinity and
  that the source is disabled, inactive, not pending, level-sensitive and at its
  priority; the mask, rearm and stop callbacks then check only the calling core.
  The worker is pinned to core 1, and `npu_runtime_bind_irq` passes the route
  mask `1 << 1`.
- **Unqualified delivery.** Until a board run shows one interrupt per job, the
  runtime keeps its idle-register read per worker step and a one-tick timeout
  while a job runs (section 1.3, the heavier form).
- **Clients and USB0.** The client bounds each runtime request and reply wait to
  two seconds, waits indefinitely for task notifications, and never calls
  `vTaskDelay`. After CLOSE it notifies the bootstrap. The bootstrap also wakes
  every 25 ms to supervise thermal freshness and the stop deadline. The service
  worker keeps its 1,000 ms idle timeout. USB0 uses the shared loop of
  `apps/board/usb0_service.h`, at most 200 µs or 64 passes per tick, because
  USB0 interrupt delivery is unqualified.

## Thermal admission and orderly stop

`apps/npu-board/thermal.c` runs the native THS controller and fixed board policy
with the board's counter, clock registry and register mutex. Its task owns IRQ71
on core 0, at priority `0xd8`. The ISR masks the source and notifies the task;
the task reads the five channels, clears observed events and rearms the source.
It also reads every 25 ms. Startup validates the live clock token and device
mappings and applies the factory SID calibration. An inherited thermal service or
invalid calibration refuses.

The NPU bootstrap and every SUBMIT require all five channels to have fresh
readings and the policy to be NORMAL. The policy throttles CPU/GPU at 90 °C and
DDR at 85 °C; all channels are critical at 110 °C. Its NPU channel has no
passive trip. A reading older than one second blocks new work, and five seconds
without readings is critical. THROTTLE blocks new SUBMIT requests; fresh
readings below the policy's hysteresis limits restore admission. No frequency or
voltage changes occur.

One atomic word carries a freshness deadline and the admission, accepted-work
and terminal stop bits. A compare-and-exchange claims startup or a job before it
enters the runtime, then a second counter read checks that the claim did not
expire while the task was preempted. Clearing accepted work preserves a
concurrent stop. Initial NORMAL without valid samples cannot admit startup.
Sensor faults close admission without revoking the board-live condition needed
to drain the NPU.

CRITICAL latches a five-second stop deadline, capped by the board session
deadline, and never renews it. An accepted job keeps its one-second controller
deadline and two-second request and reply bounds. A failure retains its
resources. USB completes an accepted upload or an active IN response, including
its terminal zero-length packet, before returning the window. Later READ
requests are refused; later uploads are discarded into fixed scratch storage.
The client sends CLOSE only with both windows returned. Automatic CLOSE
publishes only the atomic closed flag, leaving USB's plain result snapshot
stable.

Successful service CLOSE proves controller quiescence and stops the NPU
interrupt before the bootstrap applies the local gate and reset table. A
critical observation before startup prevents any NPU startup call. If startup
was already accepted, it finishes under its existing bounds, then OPEN and CLOSE
establish the runtime's closed-slot state; no model job is admitted during this
handshake. Domain 4, PLL_NPU, the rail pin and all leases remain held; the
design has no fault reset or restart.

The bootstrap independently checks the publication every 25 ms on core 1. If
the core-0 monitor freezes, it refuses stale admission and latches stop at five
seconds since the oldest reading; the five-second drain then gives a maximum of
about ten seconds since that reading, plus scheduling delay, before retained
failure. A stalled host or retained register mutex cannot extend the deadline or
authorize reclaiming an active window. The monitor is one static task with a
2,048-word stack; successful startup and SUBMIT claims each add two counter
queries and one atomic claim.

Focused checks execute the linked sensor, controller and policy, the real
runtime client and service under QEMU, and the linked TinyUSB and MUSB transfer
path. They cover calibration, fresh, hot and stale readings, critical and
frozen-monitor deadlines, accepted-job completion, automatic CLOSE, IRQ masking
and rearm, and window retention through IN and zero-length packets at both USB
speeds. Physical thermal response, numerical NPU execution and startup are
unqualified.

## Memory placement and cache policy

`link-devices.ld` gives the image only the extended pools. The board's own
allocations and the runtime's parent come from the 768 MiB write-back pool at
`0x48000000`–`0x78000000`, which EL1 maps with 2 MiB blocks. For one
ShuffleNetV2-sized slot, the placement rules of `npu_runtime_requirements` give
8,388,608 bytes: the read-only group holds 2,953,216 bytes and ends at 4 MiB.
The input group holds 151,552 bytes and the pool and output group 1,097,728
bytes, and each ends on the next 2 MiB boundary.
no host check (evidence: npu-board-image-checks) runs the image's own construction and reads the
pool's slots afterwards; no runtime, service or client function calls the
allocator, so LOAD and CLOSE leave the free extents unchanged (796,917,760 bytes
free after admission).

The cache policy per job, by address through `aarch64_device_cache`:
preparation cleans the inputs, cleans and invalidates the output lines, and
zeroes the 1,093,632-byte pool with `DC ZVA`, then cleans and invalidates it.
Completion invalidates the outputs. The interim checks run (T and A before the
trigger, H after the job, about 1.51 MB read per job; H holds back the SUBMIT
reply). LOAD cleans the read-only bytes it wrote. Startup, service and runtime
state lives in kernel `.bss` (Normal write-back, inner shareable). Each field
has one writing task, except the row's request and answer fields.

## Synchronization across cores

The power service runs on core 0, where the board places it, and the board's
USB0 task is pinned there too. The worker, the bootstrap task and the client run
on core 1, the core that receives INTID 97, so a completion needs no
inter-processor interrupt and the worker does not share a core with USB0.
Requests and replies use FreeRTOS queues. Startup takes the shared register lock
three times in total.

## Physical entry

The generated NPU boot starts cores 1 through 7 before the scheduler. Pinning
the NPU tasks to core 1 does not reduce that requirement. The image needs a
qualified eight-core entry, a coherent extended DDR reservation and matching
recovery evidence on the target board; a single-core USB result cannot supply
them. The preparer's authenticated conditions (table above) cover exclusive CPU,
memory and DMA ownership with remote writers excluded, NPU SRAM confinement and
drain ownership, a disabled, inactive, clear INTID 97, exclusive PLL_NPU
consumers with independent DRAM clock sources, stable DCDC2 at 800 mV, inherited
PMIC pin ownership and a USB reference matching the cached-SMP cold-entry
profile. Runtime readbacks can reject changed settings but do not establish
these external conditions.

The smallest inference attempt uses catalogue model 0, one upload, one SUBMIT, a
byte comparison with its pinned 72-byte reference, RETURN and CLOSE, with the
image, session and evidence frozen before launch. The host consumes and durably
records the terminal zero-length packet when a response occupies a whole number
of USB packets, including every 64-byte header at full speed. It reports success
only after all output files match, CLOSE reports controller state CLOSED and the
original USB instance remains within the capture deadline. Twenty modeled host
cases exercise those boundaries and the actual supervisor
(`checks/npu-board-capture.py`).

The image keeps resources until the prescribed whole-board power removal and
stops communication after the first failure. CLOSE stops the NPU locally and
USB0 remains active until the 20-second session deadline; the image has no
normal ROM-return path. The single-core `apps/npu-finite` consumer implements
verified quiescence and normal ROM return while retaining the NPU power domain;
that return does not cover the SMP board providers. Current board recovery state
is in the [handoff](../handoff.md).

## Register sequences and their sources

| Sequence | Source |
| --- | --- |
| PLL_NPU method-2 enable, LOCK predicate, consumer masks | A733 User Manual V1.00 §4.1.4.1–4.1.4.2, §4.1.6.56–60; the lock helper of the board clock source at Linux revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` ([docs/npu-pll.md](../npu-pll.md)) |
| Local open order, clocks before domain power | Pinned VIPLite `vip_drv_device_platform.c` lines 419–428 |
| Local stop | `npu_clk_uninit` in the pinned VIPLite platform |
| Domain-4 delay row, command and status | Owner code, from `pck600_domains.c` lines 126–142 and 547–569 at the same revision. **Unproven:** words `0xc00`–`0xc10` are absent from the PCK-600 manual |
| 2 ms settle after domain ON | **Unproven:** no pinned source; kept from the controller and to be measured on the board |

## Host checks

no host check (evidence: npu-board-image-checks) builds the images of a configured
`apps/accelerators` build and, when given, the root `sunstone-a733-npu-board`,
with `nice -n 10` and six jobs. It converts the reviewed session fixtures,
608-byte version-2 NPU pages and the 9,080-byte combined session, to the reduced
layout the image's DWARF and layout words describe, and invokes each check:

| Check | Images |
| --- | --- |
| `npu-stop-native.py` | Single-core, all-core, accelerator, board; fourteen cases including refused domain-4 policy and delay-row writes and the PLL gate rules |
| no host check (evidence: docs/evidence/npu-board.jsonl, docs/evidence/npu-platform.jsonl) --retention-only` | Single-core; three cases |
| no host check (evidence: docs/evidence/npu-board.jsonl npu-board-image-checks) | Accelerator; the application's one GPU/PCK manager, the startup's borrowed providers and its single domain-4 row |
| no host check (evidence: npu-board-image-checks) | Board; provider construction, the pool's slots and the linked FIFO sizes |

`platform_init` sets DAIF.I again after the local pool preparation, because a
kernel critical section there clears it in the single-core port, and the
admission in `a733_smp_diagnostic_start` runs before any task is created and
must stay there. `checks/storage-read-native.py` refuses images in which two
compilation units define different structures with one tag (`a733_pmic.c` and
`a733_power_domains.c` both define `struct profile`), so each check passes
`layout_names` to `Image` with the structures it reads.

## Failure classes

| Failure | Class (section 1.8) | Retained |
| --- | --- | --- |
| Token or lease refused, supply refused or its deadline expired, register-pass mismatch | Refused before any NPU write | Nothing; tokens released |
| LOCK not seen in 100 observations | Unknown | Clock, rail and PCK tokens; output gate closed; PLL left enabled |
| Row REFUSED or FAILED, row deadline expired, readback mismatch, any deadline after the first write | Unknown; supervised whole-board power removal is the only recovery | Tokens, the row with its request (it cannot be withdrawn), the reservation; the register lock only if a table stopped midway |
| Runtime or controller fault | As in [npu-breadth.md](npu-breadth.md) | As there |

## Qualified figures

Host-model figures against the base; each row is in `npu-board.jsonl`.

| Quantity | Base | Image | Record |
| --- | --- | --- | --- |
| Boot to READY, domain 4 OFF at entry | 12,001 µs in the first routing model | 4,628 µs (4,615 µs with domain 4 ON), 4 calls | `npu-board-startup-latency` |
| Startup register reads / writes / lock acquisitions | 90 / 18 / 7 | 61 / 13 / 4, no PCK600 write | `npu-board-budgets` |
| PMIC traffic and bytes hashed in startup | 12 reads, 5,160 µs wait; 20.7 MB | none; 0 | same |
| `sizeof(struct npu_session)` | 608 bytes | 456 bytes | same |
| Runtime parent | no board image | 8,388,608 bytes at `0x48000000` | same |
| Per ShuffleNetV2 inference | 1 request and reply, 1 tick wait per tick | 3 jobs: 3 requests, 3 replies, 0 tick waits | same |
| G, P, H, W (ShuffleNetV2 slot) | 1.44, 5.41, 2.52, 8.22 ms | 1.15, 4.33, 2.01, 6.58 ms | same |
| Copies per tensor byte | 2 | 1 plus PIO | `npu-board-direct-fifo` |

The register-read budget (61), the startup source-line budget (408 + 79 lines
against 260 + 60) and the rest of the budget table are in
`npu-board-budgets`; the source-line budget is the one missed.

## Limitations and open work

- **Board session** (`npu-board-first-board-session`), one session per
  whole-board power removal: the register-pass words and domain-4 status after
  boot and the measured PLL_NPU lock time; domain-4 time to ON through the
  owner, a status trace, the reads of `0xc00`–`0xc10` before and after the
  delay-row write (docs/power-domains.md) and whether the final local state
  holds without the 2 ms settle; boot to READY in the board image,
  ShuffleNetV2 output equal to the SDK reference and a LOAD that switches the
  slot between two catalogue models; one INTID 97 interrupt per job on core 1;
  USB0 throughput and power-service step durations while jobs run; and the
  extended-memory reservation evidence that docs/device-memory-capacity.md
  requires.
- The interim runtime rows stay at their npu-breadth values until the fixed
  mappings and the pool comparison remove the interim checks and the pool clear.
- Idle gating, the 10 ms fallback, cancel and recovery deadlines and REAP
  folding ([npu-platform.md](npu-platform.md)) change the service worker and are
  not part of startup or linking.
- The single-core and accelerator images use the convolution path, not the
  runtime; their USB protocol and three of the four host checks are built on it.
- A second slot and client have no board step that needs them. The SHA-2
  digests, read-only EL1 mappings, NPU MMU pages larger than 4 KiB and A76
  placement each need board evidence or another owner. Shader graphs and a
  section-order decoder are open in npu-breadth.md.
