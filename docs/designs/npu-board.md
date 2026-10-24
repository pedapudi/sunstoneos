# Design: NPU startup on the power-domain owner, and a board image with the NPU runtime

Status: **implemented and host-modeled**. No board run has used it; the
board evidence that would qualify it is listed at the end, and
[the measurements](npu-board-measurements.md) give every budget row against
the base. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md). Startup
baselines come from the two register-model harnesses
`checks/npu-platform-budget.c` and `checks/npu-platform-pll-budget.c` at the
base and from [npu-platform-measurements.md](npu-platform-measurements.md).
Runtime baselines come from [npu-breadth.md](npu-breadth.md). No figure comes
from the board.

This package does three things:

- It moves NPU startup onto the power-domain owner
  ([docs/power-domains.md](../power-domains.md)). It also implements the single
  startup sequencer and the reduced session record that
  [npu-platform.md](npu-platform.md) designed and deferred.
- It builds `sunstone-a733-npu-board`, a supervised image derived from the
  board image that links the NPU runtime of npu-breadth.md with one slot and
  one client.
- It makes the four host checks that need separately configured accelerator
  images build and pass.

## Ownership and data flow

**One sequencer.** `a733_npu_startup.c` becomes the only NPU startup state
machine. It receives a borrowed provider view: counter, clock registry,
register lock, the image's one GPU/PCK resource manager, the rail ledger with
its power-authority word, and the power-domain owner. Construction of
providers stays with the image. The bootstrap task calls one step function.
The function runs steps until one waits and returns PENDING while it waits;
the bootstrap calls it again after one tick. Each wait has a counter deadline:

1. Acquire one shared DCDC2 rail token, one clock-registry token (exclusive
   PLL_NPU and NPU-local; shared reference, oscillator, fabric and PERI0) and
   one stable OSC and PCK lease from the image's manager.
2. **Supply.** One acquire load of the authority word must show
   `A733_POWER_SUPPLY_DONE` and `A733_POWER_DCDC2_VERIFIED` with no provider,
   clock, ledger or domain-4 fault bit. While `A733_POWER_SUPPLY_DONE` is
   clear the step returns PENDING. The owner sets the done bit within its own
   bound: `A733_POWER_SUPPLY_SETTLE_US` (5,160 µs) after its first step, or
   the PMIC provider's startup deadline before the provider is READY, plus
   `A733_POWER_SUPPLY_BUDGET_US` (100 ms). The wait's deadline is therefore
   the later of the startup's token step and the provider's startup deadline,
   plus those two figures and one tick, so a slow provider cannot cause a
   refusal. A refusal or an expired deadline releases the tokens before any
   NPU write.
3. Under one register-lock acquisition, `a733_power_domain_check(domains, 4)`
   and the register pass of npu-platform.md, 22 reads, or 27 when a fabric
   clock selects PERI0, each compared under a mask with a constant. Startup
   admits only domain 4 OFF and PLL_NPU disabled.
4. Under the same acquisition, the PLL_NPU method-2 enable table. Then release
   the lock and read LOCK without it: up to 100 observations 3 µs apart until
   three in a row read locked, then 20 µs. The spacing reads `CNTPCT_EL0`
   directly (section 1.7). The worst case is about 320 µs, below one tick, so
   the spin does not yield.
5. Under a second acquisition, the local open table with a readback of each
   write: PLL output gate, AHB, AXI and core resets released (`0xb04` bits 18,
   17, 16), module selector and divider (`0xb00`), bus gate (`0xb04` bit 0),
   module gate (`0xb00` bit 31).
6. `a733_power_domain_request(row, 1)`. The step returns PENDING while
   `a733_power_domain_state(row)` is pending. The deadline is the request
   plus `A733_POWER_PCK_BOUND_US` (10 ms) plus two ticks of service latency.
7. **Settle.** When the row reads ON, sample the counter and set the settle
   end to that sample plus 2,000 µs. The step returns PENDING until a later
   counter read reaches it. A tick count alone is not used, because two ticks
   can end just over 1 ms after the sample. Then read back the final local
   state under a third acquisition.
8. Open the engine controller and activate the service.

Steps 5 and 6 keep the vendor order, clocks before domain power
(`vip_drv_device_platform.c` lines 419–428), which the owner's interface in
docs/power-domains.md also prescribes. Step 2 removes the supply wait from
the time that the clocks run into an unpowered domain; what remains is
bounded by the step 6 deadline, the PCK bound plus two ticks, which covers the
service's wake latency and its rail-pin retries.

