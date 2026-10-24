# Design: NPU startup on the power-domain owner, and a board image with the NPU runtime

Status: **design**; no code exists yet. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md). Startup
baselines are those of [npu-platform-measurements.md](npu-platform-measurements.md).
Image and check results come from images built on 2026-10-24 from commit
`66e0ef6` with the link fix described below. No figure comes from the board.

This package does three things:

- It moves NPU startup onto the power-domain owner
  ([docs/power-domains.md](../power-domains.md)). It also implements the single
  startup sequencer and the reduced session record that
  [npu-platform.md](npu-platform.md) designed and deferred.
- It builds `sunstone-a733-npu-board`, a supervised image derived from the
  board image that links the NPU runtime of [npu-breadth.md](npu-breadth.md).
- It makes the four host checks that need separately configured accelerator
  images build and pass.

## Ownership and data flow

**One sequencer.** `a733_npu_startup.c` becomes the only NPU startup state
machine. It receives a borrowed provider view: counter, clock registry,
register lock, the image's one GPU/PCK resource manager, the rail ledger and
the power-domain owner. Construction of providers stays with the image. The
bootstrap task calls one step function, which runs steps until one waits:

1. Acquire one shared DCDC2 rail token, one clock-registry token (exclusive
   PLL_NPU and NPU-local; shared reference, oscillator, fabric and PERI0) and
   one stable OSC and PCK lease from the image's manager. The merged owner reads
   the PPU clock bit but holds no lease on it, so the NPU keeps this lease, as
   G2D does.
2. Under one register-lock acquisition, `a733_power_domain_check(domains, 4)`
   and the 29- or 30-word register pass of npu-platform.md, which compares
   each word under a mask with a constant. Startup admits only domain 4 OFF
   and PLL_NPU disabled.
3. Under the same acquisition, the PLL_NPU method-2 enable table. Then release
   the lock and sample LOCK without it: three observations 3 µs apart, then
   20 µs apart, at most 100.
4. Under a second acquisition, the local open table with a readback of each
   write: PLL output gate, AHB, AXI and core resets released (`0xb04` bits 18,
   17, 16), module selector and divider (`0xb00`), bus gate (`0xb04` bit 0),
   module gate (`0xb00` bit 31).
5. `a733_power_domain_request(row, 1)`, then `a733_power_domain_wait(row,
   deadline)`. When the row reads ON, wait 2 ms, then read back the final
   local state under a third acquisition.
6. Open the engine controller and activate the service.

The row for domain 4 is registered before the scheduler starts, with the
startup's rail token. The owner then writes the domain-4 delay row and policy
word. The NPU platform stops writing PCK600 and loses those offsets from its
native allowlist. The PLL module keeps a `const` enable table and a LOCK
predicate, the platform module keeps `const` open, gate and stop tables, and
both use one apply-with-readback helper.

**Second resource manager.** `a733_npu_startup_resources_init` constructs a
GPU/PCK manager inside the startup today. The supervised board image already
owns one, and its rail ledger is bound to that one, so a derived image would
hold two managers of one PCK. The function and `struct
a733_npu_startup_resources` are deleted. `apps/npu-convolution/native.c`
constructs its image's manager as one provider among the others. The board
image passes `providers.shared.gpu`. Every image then has one manager.

**Reduced session record.** `struct npu_session` is 608 bytes today. The PLL
configuration with its 13 consumer snapshots goes (88 bytes). So do the
platform configuration's `owner`, `handoff` and `deadline_us`, which
initialization requires to equal `npu_owner`, `npu_evidence` and zero (24
bytes), and the profile's second copy of epoch, generation and lease (24
bytes). The `npu_established` word, which holds the `A733_NPU_HANDOFF_*`
flags, and `usb_established` go as well (16 bytes); the evidence conditions
of npu-platform.md carry their facts. The record keeps one
fabric copy, the PMIC configuration and the pin handoff. npu-platform.md
removed the last two, but every NPU image now runs the power service, whose
PMIC provider reads them. The version becomes 3.
`prepare-supervised.py`, `apps/accelerators/prepare.py` and the session
checks change with it.