The domain-4 row lives in startup storage. The bootstrap task writes its
request field and the service task writes its answer field, each with
release order; each reads the other's field with acquire order. The owner
writes the domain-4 delay row and policy word. The NPU platform stops writing
PCK600 and stops reading the delay row. Its native allowlist keeps the policy
and status reads (`PCK+0x0`, `PCK+0x8`), which the register pass, the final
readback and the stop check use. The PLL module keeps a `const` enable table
and a LOCK predicate. The platform module keeps `const` open and stop tables
and gains a gate table only when idle gating lands. Both use one
apply-with-readback helper.

**Second resource manager.** The startup constructs no GPU/PCK manager of
its own; the base's `a733_npu_startup_resources_init` and `struct
a733_npu_startup_resources` are deleted, because a board-derived image would
otherwise hold two managers of one PCK and its rail ledger is bound to the
board's. `apps/npu-convolution/native.c` constructs its image's manager as
one provider among the others, and the board image passes
`providers.shared.gpu`. Every image has one manager. The merged owner reads
the PPU clock bit but holds no lease on it, so the NPU keeps its PCK lease, as
G2D and the framebuffer do. Moving that lease into the owner, so that all
three drop theirs together, is a follow-up change for the power-domain owner.

**Reduced session record.** `struct npu_session` goes from 608 to 456
bytes. The PLL configuration with its 13 consumer snapshots goes (88 bytes). So do the
platform configuration's `owner`, `handoff` and `deadline_us`, which
initialization requires to equal `npu_owner`, `npu_evidence` and zero (24
bytes), and the profile's second copy of epoch, generation and lease (24
bytes). The `npu_established` word, which holds the `A733_NPU_HANDOFF_*`
flags, and `usb_established` go as well (16 bytes); the evidence conditions
of npu-platform.md carry their facts. The record keeps one fabric copy, the
PMIC configuration and the pin handoff. npu-platform.md removed the last two,
but every NPU image now runs the power service, whose PMIC provider reads
them. `NPU_SESSION_VERSION` is 3 for the single-core profiles (2 in the base)
and 2 for the multicore profile (1 in the base). The combined accelerator session
goes from version 1 to 2, because its embedded NPU record shrinks.
`prepare-supervised.py`, `apps/accelerators/prepare.py` and the session
checks change with it.

**Board image.** `cmake/a733-npu-board.cmake` calls
`sunstone_board_application(npu-board …)` the way `cmake/a733-g2d.cmake`
does: `apps/npu-board/boot.patch` and `extended-boot.patch` over the board's
supervised boot, `extended-link.patch` over `link-devices.ld`, and
`SUNSTONE_A733_EXTENDED_DEVICE_MEMORY=1` and `SUNSTONE_A733_IRQ_TABLE=1`.
`apps/npu-board/` supplies:

- `board_application_start`, which takes the runtime's parent, prepares the
  runtime, constructs the startup and its row, creates the bootstrap and the
  EL0 client task, binds the service and the interrupt, and pins the board's
  USB0 task to core 0;
- `board_application_task_privileged`, which admits the NPU worker and the
  bootstrap task at EL1t. `npu_runtime_startup_poll` refuses a caller below
  EL1 (`a733_npu_runtime.c` lines 784–785), and the bootstrap writes CCU
  registers;
- `admission.c`, which validates the record. The 808-byte record
  (`npu-board-session.h`) extends `struct a733_board_session`, as `struct
  a733_g2d_session` does, with NPU owner, command admission, supervisor, PCK
  client, catalogue digest and the four evidence digests. The board workload
  is 2: USB0 and the providers, no USB2 host and no radio. Admission requires
  the owner to differ from the board's owners and the PCK client to differ
  from the board session's, which the USB2 host would use; neither G2D nor the
  framebuffer is linked into this image. The record has version 1;
- `prepare.py` and `launch.c`, which follow their G2D counterparts, and the
  supervised capture `tools/check-a733-npu-board-supervised.py`. The launcher
  authenticates the physical-memory and remote-firmware exclusion that
  [docs/device-memory-capacity.md](../device-memory-capacity.md) requires for
  the version-2 extended boot record.

The preparer requires these conditions, each bound into its evidence digest:

| Digest | Conditions |
| --- | --- |
| Admission | `npu-startup-sram-confinement-drain-ownership`, `remote-writers-excluded`, `exclusive-cpus-memory-dma`, `pmic-bus-rail-pin-ownership`, `npu-irq97-disabled-inactive-clear`, the DDR reservation record for `0x48000000`–`0x79000000` |
| Clock | `supervised-cross-pll-disturbance-risk`, `exclusive-pll-npu-consumers-and-writers`, `independent-dram-phy-sources`, `no-autonomous-parent-selector-writers` |
| Power | `dcdc2-stable-800mv`, `authenticated-pmic-pin-inheritance` |
| Supervision | `manual-whole-board-power-removal`, `retain-all-resources-until-power-removal`, `stop-after-first-failure` |

The catalogue is the five models of `drivers/npu/npu_session_models.inc`.
`apps/npu-board/catalogue.py` checks each model file against
`tools/npu-session-models.json` and links it read-only, with
`npu_board_catalogue_digest`, the SHA-256 over the five model digests, which
the record must carry. One slot and one client serve every board step; a
second slot has no board step that needs it.

**Window hand-offs** (`apps/npu-board/client.h`). npu-breadth.md (data flow,
item 5) makes the read and write windows device-owned from SUBMIT until its
reply. After a LOAD the client publishes the tensor addresses and lends the
read window to the USB0 task with a release store of the window word; USB0
writes the input chunks into it. Before SUBMIT, USB0 hands it back with a
release store and a task notification, then posts the request. After the
reply the client lends the write window, and USB0 transmits the output from
it in place. USB0 hands it back the same way after the last output byte and
posts RETURN, and the client lends the read window again. The client sends
SUBMIT, LOAD or CLOSE only while it holds both windows and refuses a request
that arrives while one is lent. USB0 posts one request at a time and reads
the answer with an acquire load; neither task waits on a tick. LOAD carries a
catalogue index and is answered when the slot is READY or the load is
refused. The USB0 framing is described at the top of `apps/npu-board/usb.c`.

## Interrupts and polling

- **Startup.** The supply, row and settle waits are the PENDING returns
  above, one call per tick with a counter deadline. Supply verification and
  the 2 ms settle each last more than one tick, which section 1.3 permits.
  The row wait normally ends within the owner's 50 µs spin, so its tick poll
  adds at most one tick before the settle. The PCK600 has no
  known interrupt wiring, so the owner polls it.
- **Jobs.** `apps/npu-board/irq.c` holds the `a733_irq_routes` entry for
  SPI 65 (INTID 97): core 1, priority `0xd8`, which meets
  `A733_IRQ_TABLE_MIN_PRIORITY` (`0xb0`) as the OS entries do.
  `boot.patch` calls `a733_irq_table_route(admission.startup_deadline,
  a733_smp_counter)` at the point where the OS and I2S boot patches call it.
  The ISR masks the level-high SPI with `gicv3_spi_mask` before
  `npu_service_irq_notify_from_isr`. The bind, before the scheduler, checks
  once that `GICD_IROUTER` for INTID 97 (`GICD+0x6308`) holds core 1's
  affinity and that the source is disabled, inactive, not pending,
  level-sensitive and at its priority; the mask, rearm and stop callbacks then
  check only the calling core. The worker is pinned to core 1, and
  `npu_runtime_bind_irq` passes the route mask `1 << 1`, equal to that
  affinity. The CPU0 binding in `apps/npu-interrupt/irq.c` stays for the
  single-core images.
- **Unqualified delivery.** Until the board shows one interrupt per job, the
  runtime keeps its idle-register read per worker step and a one-tick timeout
  while a job runs (the section 1.3 heavier form).
- **Clients and USB0.** The client blocks on its reply queue and task
  notifications and never calls `vTaskDelay`. USB0 uses the shared loop of
  `apps/board/usb0_service.h`, at most 200 µs or 64 passes per tick, because
  USB0 interrupt delivery is unqualified.

## Memory placement and cache policy

`link-devices.ld` gives the image only the extended pools; the compact 2 MiB
pools do not exist there. The board's own allocations and the runtime's
parent all come from the 768 MiB write-back pool at
`0x48000000`–`0x78000000`, which EL1 maps with 2 MiB blocks. For one
ShuffleNetV2-sized slot, the placement rules of `npu_runtime_requirements`
give 8,388,608 bytes (counted from the code). The read-only group holds
2,953,216 bytes and ends at 4 MiB. The input group holds 151,552 bytes and the
pool and output group 1,097,728 bytes, and each ends on the next 2 MiB
boundary. `checks/npu-board-providers.py` runs the image's own construction
and reads the pool's slots afterwards; no runtime, service or client function
calls the allocator, so LOAD and CLOSE leave the free extents unchanged.

The runtime's cache policy per job, by address through
`aarch64_device_cache`:

- preparation cleans the inputs, cleans and invalidates the output lines, and
  zeroes the 1,093,632-byte pool with `DC ZVA`, then cleans and invalidates it;
- completion invalidates the outputs;
- the interim checks run: T and A before the trigger and H after the job,
  about 1.51 MB read per job; H holds back the SUBMIT reply until it ends;
- LOAD cleans the read-only bytes it wrote.

Startup, service and runtime state lives in kernel `.bss` (Normal write-back,
inner shareable). Each field has one writing task, except the row's request
and answer fields described above. The record is copied once into retained
memory and hashed once.

## Synchronization across cores

The power service runs on core 0, where the board places it, and
`board_application_start` pins the board's USB0 task there too. The
worker, the bootstrap task and the client run on core 1, the core that
receives INTID 97, so a completion needs no inter-processor interrupt and the
worker does not share a core with USB0. Section 2 item 7 moves them to cores 6
and 7 after the multicore work admits the A76 cores for this image. Requests
and replies use FreeRTOS queues. Startup takes the shared register lock three
times in total.

## Reuse and change

Reused unchanged: the power-domain owner and power service, the board
providers, `npu_service.c`, the controller, MMU builder and NBG assembler, the
USB0 shared loop, `irq_table.c`, `gicv3_spi.c` and `device_memory_layout.c`.
`a733_npu_runtime.c`, `npu_session.c` and `npu_convolution.c` change in the
same two places: `platform_step` calls the new step function without
comparing the handoff's backend, image and command copies or hashing the
models, and the activation callback checks the service's copy once and asks
the startup whether it is READY. `bootstrap_intact` stays in the runtime.

Removed: the PLL and platform stage machines, `apps/npu-convolution/clock.c`
and `clock.h`, `a733_npu_pll_native.c`, the startup's PMIC consumer, I2C bus
and 5,160 µs DCDC2 sampling, the NPU's domain-4 writes, delay-row reads and
adoption reads, and the per-poll model hashing in `npu_convolution.c`. The
removal table of npu-platform.md applies, except that the PCK lease, the PMIC
configuration and the pin handoff stay, for the reasons above.

## Register sequences and their sources

| Sequence | Source |
| --- | --- |
| PLL_NPU method-2 enable, LOCK predicate, consumer masks | A733 User Manual V1.00 §4.1.4.1–4.1.4.2, §4.1.6.56–60; the lock helper of the board clock source at Linux revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` ([docs/npu-pll.md](../npu-pll.md)) |
| Local open order, clocks before domain power | Pinned VIPLite `vip_drv_device_platform.c` lines 419–428 |
| Local stop | `npu_clk_uninit` in the pinned VIPLite platform |
| Domain-4 delay row, command and status | Owner code, from `pck600_domains.c` lines 126–142 and 547–569 at the same revision. **Unproven:** words `0xc00`–`0xc10` are absent from the PCK-600 manual |
| 2 ms settle after domain ON | **Unproven:** no pinned source; kept from the current controller and measured on the board |

## Accelerator image link failure

The `apps/accelerators` project fails to link at `66e0ef6`. The single-core
NPU image and the accelerator image report every FreeRTOS `tasks.c` symbol
undefined. Commit `f4a272f` patched `tasks.c` for the SMP base. Its filter
removes the patched copy from single-core images, and that commit added the
kernel's own `tasks.c` to two single-core recipes but not to
`cmake/a733-npu-single-core.cmake`. This package adds the one line there.
With it, all three images link.

## The four checks

`checks/npu-image-checks.py` builds the images of a configured
`apps/accelerators` build and, when given, the root `sunstone-a733-npu-board`,
with `nice -n 10` and six jobs. It converts the reviewed session fixtures,
608-byte version-2 NPU pages and the 9,080-byte combined session, to the
reduced layout the image's DWARF and layout words describe, and invokes each
check, which keeps its own result record:

| Check | Images | Change |
| --- | --- | --- |
| `npu-application-clock-reference.py` | Single-core, accelerator, board | The six reference cases drive the image's sequencer through its register pass; refused cases make no write and admitted ones write the enable table, first the gate enable at `CCU+0x1920` |
| `npu-stop-native.py` | All four | Layout reader restricted to no structure; adds refused domain-4 policy and delay-row writes and the PLL gate rules, fourteen cases |
| `npu-single-core.py --retention-only` | Single-core | Restricted layout reader; reduced-record fixture at version 3 |
| `accelerator-providers.py` | Accelerator | The view of the application's one GPU/PCK manager, the startup's borrowed providers and its single domain-4 row; a domain-4 fault leaves the shared providers live |
| `npu-board-providers.py` | Board | New; see Memory placement |