**Board image.** `cmake/a733-npu-board.cmake` calls
`sunstone_board_application(npu-board …)` with the extended-memory linker
script `link-devices.ld` and `SUNSTONE_A733_EXTENDED_DEVICE_MEMORY`, the
pattern of the G2D and HEVC images. `apps/npu-board/` supplies the board
application hooks: `board_application_start` registers the row, constructs
the startup and the runtime, binds two EL0 client tasks and creates the
bootstrap and USB0 tasks. `board_application_task_privileged` admits the NPU
worker at EL1t, and an `admission.c` validates the record. The NPU record extends
`struct a733_board_session`, as `struct a733_g2d_session` does, with NPU owner,
command admission, PCK client, catalogue digest, the four evidence digests and
supervisor identity. The board session already carries the fabric, PMIC and
USB fields. The catalogue is the five models of
`drivers/npu/npu_session_models.inc`, which the image links read-only.

Data path per inference: USB0 receives the request and writes the input
chunks into the client's read window, which the client lent to the USB0 task
by notification. The client sends SUBMIT to the runtime and blocks on its
reply queue. On the reply, it lends the output in the write window to the USB0
task, which transmits it in place. LOAD carries a catalogue index and is
answered when the slot is READY or the load is refused.

## Interrupts and polling

- **Startup.** The PLL LOCK samples spin for microseconds with no lock held.
  `a733_power_domain_wait` blocks on a row semaphore that the service gives
  after it publishes the row state; docs/power-domains.md names this function
  for this caller. The 2 ms settle is a tick wait, which section 1.3 permits.
  The PCK600 has no known interrupt wiring, so the owner polls it.
- **Jobs.** The board image routes SPI 65 (INTID 97) to core 1 through an
  `a733_irq_routes` table in `apps/npu-board/irq.c`. The EL3 installer and
  dispatcher in `platform/a733-smp-cached/irq_table.c` program it, as for the
  OS and I2S images. The worker is pinned to core 1 and binds with
  `npu_service_bind_irq`, which the runtime requires. Until the board
  qualifies delivery, the runtime keeps its idle-register read per step and a
  one-tick timeout while a job runs (the section 1.3 heavier form).
- **Clients and USB0.** Clients block on reply queues and task notifications
  and never call `vTaskDelay`. USB0 uses the shared loop of
  `apps/board/usb0_service.h`, at most 200 µs or 64 passes per tick, because
  USB0 interrupt delivery is unqualified.

## Memory placement and cache policy

The runtime's parent allocation, `npu_runtime_requirements` bytes for the
five-model catalogue and two slots, comes from the 768 MiB write-back pool at
`0x48000000`–`0x78000000`. EL1 maps that pool with 2 MiB blocks. The board's
compact 2 MiB pool stays with the board's own users. It is smaller than the
2,195,456 bytes that [docs/audit/README.md](../audit/README.md) records for
three catalogue models, and the runtime's three groups, each starting on a
2 MiB boundary, need at least 6 MiB. The runtime keeps its cache policy:
inputs cleaned and outputs invalidated by address per job, LOAD cleaning the
bytes it wrote. Startup, service and runtime state lives in kernel `.bss`
(Normal write-back, inner shareable), and each structure has one writing
task. The record is copied once into retained memory and hashed once.

## Synchronization across cores

USB0 and the power service run on core 0, as the board places them. The
worker, the bootstrap task and both clients run on core 1, the core that
receives INTID 97, so a completion needs no inter-processor interrupt. Section 2
item 7 moves them to cores 6 and 7 after the multicore work admits the A76
cores for this image. Requests and replies use FreeRTOS queues. Row requests
and answers use the owner's release and acquire stores. A window lent
between USB0 and a client passes with a release store of (slot, length)
followed by a task notification. Startup takes the shared register
lock three times in total.

## Reuse and removal

Reused unchanged: the power-domain owner and power service, the board
providers, `a733_npu_runtime.c`, `npu_service.c`, the controller, MMU builder
and NBG assembler, the USB0 shared loop, `irq_table.c`, `gicv3_spi.c` and
`device_memory_layout.c`. Removed: the PLL and platform stage machines,
`apps/npu-convolution/clock.c` and `clock.h`, `a733_npu_pll_native.c`, the
startup's PMIC consumer, I2C bus and 5,160 µs DCDC2 sampling, the NPU's
domain-4 writes and adoption reads, and the per-poll model hashing in
`npu_convolution.c`. The removal table of npu-platform.md applies, except that
the PCK lease, the PMIC configuration and the pin handoff stay, for the
reasons above.