`checks/storage-read-native.py` refuses images in which two compilation units
define different structures with one tag; `a733_pmic.c` and
`a733_power_domains.c` both define `struct profile`. Each check passes
`layout_names` to `Image` with the structures it reads, as
`checks/displayport-admission.py` does, so no source is renamed.

Two of the checks failed on the base for reasons outside the record change:

- `npu-single-core.py --retention-only` stopped at its third case because
  `a733_smp_device_preparation_admitted` returned 0. Since the boot began
  preparing the local pools in `platform_init`, a kernel critical section runs
  there before the scheduler. In the single-core port, `uxPortSetInterruptMask`
  masks IRQs through the priority mask and clears DAIF.I, so the admission's
  DAIF check refused. `platform_init` now sets DAIF.I again after the
  preparation, which restores the boot's mask; the board image would have
  refused in the same way.
- `accelerator-providers.py` returned reason 5 from `a733_npu_application_init`
  because the check skipped that same boot preparation, so the NPU reservation
  read a NULL pool. The check now runs both pool preparations first and models
  the port's critical sections they enter.

## Budgets

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Boot to READY, board image, scheduler start to READY, PMIC at 400 kHz | No board image | ≤ 12 ticks: supply verification ends 8.4 ms after the service's first step, so the done bit is seen at tick 8 or 9; that call runs the pass, the enable table, LOCK, the local open table and the row request; the next call sees ON and samples the counter; the 2 ms settle ends two ticks later | `checks/npu-startup-host.c`, with the actual PMIC provider, ledger and owner |
| Startup calls that advance a stage | 9 harness polls (3 local, 6 PLL); 8–9 calls in `checks/npu-startup.c` | 4: tokens; supply done through the row request; row ON and the settle sample; settle end and the final readback | Same |
| Startup register reads, cold method-2 path, LOCK reads excluded | 90 (51 local, 39 PLL) | ≤ 60 | Same |
| Startup register writes | 18, of which 6 to PCK600 | ≤ 12, none to PCK600 | Same |
| Register-lock acquisitions in startup | 7 (2 local, 5 PLL) | 3 | Same |
| Counter reads in startup | 31, with 3 LOCK samples | ≤ 16 in the calls that advance; one in each PENDING call | Same; LOCK spacing reads the raw counter |
| PMIC traffic in startup | 12 register reads, 6 bus-state reads, 5,160 µs wait | None | Same; the owner's once-per-boot verification is counted in docs/power-domains.md |
| Bytes hashed by startup polls, accelerator image | 20.7 MB (11 calls × 1,881,936) | 0 | Disassembly of the linked image: no digest call reachable from the startup poll |
| GPU/PCK managers in an image with the NPU | 1 (accelerator); 2 in a board-derived image | 1 | `accelerator-providers.py`, `npu-board-providers.py` |
| `sizeof(struct npu_session)` | 608 bytes | ≤ 456 bytes | Geometry word in the linked single-core image |
| Source lines, code + header | Startup 445 + 131; platform 223 + 96; PLL 204 + 109; adapter 128 + 19 | ≤ 260 + 60; ≤ 90 + 30; ≤ 60 + 25; adapter deleted | `wc -l` |
| Runtime parent, board image | No board image | 8,388,608 bytes from the 768 MiB pool | `npu-board-providers.py` |
| Largest free extent after admission; free extents after LOAD and CLOSE | Not modeled | ≥ 8,388,608 bytes; unchanged | `npu-board-providers.py`: the pool's slots, and no allocator call in runtime, service or client code |
| Requests, replies and client tick waits per inference | Runtime check: 1 request, 1 reply; the client in `apps/npu-runtime/main.c`: one `vTaskDelay(1)` per tick of waiting | 1, 1, 0 | `checks/npu-session.py --mode board-client` |
| Copies per tensor byte, counting the TinyUSB FIFO copy | No board image | Input 1 (`tud_vendor_read`), output 1 (`tud_vendor_write`) | Same check, counting the bytes moved through the windows |
| Cache lines maintained per job | 19,442 | 19,442 | `checks/npu-session.py --mode runtime` |
| Bytes zeroed per job | 1,093,632 | 1,093,632 | Same |
| Integrity-check bytes read per job | 1,509,856 | 1,509,856 | Same |
| G, P, H, W, ShuffleNetV2 slot | 1.44, 5.41, 2.52, 8.22 ms with USB0 on the worker's core | 1.15, 4.33, 2.02, 6.58 ms, without the 1.25 USB0 factor | `checks/npu-runtime-bounds.py` |
| Worker wakes, idle | 0 (counted from the code: the worker blocks) | 0 | Counted from the code |
| Worker wakes and idle-register reads per job | One per tick of execution plus one read per worker step | Same until the board shows one INTID 97 per job; then one wake per job | `checks/npu-session.py --mode runtime`, case 2 |
| USB0, 64 KiB response | 13 ticks in the shared-loop model | 13 ticks | `checks/usb0-service-burst.py` |

The interim rows stay at their npu-breadth values until the fixed mappings
and the pool comparison remove T, A, H and the pool clear.

## Failure classes

| Failure | Class (section 1.8) | Retained |
| --- | --- | --- |
| Token or lease refused, supply refused or its deadline expired, register-pass mismatch | Refused before any NPU write | Nothing; tokens released |
| LOCK not seen in 100 observations | Unknown | Clock, rail and PCK tokens; output gate closed; PLL left enabled |
| Row REFUSED or FAILED, row deadline expired, readback mismatch, any deadline after the first write | Unknown; supervised whole-board power removal is the only recovery | Tokens, the row with its request (it cannot be withdrawn), the reservation; the register lock only if a table stopped midway |
| Runtime or controller fault | As in npu-breadth.md | As there |

## Left out

- Idle gating, the 10 ms fallback, cancel and recovery deadlines and REAP
  folding (npu-platform.md). They change the service worker and are not
  needed to start the NPU or link the runtime.
- Moving the single-core and accelerator images to the runtime. Their USB
  protocol and three of the four checks are built on the convolution path;
  the board image is the runtime's first consumer.
- A second slot and client, for the reason given above.
- The 852 MHz point, SHA2-instruction digests, read-only EL1 mappings, NPU MMU
  pages larger than 4 KiB and A76 placement. Each needs board evidence or
  another owner.