## Register sequences and their sources

| Sequence | Source |
| --- | --- |
| PLL_NPU method-2 enable, LOCK predicate, consumer masks | A733 User Manual V1.00 §4.1.4.1–4.1.4.2, §4.1.6.56–60; board clock source at Linux revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` ([docs/npu-pll.md](../npu-pll.md)) |
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

`checks/npu-image-checks.py`, a new runner, does the following:

1. Configures `apps/accelerators` under `~/.cache/sunstoneos/build/npu-board/`
   with the standard cross options, the complete-graph model, input and
   reference, and the pinned GPU firmware. It builds the single-core, multicore
   and accelerator images and the root `sunstone-a733-npu-board`, with
   `nice -n 10` and six jobs.
2. Writes session and contract fixtures from each image's DWARF layout. It
   uses `admission_fixture` from `checks/npu-supervised-host.py`,
   `request_bytes` from `prepare-supervised.py`, and `assemble_session` from
   `apps/accelerators/prepare.py`, with the GPU package frozen in
   `gpu-npu-composition-20261020`. Today's fixtures predate the record changes
   and cannot be reused.
3. Runs the checks and records image, fixture and result hashes in one
   `result.json`.

| Check | Images | Change | Result at `66e0ef6` with the link fix |
| --- | --- | --- | --- |
| `npu-application-clock-reference.py` | Single-core, accelerator, board | The six reference cases drive the register pass; refused cases make no write and accepted ones write the enable table first | Pass, six cases, with the layout reader below; the original reader refuses |
| `npu-stop-native.py` | All four | Uses `checks/npu-large-native.py` as layout helper; adds a refused domain-ON write to `0x07064000` | Pass, ten cases, on the three images |
| `npu-single-core.py --retention-only` | Single-core | Layout reader below; reduced-record fixture | Fails at its third case: `a733_smp_device_preparation_admitted` returns 0 in the model; cause not yet found |
| `accelerator-providers.py` | Accelerator | Provider view without `a733_npu_startup_resources`, with the domain owner; a domain-4 FAILED row revokes the NPU only | Fails: `a733_npu_application_init` returns 0 (reason 5) with the 20 October fixtures |

`checks/storage-read-native.py` refuses images where two compilation units
define different structures with one tag. `a733_pmic.c` and
`a733_power_domains.c` both define `struct profile`. The NPU checks switch to
the reader in `checks/npu-large-native.py`, which drops ambiguous tags.
A fifth check, `checks/npu-board-providers.py`, runs `board_application_start`
of the board image in Unicorn. It asserts one manager, one domain-4 row and a
runtime parent of `npu_runtime_requirements` bytes inside the extended pool.

## Budgets

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Startup calls, first step to READY | 9–10; 16–17 with PLL adoption | ≤ 4 | Call counter in `checks/npu-startup.c`, two QEMU cores |
| Bootstrap ticks to READY | 13–14; 18–19 with adoption | ≤ 4 | Same fixture |
| Startup register reads and writes, without LOCK samples | 94 reads; 19 writes plus the NPU's domain-4 writes | ≤ 60 reads, ≤ 12 writes, no PCK600 write | `checks/npu-platform-budget.c` register model |
| Register-lock acquisitions in startup | 7 | 3 | Same |
| Counter reads in startup | 31 | ≤ 16 plus one per LOCK sample | Same |
| PMIC traffic in startup | 12 register reads, 6 bus-state reads, 5,160 µs wait | None | Same; the owner's once-per-boot verification is counted in docs/power-domains.md |
| Bytes hashed by startup polls, accelerator image | 20.7 MB (11 calls × 1,881,936) | 0 | A digest counter added to `checks/npu-convolution.c` |
| GPU/PCK managers in an image with the NPU | 1 (accelerator); 2 in a board-derived image | 1 | `accelerator-providers.py`, `npu-board-providers.py` |
| `sizeof(struct npu_session)` | 608 bytes | ≤ 456 bytes | Geometry word in the linked single-core image |
| Source lines, code + header | Startup 445 + 131; platform 223 + 96; PLL 204 + 109; adapter 128 + 19 | ≤ 260 + 60; ≤ 90 + 30; ≤ 60 + 25; adapter deleted | `wc -l` |
| Device pool for the NPU, board image | 2 MiB compact pool | ≥ `npu_runtime_requirements` for five models and two slots, from 768 MiB | `npu-board-providers.py` |
| Requests per inference; client tick waits | Runtime check: 1 and 1; no board image | 1 request, 1 reply; 0 tick waits | `npu-board-providers.py` hooks on queue sends and `vTaskDelay` |
| Copies per byte after USB0 transport | No board image | Input 1, output 0 | Counted from the code; the check asserts window addresses |
| USB0, 64 KiB response | 13 ticks in the shared-loop model | 13 ticks | `checks/usb0-service-burst.py` |
| Worker wakes per second, idle, board image | Not counted | 0 | A wake counter added to `checks/npu-session.py --mode runtime` |

## Failure classes

| Failure | Class (section 1.8) | Retained |
| --- | --- | --- |
| Token or lease refused, register-pass mismatch | Refused before any NPU write | Nothing; tokens released |
| LOCK not seen in 100 samples | Unknown | Clock, rail and PCK tokens; output gate closed; PLL left enabled |
| Row REFUSED or FAILED, readback mismatch, deadline after the first write | Unknown; supervised whole-board power removal is the only recovery | Tokens, row, reservation; the register lock only if a table stopped midway |
| Runtime or controller fault | As in npu-breadth.md | As there |

## Left out

- Idle gating, the 10 ms fallback, cancel and recovery deadlines and REAP
  folding (npu-platform.md). They change the service worker and are not
  needed to start the NPU or link the runtime.
- Moving the single-core and accelerator images to the runtime. Their USB
  protocol and three of the four checks are built on the convolution path;
  the board image is the runtime's first consumer.
- The 852 MHz point, SHA2-instruction digests, read-only EL1 mappings, NPU MMU
  pages larger than 4 KiB and A76 placement. Each needs board evidence or
  another owner.
- Shader graphs and a section-order decoder (npu-breadth.md).

## Edits outside the owned files (primary.patch)

- `CMakeLists.txt`: option `SUNSTONE_A733_NPU_BOARD` and the include of
  `cmake/a733-npu-board.cmake`.
- `drivers/power/a733_power_domains.c`, `include/a733_power_domains.h`:
  `a733_power_domain_wait` and the row semaphore.
- `checks/power-domains-tree.py`: four delay-row writers, no NPU allowlist,
  domain-4 rows accepted.
- `cmake/npu.cmake`, `cmake/npu-startup.cmake`, `cmake/npu-clock-handoff.cmake`,
  `cmake/npu-convolution.cmake`: fixture source lists without the clock
  adapter and PLL native file, with the owner sources.
- `checks/a733-pmic-consumers.c`, `checks/a733-npu-pll-clock-reference.c`,
  `checks/a733-npu-platform-clock-reference.c`,
  `checks/a733-accelerator-clock-reference.py`: the NPU PMIC consumer case
  removed; reference checks on the register pass constants.
- `tools/check-a733-npu-supervised.py`: the version-3 record.
- `checks/index.json`, which `checks/check-index.py` regenerates.

## Board evidence

All board steps are planned for the coordinator; this package touches no
board. One session per whole-board power removal:

1. The register-pass words and domain-4 status after boot, against the
   constants.
2. Domain-4 time to ON through the owner and a status trace; whether the
   final local state holds without the 2 ms settle.
3. Startup to READY in the board image, ShuffleNetV2 output equal to the SDK
   reference, and a LOAD that switches one slot between two catalogue models.
4. One INTID 97 interrupt per job on core 1.
5. USB0 throughput and power-service step durations while jobs run.
6. The extended-memory reservation evidence that
   [docs/device-memory-capacity.md](../device-memory-capacity.md) requires.