- Shader graphs and a section-order decoder (npu-breadth.md).

## Edits outside the owned files (primary.patch)

- `CMakeLists.txt`: option `SUNSTONE_A733_NPU_BOARD` and the include of
  `cmake/a733-npu-board.cmake`; the removed `A733_NPU_HANDOFF_SOURCE` path and
  the include of `cmake/npu-clock-handoff.cmake`.
- `platform/a733/platform.c`: DAIF.I set again after the local pool
  preparation (the four checks, above).
- `checks/power-domains-tree.py`: four delay-row writers, no native allowlist
  that admits the delay row, domain-4 rows accepted.
- `cmake/npu-startup.cmake`, `cmake/npu-convolution.cmake`: fixture source
  lists without the clock adapter, the PLL native file and the PMIC consumer,
  with the owner sources. `cmake/npu-clock-handoff.cmake`,
  `checks/a733-npu-pll-clock-reference.c` and
  `checks/a733-npu-platform-clock-reference.c` are deleted with the PLL
  adoption and stage machines they exercised.
- `checks/a733-pmic-consumers.c`: the NPU startup cases removed; the GPU
  consumer cases build their own GPU/PCK manager.
  `checks/a733-accelerator-clock-reference.py`: GPU cases only; the NPU
  reference cases run in `checks/npu-startup-host.c` and
  `npu-application-clock-reference.py`.
- `docs/power-domains.md`: domain 4 in use by the NPU startup.
- `checks/index.json`, which `checks/check-index.py` regenerates.

`tools/check-a733-npu-supervised.py` needs no change: it validates through
`prepare-supervised.py`, which reads the version-3 record.

## Board evidence

All board steps are planned for the coordinator; this package touches no
board. One session per whole-board power removal:

1. The register-pass words and domain-4 status after boot, against the
   constants, and the measured PLL_NPU lock time.
2. Domain-4 time to ON through the owner, a status trace, the reads of
   `0xc00`–`0xc10` before and after the delay-row write
   (docs/power-domains.md, board evidence), and whether the final local
   state holds without the 2 ms settle.
3. Boot to READY in the board image, ShuffleNetV2 output equal to the SDK
   reference, and a LOAD that switches the slot between two catalogue models.
4. One INTID 97 interrupt per job on core 1.
5. USB0 throughput and power-service step durations while jobs run.
6. The extended-memory reservation evidence that
   docs/device-memory-capacity.md requires.
