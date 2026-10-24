# SunstoneOS code redesign and clean-up

This report covers the structure of the SunstoneOS code base at commit
`70a4e63`. It describes how the boot path, kernel ports, drivers, shared
providers, supervised applications, build and host tools fit together. It then
groups the confirmed system-design findings of a large-scale audit into
themes. Each theme states the problem, the evidence, a proposed design, the
migration steps and the requirements the change must preserve.

Every finding has an identifier of the form `area-N`, for example
`supervisor-apps-3`. The identifiers name audit records; they appear here so a
reader can trace a recommendation back to its evidence. Severities (low or
medium) and the corrections recorded during verification are already applied
to the text below. Line numbers refer to commit `70a4e63`.

The reader is assumed to know C, AArch64 exception levels, FreeRTOS tasks and
queues, and CMake. Terms specific to this project are defined where they first
appear.

## 1. How the system is put together

### 1.1 Boot and execution

The board is an Orange Pi Zero 3W with an Allwinner A733 system on chip: eight
Arm cores (six Cortex-A55 and two Cortex-A76), a GIC-600 interrupt controller
and 12 GiB of DDR. During development the board is driven over USB in FEL
mode, the boot ROM's USB download mode. A host tool writes the image and a
small SRAM program called the launcher. The launcher arms watchdog WDT0 with a
4-second interval, checks a 224-byte admission record called the contract, and
enters the EL3 monitor. The monitor configures the interrupt controller and
switches to nonsecure EL1, where FreeRTOS runs.

Two FreeRTOS ports are in use:

- a single-core port, used by the board, GPU, NPU, thermal, RTC, storage and
  other "single-core" images;
- a patched eight-core SMP port (the FreeRTOS ARM_CR82 port) with caches
  enabled, used by the board-derived device images and by the native mimus
  recognizer. A two-core QEMU target exists for host modelling.

Every image is a time-limited diagnostic. It admits itself against the
contract, runs for at most 2, 20 or 60 seconds depending on the profile, and
returns to the boot ROM. A separate cold-entry recorder exists for microSD
boot, but nothing hands off from it to a running FreeRTOS image.

### 1.2 Drivers, adapters and providers

Drivers are bounded, poll-driven state machines. A poll performs at most one
register operation, deadlines are checked on every step, and an uncertain
failure moves the driver into a terminal state that retains (does not release)
its resources. Owner tasks call `poll` once per 1 ms FreeRTOS tick.

Each driver reaches hardware through a native adapter, a small file named
`*_native.c`. The adapter admits a register access only when four conditions
hold: the caller is the driver's designated owner task, it runs at EL1 outside
interrupt and critical-section context, it holds the platform register mutex,
and the address is on the driver's allowlist.

Shared hardware is coordinated by provider objects:

- the counter clock and the platform locks (`struct a733_platform_locks`);
- the 128-slot clock and scope registry;
- the 39-output power-rail ledger;
- the bank-qualified pin manager;
- the read-only AXP8191 PMIC snapshot provider;
- the GPU resource manager;
- the device-memory pool, which hands out generation-checked leases on
  reserved write-back (WB) and Normal non-cacheable (NC) memory.

Some controllers sit behind a FreeRTOS service: a worker task owns the
controller and clients send requests over a queue. The DMA, GPU and NPU
controllers use this pattern.

### 1.3 Supervised applications

[apps/board](../../apps/board/) is the shared board composition. It
constructs every provider inside one structure, `struct a733_native_usb`
([include/a733_native_usb.h](../../include/a733_native_usb.h)), and runs the
USB0 vendor channel that carries evidence to the host, the Type-C
controller, the USB2 host, the radio and a sealed board session. The board
session is a page uploaded by the host at `0x41f00000`, copied into private
memory and checked against a SHA-256 digest held in the contract.

A device application plugs into the board through three functions declared in
[apps/board/application.h](../../apps/board/application.h):
`board_application_task_privileged`, `board_application_pins` and
`board_application_start`. Each application also supplies:

- a unified-diff patch to the EL3 boot code (`boot.patch`) and usually one to
  the IRQ dispatcher (`platform.patch`);
- its own session extension appended to the board session, its own contract
  and result magic numbers, and its own USB0 wire protocol;
- a `prepare.py` script that builds the session and generates the launcher by
  text substitution on a shared launcher source;
- a host wrapper `tools/check-a733-*-supervised.py` that claims a slot in the
  board-test queue and runs the session.

Twenty-two applications define the three board hooks. Twelve image roots,
including the board itself and the camera, accelerator, GPU, NPU, I2S and
native mimus images, define their own `a733_smp_diagnostic_start` entry. The
NPU and accelerator images form a separate single-core lineage with their own
provider instances.

### 1.4 Build

The build has a root [CMakeLists.txt](../../CMakeLists.txt), about 69
fragments under [cmake/](../../cmake/) and about 30 standalone CMake projects
under `apps/` and `platform/`. Together they define about 81 executables, all
`EXCLUDE_FROM_ALL`. An image is usually made by copying another target's
`SOURCES` list and patching copies of `boot.c`, `platform.c` and `link.ld`
with in-repository `.patch` files (more than 100 files, applied at about 150
sites). Each image's build identity, which the contract compares at
admission, is a SHA-256 over a hand-written list of inputs computed at
configure time.

### 1.5 Where the structure works against one integrated image

The goal in [docs/goal.md](../goal.md) is one FreeRTOS image that supports all
usable onboard hardware and runs the mimus recognizer. The current structure
blocks that in seven places:

1. **One application per image.** The three board hooks are single symbols,
   so a second application cannot be linked in (`supervisor-apps-1`).
2. **Composition by text patch.** EL3 hooks, GIC routing and IRQ dispatch are
   added by per-application diffs that edit the same hunks of the same files
   (`supervisor-apps-2`, `x-concurrency-5`).
3. **An overloaded contract.** The contract's six reserved words mean "board
   session digest" in one image family and "device-memory layout" in another,
   so an application that needs both forks the boot code (`supervisor-apps-3`,
   `bringup-gaps-11`).
4. **Two provider roots.** The board and the NPU/accelerator lineage each
   construct their own clock registry, PMIC owner and rail ledger
   (`supervisor-apps-10`).
5. **Hand-placed memory.** The recognizer's model slots and arena are literal
   addresses in more than ten files, outside the memory-reservation tables
   (`bringup-gaps-12`).
6. **Per-image recompilation.** Target-wide macros that only the boot path
   needs force each image to recompile the full board source set
   (`build-system-8`).
7. **Order-dependent configuration.** Native eight-core images include the
   two-core QEMU configuration directory and depend on include order to
   override it (`smp-8`, `build-system-11`).

Beneath these sit many copies of the same mechanism. The largest families
are the USB0 vendor transport (about 25 close copies), the task-context
admission check (about 15 copies), private address-range helpers (about 190
definitions), page-mapping checks (about 40) and MMIO accessors (about 60
files). A fix to any of them has to be repeated in every copy, and several
copies have already diverged.

## 2. Requirements every change must preserve

The project's working rules ([AGENTS.md](../../AGENTS.md)) prefer minimal
designs, fixed board configuration and consolidation of duplicate mechanisms.
They also require that consolidation keep the following properties. Each
theme below names the ones it touches.

- **Ownership.** Every register block, clock, rail, pin, interrupt and memory
  lease has one owner task. Adapters refuse access from any other task, from
  interrupt context or inside a critical section. Tokens are authenticated
  and lock order is fixed.
- **Retention.** After an uncertain failure, resources stay retained rather
  than released. Finite applications refuse REAP or CLOSE while hardware
  completion is unknown. A shared helper must not release anything its
  callers would have retained.
- **Coherence.** Memory the CPU shares with a DMA master is cleaned before
  the device reads it, clean-invalidated before the device writes it and
  invalidated after quiescence, with `DSB SY` on both sides. Mapping checks
  confirm the attributes (WB `0xff`, inner shareable; NC `0x44`) before a
  device sees an address.
- **Bounded failure.** Every wait has a deadline. Admission fails closed:
  any mismatch in a contract, session or register read-back refuses the
  launch with a distinct stop code.
- **Evidence identity.** Recorded physical and host evidence is bound to an
  image's build identity. Images whose evidence has been accepted stay frozen
  at their recorded commits. A refactor changes the identity of every image
  it touches, so migrated images need new evidence and old evidence does not
  carry over.

## 3. Theme: application composition and the board hook set

Findings: `supervisor-apps-1`, `supervisor-apps-10`, `camera-7`, `gpu-5`,
`mimus-perf-6`.

### Problem

The board hooks hold one application. Twenty-two applications define the same
three symbols, so any two of them collide at link time. Their validators also
conflict: the monitor requires session workload 2, while the mimus-board image
refuses anything other than workload 0 or 4. Some multi-device images already
exist, built by hand: the accelerator image (GPU and NPU,
[docs/accelerator-composition.md](../accelerator-composition.md)), the
monitor (ADC and thermal), mimus-board workload 4 (keyboard, recognition and
storage) and gpu-display. Each new combination needs a new bespoke
application that redoes the session envelope, owner identifiers, pin and
memory admission, and the USB0 protocol (`supervisor-apps-1`).

The NPU and accelerator lineage builds a second instance of every shared
provider inside the NPU application's static state, on the single-core port
and with a different 64 KiB session format at the same address
(`supervisor-apps-10`). Because `struct a733_native_usb` is named after USB0,
the fact that it is the board-wide provider root is easy to miss.

The camera application reuses the storage-maintenance target, removes about
12 storage sources from it and adds its own main, session snapshot, admission
and USB0 loop. ISP0, its sibling, uses the board hooks (`camera-7`). GPU reuse
after CLOSE exists only in an `#ifdef` build reached through a symbol that the
gpu-render application defines, so a generic driver depends on an
application (`gpu-5`).

### Evidence

- [apps/board/main.c](../../apps/board/main.c) lines 331-333 call
  `board_application_start()` and panic on failure.
- [apps/npu-convolution/native.c](../../apps/npu-convolution/native.c)
  lines 195-196 return pointers into the NPU application's own clock,
  registry, locks, GPU manager and PMIC.
- [cmake/a733-camera-application.cmake](../../cmake/a733-camera-application.cmake)
  lines 9-31 copy and filter the storage-maintenance sources.
- [drivers/gpu/pvr_gpu_service.c](../../drivers/gpu/pvr_gpu_service.c)
  line 544 calls `gpu_application_disposal_get()`, defined in
  [apps/gpu-render/main.c](../../apps/gpu-render/main.c).

### Proposed design

The goal requires representative concurrent workloads
([docs/goal.md](../goal.md) line 71). The design follows the existing
accelerator composition: build concrete composed applications for the
required concurrent workloads, and generalize only what those compositions
share.

1. Rename `struct a733_native_usb` to a board provider root (for example
   `struct a733_board_providers`) with USB0 as one member. Make the NPU
   startup accept the borrowed provider view `struct accelerator_providers`
   that already exists, instead of constructing its own providers.
2. Define an integrated session format and a static admission step that
   checks pins, owner identifiers and memory reservations across all
   applications in the image before any of them starts.
3. Only after one concrete two-application consumer exists, replace the
   three hooks with a fixed, image-owned `const` table of application
   descriptors (name, session-extension tag and size, validate, pins, start,
   task-privileged). Iterate the table at the three existing call sites and
   keep the panic-on-any-failure behaviour. A one-entry table reproduces
   today's single-application images.
4. Move the camera application onto the board hooks, as ISP0 already is.
5. Pass the GPU disposal owner through `pvr_gpu_service_config` (a NULL owner
   refuses disposal) instead of the global application hook, and delete the
   uncompiled NULL stub in
   [platform/a733/gpu_disposal.c](../../platform/a733/gpu_disposal.c).

### Migration steps

1. Rename the provider root. This is mechanical.
2. Convert the NPU startup to borrowed providers. Its PMIC and bootstrap task
   becomes a borrower of the board PMIC owner.
3. Rebuild the accelerator composition as a board application under the SMP
   image, keeping the single-core image as a qualification variant.
4. Port the camera application; this changes its wire protocol (72-byte
   header to the streamed response used by ISP0) and its host decoder.
5. Introduce the descriptor table when the first new composed workload lands.

### Requirements to preserve

- **Memory reservation order.** The accelerator image requires the NPU to
  take the first 790 WB pages at `0x41600000` before any GPU allocation
  ([apps/accelerators/main.c](../../apps/accelerators/main.c) lines 12-15). A
  merged image must keep that order and fit the board memory layout.
- **GPU disposal.** Exact cleanup receipts, a stopped backend and a sole
  client stay required before disposal. Mixed-device images also need shared
  pool serialization before GPU reuse is enabled for them.
- **Evidence identity.** Rebuilt images get new identities and need new
  evidence.

### Recognition workload placement

`mimus-perf-6` belongs here because it concerns which cores a workload owns.
The recognizer installs two inference workers on A55 cores 0 and 1,
splits each range at its midpoint, and the engine fixes `kMaxWorkers = 2`.
Cores 2-7, including both A76 cores, stay idle. This limit is documented in
[docs/mimus-smp.md](../mimus-smp.md) lines 157-159.

The proposed change has two steps. First, make the inference core set a fixed
board constant and keep paced capture off the inference cores. Placing work
on the A76 cores first needs their cluster PLL read or brought up and
sustained SMP scheduling qualified on cores 6-7. Second, in the mimus port,
make the worker count a per-target constant (2 on ESP32 targets, up to 8 on
the A733) and partition packed GEMM by output groups. Every two-element
worker-context initializer in the engine becomes an array. The change must
keep the per-worker cache-line ownership rules, the per-output arithmetic
order (for bit-exact parity) and the borrowed-context lifetime that the
current queue pair provides.

`mimus-perf-12` records the matching plan decision. The NPU path supports only
UINT8 graphs with requantized outputs and the GPU compute service runs only
fixed trusted programs, so neither can reproduce the engine's exact int64
linear sums. The proposal is a performance-plan section in
[docs/mimus.md](../mimus.md) with the measured stage budget, the CPU levers
(clock, kernel, A76 placement, worker count, serial overheads) and the reason
accelerator offload is deferred. The section should present the 1.8 GHz and
2.0 GHz operating points as targets, because the CPUs currently run at the
boot loader's 494 MHz. Accelerator evaluation stays on the plan because
[docs/goal.md](../goal.md) requires it.

## 4. Theme: EL3 boot hooks, interrupt routing and IRQ dispatch

Findings: `supervisor-apps-2`, `x-concurrency-5`, `x-concurrency-6`.

### Problem

An application receives a device interrupt only by patching two files. Its
`boot.patch` writes the GIC distributor registers for its interrupt (group,
trigger, priority, route) as raw stores in the EL3 boot code of
[platform/a733-smp-cached/boot.c](../../platform/a733-smp-cached/boot.c). Its
`platform.patch` inserts `if(interrupt==N){handler();return;}` into
`vApplicationIRQHandler` in
[platform/a733-smp-cached/platform.c](../../platform/a733-smp-cached/platform.c).
There are 27 boot patches and 13 platform patches of this kind.

Because behaviour depends on where each line lands, the copies differ:

- Most device handlers run before the core check and the diagnostic-deadline
  exit. The USB2 handler runs after the deadline check and only on core 0.
- The board USB2 route ([apps/board/host_irq_route.h](../../apps/board/host_irq_route.h))
  refuses inherited pending or active state, checks the deadline, programs the
  route while distributor groups are disabled and reads back every register.
  The monitor, ADC, thermal and PWM routes do none of this.
- INTID 70 is claimed by both the monitor and ADC, and INTID 71 by both the
  monitor and thermal. Each image applies only one of these patches, so the
  overlap matters only when images are combined. INTID 95 appears twice in
  the single-core dispatch chain, separated only by build flags.

On the task side (`x-concurrency-6`), about 11 applications re-implement the
same binding state machine: mask in the ISR, set pending, then claim, check
quiescence and re-arm under `taskENTER_CRITICAL`. They use different deferral
bounds (32, 100 or 128) and raw distributor offsets. ADC, thermal, PWM, UART,
monitor, RTC and PCIe-test set an atomic `pending` flag but never notify their
owner task, which wakes on `vTaskDelay(1)` regardless. Ownership of the
distributor is split: the PWM driver re-enables its interrupt through
`GIC+0x108` under the platform register mutex without updating the
application's `masked` flag, while the application's ISR masks it through
`gicv3_spi_mask` without the mutex.

### Evidence

- [apps/monitor/boot.patch](../../apps/monitor/boot.patch) replaces the
  board's `board_identity_prepare` call and writes distributor registers for
  INTIDs 70 and 71 directly.
- [apps/monitor/platform.patch](../../apps/monitor/platform.patch) inserts
  its dispatch before `unsigned c=core();` and the deadline check;
  [apps/board/host_irq_platform.patch](../../apps/board/host_irq_platform.patch)
  inserts `if(interrupt==187 && !c)` after it.
- [drivers/pwm/a733_pwm.c](../../drivers/pwm/a733_pwm.c) lines 244-247 write
  `GIC+0x108`.
- The only shared helper, [drivers/gpu/gicv3_spi.c](../../drivers/gpu/gicv3_spi.c),
  lives under the GPU driver directory.

### Proposed design

1. **One SPI route helper in `boot.c`,** driven by a small per-image `const`
   table `{intid, group, trigger, priority, target, admission predicate}`.
   The helper performs the board USB2 route's checks for every entry:
   pre-route admission (no inherited pending or active state, groups
   disabled, RWP clear, before the startup deadline), read-back of every
   written register and a post-route re-check. Each entry keeps a distinct
   stop code. Extend `gicv3_spi.c` (moved to `platform/a733` or
   `arch/aarch64`) instead of adding a parallel helper.
2. **One dispatch table in `platform.c`,** `{intid, core mask, handler}`,
   supplied by each image and empty by default. `vApplicationIRQHandler`
   (SMP) and `vApplicationFPUSafeIRQHandler` (single-core) look the INTID up
   once, check the core mask, apply one documented deadline policy, and send
   unknown or misrouted interrupts to the existing exit path (exit code 3 for
   the deadline, 4 plus `0x100` for an unknown INTID). A host check rejects
   duplicate INTIDs.
3. **One task-side binding,** `gicv3_spi_binding`, with a fixed INTID, owner
   task and deferral bound. Its ISR entry masks, sets pending, counts and
   notifies the owner with `vTaskNotifyGiveFromISR`. Its task-side claim,
   quiescence check, re-arm and stop run under `taskENTER_CRITICAL`. Device
   drivers stop writing distributor registers; the owner calls the binding.

### Migration steps

1. Add the route helper and convert the monitor, thermal, ADC, PWM and board
   USB2 routes first. Their boot patches shrink to the application-specific
   EL3 work that remains (session preparation, stop codes).
2. Add the dispatch table and convert the same applications.
3. Move each IRQ owner from `vTaskDelay(1)` polling to
   `ulTaskNotifyTake(pdTRUE, timeout)` one application at a time.

### Requirements to preserve

- **Deadline semantics.** Moving the monitor, ADC, thermal, PWM, UART and RTC
  handlers after the common deadline check changes their behaviour: on expiry
  they take the privileged-exit path instead of masking and setting pending.
  Each needs re-checking against its documented contract and new evidence,
  because frozen evidence binds these patch hashes.
- **Ownership.** Route ownership (`gic_route_owned`, checked by `prepare.py`)
  stays per application. Each driver keeps its own address allowlist.
- **Retention.** Each converted application keeps its own fault and
  retention policy; the binding never releases on uncertainty.

## 5. Theme: boot contract, session sealing and memory layout

Findings: `supervisor-apps-3`, `bringup-gaps-11`, `supervisor-apps-6`,
`bringup-gaps-12`.

### Problem

The 224-byte `struct a733_smp_contract` has one six-word `reserved` field with
two incompatible meanings:

- In board images, `reserved[0]` is the session address, `reserved[1]` its
  size and `reserved[2..5]` its SHA-256
  ([apps/board/session.c](../../apps/board/session.c) lines 20-27).
- In images with the 768 MiB extended device memory, the same words are the
  layout identifier, reservation identity and WB/NC bounds
  ([platform/a733/device_memory_layout.c](../../platform/a733/device_memory_layout.c)
  line 66).

An application that needs both forks the EL3 boot. At least seven
applications define a private 288-byte extended record (the base contract plus
a session digest and an entry digest): hevc, displayport, deinterlace, g2d,
h264-encode, h264 full-HD, gpu-display, with gpu-render, dsi-scanout and
storage-read carrying equivalent tails. Their `extended-boot.patch` files are
identical after renaming, and 21 of the 37 `a733_smp_contract_valid`
definitions reuse the board validator by textually including
`../board/admission.c` under a `#define` rename (`supervisor-apps-3`,
`bringup-gaps-11`).

Session sealing is done twice. `board_session_prepare` already copies the
full contract-declared span (up to 4 KiB) into private memory and checks its
digest, but exports only the 592-byte board prefix. About 24 applications then
repeat the copy, digest check and coherence publish from the uploaded page in
their own `snapshot.c`, each with its own EL3 hook (`supervisor-apps-6`).

The recognizer's memory is placed by hand. Model slots at `0x44000000` and
`0x44800000`, the audio slot at `0x45000000` and the 16 MiB arena at
`0x46000000..0x47000000` are literals in C, C++ and Python preparation code in
more than ten files. They are absent from the linker layout and from the
reservation tables. The arena ends where the cold-boot monitor reservation
(`0x47000000..0x47400000`) begins (`bringup-gaps-12`).

### Proposed design

1. **One shared extended record** in `platform/a733-smp-cached` as ordinary
   source: `reserved[0..5]` keep the memory-layout binding, and an explicit
   tail carries `session_bytes`, `session_digest` and `entry_digest`. Give it
   a distinct version or magic: version 3 is already used by the supervised
   SMP contract and the NPU contract under their own magics.
2. **One sealed-session accessor,**
   `const void *board_session_extension(size_t expected_bytes)`, which
   returns the retained copy only when it is ready, its length matches and
   the contract magic matches. Each application keeps a short EL3 check that
   calls the accessor and its typed validator in the same boot slot.
3. **One recognition layout header** (for example
   `include/a733_recognition_layout.h`) included by C and C++ and mirrored by
   the Python preparers. A host check confirms it does not overlap the
   cold-entry reservation, image RAM or the extended device pool. The ranges
   stay out of [platform/a733/memory_reservations.c](../../platform/a733/memory_reservations.c),
   which only constructs the DMA-visible device pool.

### Migration steps

1. Add the shared record and accessor alongside the existing ones.
2. Migrate the extended applications to the shared record, together with
   their launchers, `prepare.py` text patches and host fixtures.
3. Migrate the plain board applications from per-application `snapshot.c` to
   the accessor. The h264, hevc, g2d, deinterlace, h264-encode and
   displayport applications take their digest from the extended record and
   move in the same step.
4. Add the recognition layout header and replace the literal copies, including
   [apps/mimus/service.cc](../../apps/mimus/service.cc),
   [apps/bluetooth/application.c](../../apps/bluetooth/application.c) and
   the `prepare.py` scripts of mimus, i2s and bluetooth.

### Requirements to preserve

- **Fail-closed admission.** Keep the per-application contract and result
  magic, so one application's image cannot admit another's contract. Keep the
  session digest computed over each application's full session type, the
  entry digest bound to the session's qualification field, the exact version
  check and the comparison between the EL3-retained copy and the live
  record.
- **Ordering.** Session rejection stays in the primary EL3 hook, before SMP
  start and provider construction. Moving it to `board_application_start`
  would weaken bounded failure.
- **ABI.** Each application's 288-byte layout is checked by host preparation
  (`hevc_geometry` records `sizeof` of the boot contract), so the shared
  record keeps that size.
- **Evidence identity.** Every migrated image needs new physical evidence.

## 6. Theme: CPU startup and SMP platform variants

Findings: `cpu-startup-4`, `cpu-startup-6`, `cpu-startup-7`, `cpu-startup-8`,
`cpu-startup-9`, `smp-7`, `smp-8`, `build-system-11`, `x-coherence-10`.

### Problem

The EL3-to-nonsecure-EL1 handoff exists in at least three copies, and they
disagree (`cpu-startup-4`):

| Copy | `SCR_EL3` | `MDCR_EL3`, `MDCR_EL2` | `SCTLR_EL2` | Entry admission | Set/way invalidate at entry |
| --- | --- | --- | --- | --- | --- |
| [platform/a733/monitor.S](../../platform/a733/monitor.S) and [monitor.c](../../platform/a733/monitor.c) | `0x505` (RES1 bits 4-5 clear) | not written | `0` | none | yes |
| [apps/storage-read-single-core/monitor.c](../../apps/storage-read-single-core/monitor.c) and [start.S](../../apps/storage-read-single-core/start.S) | `0x505` | not written | `0` | yes | yes |
| [platform/a733/cpu_continue.S](../../platform/a733/cpu_continue.S), [platform/a733-smp-cached/continue.S](../../platform/a733-smp-cached/continue.S) | `0x535` (TF-A) | `MTPME`, `0` | RES1 | yes | no |

The Arm architecture leaves the `MDCR` trap fields UNKNOWN after a warm
reset, which is why TF-A writes them. Leaving them unwritten is a robustness
gap; no trap has been observed. The single-core monitor's GIC save and
configure block is copied almost verbatim into the storage-read single-core
monitor, which the board, GPU, NPU, thermal, RTC and storage single-core
images use.

The SMP image exists in five variants: uncached `a733-smp`, `a733-smp-cached`,
`-cached-devices`, `-observe` and `-supervised` (`smp-7`). `diagnostic.c` is
identical in the first two, and `platform.c` differs by about 40 lines. The
cached build disables the native continuation in
[platform/a733/cpu_continue.c](../../platform/a733/cpu_continue.c) with a patch
that turns `#if defined(__aarch64__)` into `#if 0`, then re-implements the same
MMIO allowlist callbacks in
[platform/a733-smp-cached/continue.c](../../platform/a733-smp-cached/continue.c).
The uncached image never received the physically required nTWE and Secure
PMR corrections; its continuation code still has nTWE clear and writes PMR
only at nonsecure EL1.

Smaller items in the same code:

- **Deadline units** (`cpu-startup-6`). Some CPU-startup APIs take CNTPCT
  ticks and others take microseconds, so every caller divides by 24 by hand
  (`boot_values[1]/24`). Four counter-to-microsecond converters differ:
  `native_now` refuses unless CNTFRQ is 24 MHz, `time_now` assumes it without
  checking.
- **Ineffective check** (`cpu-startup-7`).
  [arch/aarch64/start.S](../../arch/aarch64/start.S) lines 6-8 park a CPU
  when MPIDR Aff0 is non-zero, but A733 cores are numbered in Aff1, so the
  check never parks a secondary. No current path brings a secondary to
  `_start`; the check misleads readers.
- **Dead code and repeated masks** (`cpu-startup-8`). The `.Lreturned` fault
  report in [platform/a733/cpu_entry.S](../../platform/a733/cpu_entry.S)
  lines 89-93 cannot run, because the continuation never returns. The SCTLR
  admission mask is spelled out in more than ten places, although
  `A733_ENTRY_SCTLR_REJECT` exists.
- **Duplicated platform code** (`cpu-startup-9`). `platform_hex`,
  `platform_exception`, tick setup and acknowledge are copied line for line
  between [platform/a733/platform.c](../../platform/a733/platform.c) and
  [platform/qemu-virt/platform.c](../../platform/qemu-virt/platform.c).
- **Counter reads** (`x-coherence-10`). Several `counter()` helpers read
  `CNTPCT_EL0` without an `ISB` and without a memory clobber, while the native
  adapters use `isb; mrs` with a clobber. The practical effect is a timestamp
  a few cycles early.

Configuration order is a latent hazard (`smp-8`, `build-system-11`).
`sunstone_smp_compile` in [cmake/smp-runtime.cmake](../../cmake/smp-runtime.cmake)
adds `platform/qemu-smp`, whose `FreeRTOSConfig.h` sets
`configNUMBER_OF_CORES 2`, to every SMP target. Native eight-core images get
the eight-core configuration only because a later `BEFORE` include of
`platform/a733-smp-cached` wins. A continuation translation unit carries an
`#error` for a core count other than 8, which catches a mismatch in that unit
only.

### Proposed design

1. **One handoff routine** shared by `monitor.S`, the single-core entry and
   the continuations, with TF-A values: `SCR_EL3` RES1 plus NS, FIQ, HCE and
   RW; `MDCR_EL3=MTPME`; `MDCR_EL2=0`; `HSTR_EL2=0`; `VTTBR_EL2=0`;
   `SCTLR_EL2` RES1; and `CPTR`, `CNTHCTL`, `CNTVOFF` and `ICC_SRE` at every
   level.
2. **One entry admission macro** (`CurrentEL`, MPIDR core 0, `SCTLR_EL3`
   M/C/I/EE clear, inherited timers stopped) and assembler macros
   `admit_sctlr reg, tmp` and `admit_mpidr reg, tmp` built on
   `A733_ENTRY_SCTLR_REJECT`. Give `monitor_entry` the same admission, timer
   stop first. Make the set/way invalidate at entry an explicit, documented
   per-profile choice.
3. **One GIC monitor module** for save, configure and restore, with
   per-application hooks.
4. **One SMP base:** `a733-smp-cached`. Move the probe barrier, SGI value,
   tick, IRQ dispatch and exit into one shared source. Move the native
   continuation adapters into a separately linked `platform/a733` file that
   every profile links, and delete the `#if 0` patch.
5. **One deadline unit.** Use counter ticks everywhere, or distinct
   `struct a733_ticks` and `struct a733_us` types. Keep a single time adapter
   that checks the counter frequency.
6. **Explicit platform configuration.** Give `sunstone_smp_compile` platform
   and port-build arguments so hardware targets never see QEMU directories,
   move `runtime_current_reent` out of `platform/qemu-smp`, and put a
   core-count `#error` in a header every A733 SMP translation unit includes.
7. **Shared helpers** for counter reads (`isb`, `mrs`, memory clobber; usable
   from C and C++) and for the safe subset of platform code (`platform_hex`,
   `platform_exception`, `platform_panic`, tick setup).
8. **Smaller fixes.** Change the `start.S` check to `tst x0, #0xffff`, which
   parks A733 secondaries and still works on QEMU. Remove `.Lreturned`.

### Migration steps

1. Add the shared handoff and admission macros; switch each entry path one at
   a time and diff the system-register write traces from the existing
   instruction checks. Only the intended `SCR`, `MDCR` and `SCTLR_EL2`
   values may change.
2. Decide the uncached image's status. Retiring `a733-smp` drops the
   cache-disabled functional diagnostic described in
   [docs/a733-smp.md](../a733-smp.md); keeping it means porting the PMR and
   nTWE corrections. Retire `a733-smp-observe` if the supervised variant
   covers USB observation.
3. Express supervised and observer differences as compile-time options or
   small hook functions. Tag the commits of frozen images so their evidenced
   ELFs stay reproducible after the hash-pinned patches go.
4. Fix the include configuration and add the core-count check.

### Requirements to preserve

- **Admission before memory access.** Entry stubs may not touch memory or the
  stack before admission, so the macros take a scratch register argument.
- **Context checks.** Keep the watchdog's stricter context check (EL1, IRQs
  not already masked, exact owner task) when sharing it.
- **Per-image checks.** Each image keeps its own linker-range checks and
  record publication; cached and uncached use different record types.
- **IRQ dispatch.** `vApplicationFPUSafeIRQHandler` stays per platform until
  the patch-based dispatch is replaced (section 4), because single-core
  application patches edit it.

## 7. Theme: shared runtime helpers

Findings: `clock-power-pins-12`, `security-misc-10`, `buses-pcie-9`,
`x-concurrency-8`, `x-duplication-6`, `x-bounds-4`, `x-duplication-9`,
`bluetooth-10`, `radio-net-13`, `gpu-6`.

Several small mechanisms are copied across most drivers. Each is easy to
centralize and each copy depends on details that change.

### 7.1 Task-context admission check

**Problem.** About 15 adapters repeat the same block: read `CurrentEL` and
`DAIF`, mask IRQs, read the per-core `ullPortInterruptNestings[]` and
`ullCriticalNestings[]` through `tpidr_el1`, restore `DAIF`, then check the
scheduler state and the owner task. Each copy re-declares the FreeRTOS port's
private symbols. The copies are in the SPI, UART, MMC, radio, ADC, PWM, PCIe,
PMIC, TWI, camera and USB2 adapters, in
[platform/a733/hdmi_native.c](../../platform/a733/hdmi_native.c),
[typec_native.c](../../platform/a733/typec_native.c),
[maintenance_watchdog.c](../../platform/a733/maintenance_watchdog.c) and
[shared_locks.c](../../platform/a733/shared_locks.c). The last one omits the
EL1 check and accepts any task (`clock-power-pins-12`, `security-misc-10`,
`buses-pcie-9`, `x-concurrency-8`, `x-duplication-6`). A change to the port,
such as how `tpidr_el1` holds the core index, needs 15 synchronized edits.

**Proposed design.** Export one predicate from `shared_locks.c`:
`int a733_task_context(TaskHandle_t owner)`, where NULL means any task. It
includes the EL1 check, the rejection of IRQs already masked on entry, the
nesting and critical checks, and the optional owner match. Add
`a733_platform_lock_held(locks, lock, owner)`, which tests
`retained & (1u << lock)` and `mutex[lock]` for the given lock index; the TWI
and PMIC adapters use the I2C locks and PWM uses its own owner field. Each
adapter keeps its `initialized` and instance checks and its address
allowlist.

**Preserve.** The order of the EL1 and masked-IRQ rejections before
`daifset`, the out-of-range-core fallback, and the model hooks used by host
checks (`SUNSTONE_CAMERA_MODEL` replaces the check with a stub). Adding the
EL1 check to the shared-lock path is harmless.

### 7.2 Address-range and deadline helpers

**Problem.** Caller-buffer validation uses file-local helpers named
`separate`, `overlap`, `disjoint`, `span`, `outside`, `apart` and
`independent`, about 190 definitions with at least six meanings (`x-bounds-4`,
`x-duplication-9`). Some reject NULL and wraparound; some treat zero length as
non-overlapping; the camera and Bluetooth "overlap" forms treat wraparound as
overlap and are used as fail-closed rejections. About 20 compute
`a + n <= b` without a wrap check and rely on an earlier `span()` call. The
Bluetooth stack alone has about 15 (`bluetooth-10`) and the AIC8800 radio
drivers 11 (`radio-net-13`). No exploitable case was found; every unguarded
use is preceded by a span check.

Separately, 27 drivers define their own `timely()` (sample the clock, require
it not earlier than the last sample and before the deadline, store it). In the
GPU stack (`gpu-6`), about ten components each keep `last_now` and compare it
against each other's private `last_now` fields, which is why their state
structures must be visible across layers.

**Proposed design.** Add `include/a733_range.h` with two explicitly named
predicates that keep the two current semantics: `a733_disjoint()` (NULL- and
wrap-rejecting) and `a733_overlaps()` (wrap counts as overlap), plus
`a733_range_valid()` and `a733_range_within()`. Add `struct a733_deadline`
and `a733_deadline_sample()`. In the GPU worker, replace the cross-component
`last_now` comparisons with one worker-owned clock that components call to
resample.

**Migration.** Convert files as they are next edited, module by module, with a
host unit test of the header covering NULL, zero length, adjacent ranges and
ranges crossing `UINTPTR_MAX`.

**Preserve.** Each call site's zero-length and NULL behaviour stays explicit.
Callers that rely on fail-closed overlap keep it. The GPU components keep
resampling the clock after each I/O step so late completion is still
detected; one sample per worker step is not enough.

## 8. Theme: memory mapping, cache maintenance and MMIO access

Findings: `x-coherence-4`, `gpu-7`, `runtime-memory-6`, `x-coherence-3`,
`x-coherence-6`, `x-coherence-7`, `x-coherence-2`.

### Problem

All cached DMA consumers already use one cache helper,
`aarch64_device_cache` in [arch/aarch64/device_memory.c](../../arch/aarch64/device_memory.c).
The checks around it are copied:

- **Mapping checks.** About 43 static functions in 68 files walk 4 KiB pages,
  translate each with `AT S1E1R/W`, and compare the physical address, MAIR
  index and shareability (`x-coherence-4`). Nine are in the GPU stack
  (`gpu-7`) and three implement the reservation checks (`runtime-memory-6`).
  The rules differ: some require read and write translations to match and
  some check one; some accept NC with shareability 2 or 3, while the HDMI
  audio adapter requires 3. The HDMI audio rule is likely to refuse
  architecturally valid NC pages.
- **Crypto engine cache maintenance.**
  [drivers/crypto/a733_ce_native.c](../../drivers/crypto/a733_ce_native.c)
  lines 90-120 open-code `DC CVAC/CIVAC/IVAC` loops and skip the shared
  helper's checks of `CTR.CWG`, `CurrentEL` and `SCTLR` M/C
  (`x-coherence-3`). On the A733 every core reports a 64-byte writeback
  granule, so this is a consistency fix.
- **MMIO accessors.** About 60 files define their own
  `dsb sy; store; dsb sy` accessors (`x-coherence-6`). Their barrier policy
  differs: one DMA read has no leading barrier, and
  [platform/a733/boot.h](../../platform/a733/boot.h) has no barrier and no
  memory clobber.
- **Parallel types.** `enum a733_dma_cache`, `enum a733_npu_cache_op` and
  `enum aarch64_cache_op` define the same three operations; three structures
  carry the same translation fields; DMA and NPU adapters convert between
  them (`x-coherence-7`).
- **NC DMA policy.** `dma_memory_accept` admits only WB memory, so the only NC
  DMA consumer (HDMI audio) carries a private admission and cache callback
  (`x-coherence-2`). This is a documented decision in
  [docs/hdmi-audio.md](../hdmi-audio.md).

### Proposed design

1. **One mapping helper** next to `aarch64_device_translate`, for example
   `aarch64_device_mapped(cpu, physical or page list, bytes, kind, access)`
   where `kind` is WB, NC or Device and `access` selects read, write or both.
   NPU callers pass a translate callback, because the NPU memory code
   translates through injected operations for host models. Migrate the
   Normal-memory checks first; keep the Device-memory and WB-only checks in
   [platform/a733/gpu_backend.c](../../platform/a733/gpu_backend.c) and
   [gpu_reserved.c](../../platform/a733/gpu_reserved.c) as distinct kinds.
   Fix the HDMI audio shareability rule to accept 2 or 3, matching
   [platform/a733/memory_uncached.c](../../platform/a733/memory_uncached.c).
2. **Crypto engine** calls `aarch64_device_cache` per field, or a batched
   multi-range variant that keeps one `DSB` pair.
3. **One MMIO header** (`include/a733_mmio.h`) with today's `DSB SY`
   semantics, adopted mechanically. Any barrier weakening (for example
   `DMB OSHST` before a plain control write) is a separate change that needs
   board qualification and updates to the documented "full-system device
   barrier" contracts.
4. **One cache-op enum and translation structure.** DMA and NPU operation
   tables take `enum aarch64_cache_op` and `struct aarch64_translation`. The
   adapter callbacks stay, because they enforce adapter policy (initialized,
   not failed, fault on refusal); only the enum switches go.
5. **NC windows in the DMA policy** when a second NC DMA consumer exists: a
   memory type per `struct dma_memory_window`, NC windows limited to their
   exact ranges and roles, and only CLEAN (implemented as `DSB`) accepted on
   them.

### Requirements to preserve

- **Coherence.** Barrier sequences stay identical in the consolidation step;
  compare disassembly before and after. NC memory never receives set/way or
  invalidate maintenance.
- **Per-caller policy.** Exact extents, roles and read/write choices remain
  parameters; the helper never merges them.

## 9. Theme: device-memory ownership

Findings: `runtime-memory-5`, `runtime-memory-7`, `runtime-memory-8`,
`dma-iommu-5`.

### Problem

- **Pool locking** (`runtime-memory-5`). The pool header
  ([include/device_memory_pool.h](../../include/device_memory_pool.h) lines
  19-20) requires one service lock around every operation, but the lock is
  supplied by callers. In the default profile, the singleton getters
  initialize lazily without synchronization. Only the DMA pool session, GPU
  reservation and NPU reservation refuse acquisition after the scheduler
  starts. No concurrent caller pair was found.
- **Ownership transitions** (`runtime-memory-7`). About 35 sites flip a lease
  from CPU to DEVICE by writing `device_memory_lookup(pool, lease)->state`
  directly. The header's rule that only a controller returns DEVICE to CPU,
  and that retained memory is permanent, is enforced by convention.
- **memmove placement** (`runtime-memory-8`). `memmove` lives in
  [runtime/memmove.c](../../runtime/memmove.c) apart from the other
  routines in [runtime/memory.c](../../runtime/memory.c). Several component
  records describe link failures from omitting it.
- **A second DMA ownership mechanism** (`dma-iommu-5`).
  [platform/a733/dma_reserved.c](../../platform/a733/dma_reserved.c) leases
  memory per allocation over one all-roles window, but only a host check uses
  it, and `a733_memory_dma_init` has no caller.
  [docs/memory-reservations.md](../memory-reservations.md) presents it as the
  way to use DMA.

### Proposed design

1. Initialize both singleton pools once from the platform boot path before
   the scheduler starts, publish readiness with a release store, and make the
   getters acquire-load reads. Enforce pre-scheduler acquisition and document
   that runtime release and retain act only on the caller's own slot. Prefer
   this to a pool-owned mutex, which would add a second locking mechanism.
2. Add `device_memory_mark_device(pool, lease)` (CPU to DEVICE only) and
   `device_memory_complete()` (DEVICE to CPU, for controller completion
   paths), and make `device_memory_lookup` return a `const` record.
3. Merge `memmove` into `runtime/memory.c` and remove every explicit
   `memmove.c` entry in the same change, including the indirect one in
   [cmake/radio-network.cmake](../../cmake/radio-network.cmake).
4. Remove `dma_reserved.[ch]` and `a733_memory_dma_init`, keep the allocator
   and pool-policy parts of the host check, and point the reservation
   documentation at the pool-session and service path.

### Requirements to preserve

- **Runtime release and retain.** GPU disposal and crypto-engine fault
  retention depend on them, so they stay legal after scheduler start.
- **Retention on failure.** A failed later step after `mark_device` still
  retains the lease.

## 10. Theme: the USB0 evidence transport

Findings: `usb-2`, `supervisor-apps-5`, `x-bounds-3`, `x-duplication-7`,
`usb-10`.

### Problem

USB0 is the board's only link to the host and carries all evidence. Each
application re-implements the TinyUSB device glue: device, qualifier and
configuration descriptors, the string table, the TinyUSB OS hooks, the
`a733_usb_prepare` and poll hooks, transfer accounting in
`tud_vendor_tx_cb` and the owner task loop. Thirty-two source files define
`tud_descriptor_device_cb`; about 25 are close copies, and
[apps/adc/usb0.c](../../apps/adc/usb0.c) and
[apps/spi/usb0.c](../../apps/spi/usb0.c) differ only in names, magic and the
product string (`usb-2`, `x-bounds-3`). About 21 `usb0.c` files also build the
same result frame (magic, version, result, epoch, generation, session digest,
payload digest), and `apps/pcie/snapshot.c` and `apps/pcie-test/snapshot.c`
are identical (`x-duplication-7`).

The copies have diverged:

- **Stop policy** (`supervisor-apps-5`). In the board image, `stop()` parks
  USB0 and other services continue. The monitor calls `board_fail(NULL)`.
  mimus-board generates a renamed copy of the board file and rewrites `stop()`
  to also revoke Type-C.
- **Loop order.** `drivers/usb/diagnostic.c` polls before `tud_task`; the
  applications call `tud_task` before `dcd_int_handler`, which adds one tick
  per event.
- **Completion checks.** PCIe-test rechecks output readiness in the
  completion callback; the others check only before each write.

The native applications also reuse `a733_usb_prepare`, the TinyUSB
hardware-reset hook, as a blocking provider-health spin called on every loop
iteration. Three device `tusb_config.h` variants differ in FIFO sizes and
`CFG_TUD_TASK_EVENTS_PER_RUN`.

A dead generator expression on the USB2 host object library in
[cmake/a733-board.cmake](../../cmake/a733-board.cmake) line 96 evaluates the
library's own name and is always false, which suggests per-image host objects
that do not exist (`usb-10`).

### Proposed design

Add one fixed-configuration service, `drivers/usb/usb0_service.c` (or
`apps/board/result_usb.c`). It takes a `const` descriptor:

- PID, product and serial strings, result magic;
- `live()` and `fail()` callbacks supplied by the application;
- `exchange()` (request bytes to response frame) and an optional loop-exit
  predicate for bounded-run images such as usb-returning;
- an optional failure hook, for example the crypto application's upload
  scrub.

The service owns the descriptors, hooks, the result-frame header builder,
incremental write-and-flush with sent and transferred accounting, and the
owner loop. Give the native health check its own name and leave the DCD
reset hook with correct reset or no-op behaviour. Collapse the
`tusb_config.h` variants into one header with per-target overrides. Add
`board_session_snapshot(contract, magic, dst, bytes, valid)` for the
remaining snapshot copies, and have PCIe-test compile the PCIe snapshot.
Delete the always-false generator term at line 96 and keep line 164, which
selects USB3 and dual-USB2 per target.

### Migration steps

1. Port board, monitor and mimus-board first, deleting the generated
   `mimus-usb-base.c`.
2. Port the simple one-request result applications (ADC, PWM, thermal, UART,
   SPI, PCIe, PCIe-test).
3. Leave protocol-specific servers (Bluetooth, mimus-board's streaming path,
   gpu-render, storage-read) on custom exchange handlers.
4. Once the loop is shared, apply the throughput change (servicing more than
   one 512-byte packet per tick) in one place.

### Requirements to preserve

- **Bounded writes.** Responses can exceed `CFG_TUD_VENDOR_TX_BUFSIZE`
  (1024 bytes on the observe target, while board responses reach 1,160
  bytes), so the service keeps incremental writes.
- **Per-application failure domains.** The board keeps Type-C and GPIO
  shutdown running after a USB0 stop; other applications fail their own
  service. The service never releases resources itself.
- **Wire compatibility.** Wire bytes, PIDs (`0xefe8` supervised, otherwise
  `0xefe5` or `0xefea`) and host decoders stay unchanged.
- **Evidence identity.** Accepted images are migrated only in future builds;
  each migrated consumer is a new evidence scope.

## 11. Theme: shared provider authorities

Findings: `clock-power-pins-9`, `clock-power-pins-10`, `clock-power-pins-13`,
`security-misc-6`, `security-misc-7`.

### Problem

Several hardware resources have more than one authority or more than one
protocol engine:

- **I2C engines** (`clock-power-pins-9`). The Allwinner TWI protocol is
  implemented three times: a blocking reader in
  [drivers/i2c/a733_i2c.c](../../drivers/i2c/a733_i2c.c), the PMIC
  provider's phase machine in
  [drivers/power/a733_pmic.c](../../drivers/power/a733_pmic.c) and the
  generic engine in [drivers/i2c/a733_twi.c](../../drivers/i2c/a733_twi.c).
  The NPU startup and GPU native code carry their own six-register PMIC
  snapshots. The copies differ: the blocking reader issues STOP after an
  unexpected status, the others retain without STOP.
- **Rail authority** (`clock-power-pins-10`). DCDC2 is held as clock-registry
  scope bit 18 by the media clock, G2D, deinterlace and NPU startup. DCDC4
  belongs to the GPU resource manager. The 39-output rail ledger bridges into
  both with private guards and nested locks, which accounts for most of its
  complexity (the OPENING state and bridge health).
- **Pin API** (`clock-power-pins-13`). A legacy PB4-PB8 pin API with its own
  mux table remains for one consumer, the I2S platform, alongside the
  bank-qualified API that covers the same pins. The legacy path skips the
  UART-route conflict check.
- **PERI0 clock check** (`security-misc-6`). At least 14 drivers validate the
  PERI0 parent clock themselves. The crypto engine, PCIe, radio, video, USB2,
  PWM and DMA refuse PERI0 when reserved bit 24 is set; thermal, GPADC and
  LRADC accept it, which matches [docs/clock-leases.md](../clock-leases.md).
  Thermal, GPADC and LRADC also carry copy-pasted controller scaffolding
  (timely, read, update, claim, lock, fault). Their `lock()` variants differ:
  ADC and LRADC skip retention after an uncertain mutex entry.
- **Watchdog programming** (`security-misc-7`). The WDT0 arming sequence
  (key, restart word, 4-second mode `0x41`, ROM redirect and FEL flag) is
  hand-coded in four launchers (`a733-smp`, `a733-smp-observe`,
  `a733-storage-launch`, `a733-device-launch`) and re-checked with literals
  in the maintenance watchdog. A separate driver,
  [drivers/watchdog/a733_watchdog.c](../../drivers/watchdog/a733_watchdog.c),
  is used only by the expiry probe and its check.

### Proposed design

1. **One TWI engine.** Add an `A733_TWI_R0` controller to `a733_twi` with
   one-byte register addresses and an inherited-divider mode, plus a mode in
   which the caller holds the I2C mutex across several requests. Rebuild the
   PMIC snapshot as six engine requests under one held mutex, keeping the
   baseline checks and the per-rail checks of the NPU and GPU snapshots.
   Delete `a733_i2c.c` after migrating its other consumers (power and UFS
   probe checks, single-core and storage-maintenance targets).
2. **One rail authority.** Move DCDC2 consumers to rail-ledger tokens and the
   GPU platform to a ledger DCDC4 token. Move the GPU manager's conflict
   checks for DCDC4 change requests into ledger exclusive tokens before
   deleting GPU guard slots 16-31. The OPENING state then collapses to a
   synchronous acquire.
3. **One pin API.** Port the I2S platform to the banked request and lease
   types, then delete the PB-only functions (about 150-170 lines; helpers
   shared with the banked path stay).
4. **One PERI0 validator** beside `a733_clock_ref24_valid` in
   [include/a733_clock_reference.h](../../include/a733_clock_reference.h),
   taking each caller's gate, status and auto-gating masks as parameters,
   with bit 24 permitted as documented unless the manual says otherwise. One
   small internal header for the thermal, GPADC and LRADC controller core,
   using the thermal `lock()` behaviour.
5. **One WDT0 header** (`include/a733_wdt0.h`) with offsets, key, restart
   word, mode encoding and redirect values, used by every launcher and by the
   maintenance watchdog's reload check.

### Requirements to preserve

- **Admission order.** After the DCDC2 move, consumers acquire the clock
  registry and then the rail ledger in a fixed order, with retention across
  both tokens on uncertain failure.
- **GPU voltage-change exclusion** survives the DCDC4 move.
- **Test coverage.** Deadline and RECOVERY coverage now provided by the
  legacy pin checks moves to banked checks before those checks are deleted.
- **Watchdog behaviour.** The launchers keep their exact-value arm and verify
  and their non-disarming ownership. The driver does not program the ROM
  redirect and keeps disarm state, so it cannot become the shared helper.

## 12. Theme: device subsystem consolidation

These findings each concern one subsystem. Most remove a duplicated mechanism;
a few correct a design choice inside the subsystem.

### 12.1 DMA and IOMMU

- **Service coupled to I2S** (`dma-iommu-3`, medium).
  [include/dma_service.h](../../include/dma_service.h) includes the I2S
  headers, and each of 16 channels embeds an I2S capture and an I2S stream,
  including a 4 KiB snapshot buffer. Only I2S0 uses streams, on at most two
  channels. The finite capture kind `DMA_SERVICE_I2S` is used only by host
  checks. Proposal: delete the capture kind and `dma_service_i2s.[ch]`
  (keeping `a733_i2s_plan` and the shared format and operation types), and
  replace the per-channel members with a fixed array of statically allocated
  stream contexts referenced from the job. Stream contexts keep stable
  addresses and are not reused while `platform_held` or `peer` is set.
  Port `checks/i2s.c`, `checks/i2s-platform.c` and the capture case of
  `checks/dma-service.c` first. This reclaims more than 60 KiB per service
  instance.
- **Three hand-built consumers** (`dma-iommu-4`). The pool session, the UART
  application and HDMI audio each build startup, memory, adapter and service
  setup with their own protected-range literals. Proposal: generalize
  [platform/a733/dma_pool_session.c](../../platform/a733/dma_pool_session.c)
  into a per-instance constructor that takes a `const` profile (jobs, FIFO
  ports, memory type, parent pool, supervision mode) with optional bootstrap
  task creation, and derive protected ranges from linker symbols. Keep each
  application's ownership checks (HDMI's EL and session authority, UART's
  `uart_live` gating).
- **Duplicated register maps and predicates** (`dma-iommu-7`). Channel
  offsets are defined three ways and the mapping-shape test three times. Add
  one private register header; each consumer keeps its own subset (the
  `quiet()` list omits DESCRIPTOR and PACKAGES on purpose). Remove the unread
  `wrote` field or wire it into a fault report.
- **IOMMU profiles** (`dma-iommu-9`). Each profile wrapper calls the common
  init and then overwrites its masks, and `admitted()` repeats the mask list.
  Proposal: a `const` profile table that drives the existing public
  `translated_masters` and `accessible_masters` fields, with a per-row
  `supervised_allowed` and an IOVA limit that stays 4 GiB for every profile.

### 12.2 Audio

- **Cyclic RX guard band** (`audio-1`, medium). The checked copy of RX period
  *k* starts as soon as the completed-package count reaches *k*+1 and
  continues while it is below *k*+*N*. Both ends depend on undocumented
  device ordering, which is why cyclic RX is not physically admitted
  ([docs/i2s-stream.md](../i2s-stream.md)). Proposal: require
  *k*+2 ≤ count ≤ *k*+*N*−2 before and after the copy in
  `a733_dma_cyclic_rx_copy`, and mirror both bounds in the stream's
  `cyclic_active` (copy at `count >= raw_next+2`, overrun stop at
  `count > raw_next+count-2`). Give cyclic RX its own period count (6 to 8)
  instead of raising `I2S_PERIODS`, which also sets PCM sizes and launch
  geometry. Restate admission as a measured margin of less than one period
  and measure it on the board.
- **HDMI audio reimplements I2S** (`audio-5`, medium).
  [apps/hdmi-audio/native.c](../../apps/hdmi-audio/native.c) programs I2S3
  itself, clears the interrupt status once and never reads it again, and
  sets TXEN before DRQ. Interim fix: read ISTA at the final stage and fail on
  underrun or overrun, and prime the FIFO or enable DRQ before TXEN.
  Long-term: give the stream driver a controller descriptor (base, FIFO
  offsets, DRQ, FIFO depth) and make DMA ownership and resource acquisition
  per profile, because HDMI audio uses `dma_service` while the stream driver
  claims a channel directly.
- **Layering** (`audio-7`). The stream driver reads DMA channel register
  `0x138` and the channel's `transfer`, `started` and `timeout` fields.
  Proposal: a read-only DMA query reporting queue-end, error and timeout that
  also applies the controller's channel sanity checks.
- **Legacy capture path** (`audio-11`). The one-shot capture
  (`a733_i2s_capture`) duplicates the stream's register sequence with a
  different FIFO trigger policy and has no application user. It goes with the
  `dma-iommu-3` change; [docs/i2s.md](../i2s.md) and
  [docs/dma-service.md](../dma-service.md) are rewritten with it.

### 12.3 Display

- **Power-up copies** (`display-4`). The power-domain owner
  [drivers/power/a733_power_domains.c](../../drivers/power/a733_power_domains.c)
  is the only writer of PCK600 domains 1 and 5: the framebuffer, G2D and the
  GPU request them through rows ([docs/power-domains.md](../power-domains.md)).
  HDMI and DisplayPort still carry inline copies that power the VO1 domain
  (domain 10), the video platform powers domains 2 and 3, and the NPU
  platform powers domain 4 until it moves onto its row. Proposal: give
  the owner entries for those domains and move their writers onto rows.
- **Framebuffer pipelines** (`display-5`, medium).
  [apps/hdmi/framebuffer.c](../../apps/hdmi/framebuffer.c),
  [apps/displayport/memory.c](../../apps/displayport/memory.c) and
  [apps/gpu-display/display.c](../../apps/gpu-display/display.c) share lock
  wrappers, media-clock setup, IOMMU callbacks, DE configuration and the
  prepare/stop state machine; HDMI and DisplayPort differ in about 140 lines.
  Proposal: one retained-framebuffer provider (DE1, IOMMU master 8, media
  clock, replay bank) parameterized by output and an output-state predicate.
  Replace HDMI's `0x41c00000` literal with gpu-display's pool-relative check.
- **Stale contract and dead code** (`display-10`): see section 15.

### 12.4 Camera

- `camera-7` is covered in section 3.
- **Register-program interpreters** (`camera-8`). Each camera leaf has its own
  step-table interpreter and identical tick, overlap, begin and ADMIT code;
  the VIPP unity parameters and enable sequence are written in both the route
  and ISP0 drivers. Proposal, in two steps: first share the helpers,
  `a733_vipp_unity_parameters()` and one VIPP step table with its
  para-ready mask rule; then unify the interpreter only if each leaf keeps
  its per-poll MMIO granularity and fault semantics (ISP0 currently reads,
  writes and verifies in one poll). Register images stay byte-identical.

### 12.5 Video

- **Two VE platform cores** (`video-6`, medium).
  [drivers/video/a733_video_platform.c](../../drivers/video/a733_video_platform.c)
  and [a733_video_encode_platform.c](../../drivers/video/a733_video_encode_platform.c)
  duplicate clock tables, locking, register access and the parent and
  baseline checks. The decoder kept an obsolete 24 MHz reference check that
  refuses the documented 26 MHz board in the model, and its callbacks are
  typed on the JPEG job, so H.264 enters through NULL-job shims. Proposal:
  one `a733_ve_platform` with a `const struct a733_ve_unit` descriptor. The
  descriptor must cover the inverted PLL_VE polarity, the byte-write ABI,
  the identity registers and accepted versions, and decoder-only cold-start
  and drain hooks. Admit and memory callbacks take `const void *job`. Fix
  the decoder's missing device-memory translation check in the same change.
- **Per-application scaffolding** (`video-7`). JPEG, JPEG-encode, H.264,
  H.264-encode and HEVC each carry adapters, a resource-acquisition ladder,
  session validators, snapshot, launcher, USB0, `observe.py`, `prepare.py`
  and CMake. Proposal: `apps/video-common/` with a provider adapter, a
  `video_resources_step()` ladder, a session-prefix validator with codec
  hooks, and one CMake function. The shared launcher keeps two contract
  variants (base and extended). The rail, PMIC and IOMMU-gate predicate is
  also used by ISP0, G2D and deinterlace and may belong with the board
  session code.

### 12.6 GPU

- `gpu-5` is covered in section 3, `gpu-6` in section 7 and `gpu-7` in
  section 8.
- **Unused translation flush** (`gpu-9`).
  [drivers/gpu/pvr_gpu_flush.c](../../drivers/gpu/pvr_gpu_flush.c) builds the
  MMU cache-invalidation command but nothing calls it and no build target
  compiles its check. Live mapping changes are listed as required work in
  [docs/gpu.md](../gpu.md). Proposal: keep it, add its check and reference
  comparison to the build so it cannot drift, and integrate it in the same
  change that adds live mapping changes.

### 12.7 NPU

- **Completion by polling** (`npu-4`). SUBMIT replies RUNNING at once; the
  client loops `vTaskDelay(1)` with STATUS until DONE. Proposal: a WAIT
  operation whose held reply sits in its own owner-only slot, checked after
  each poll or interrupt pass. It must not reuse the existing deferred-message
  slot, which would block other clients' STATUS requests. No NPU path is on
  the recognition path today, so this is low priority.
- **Layout per `#ifdef` family** (`npu-5`, medium). Buffer sizes, device
  addresses, CPU offsets and roles are written by hand in
  [platform/a733/npu_reserved.c](../../platform/a733/npu_reserved.c),
  [drivers/npu/npu_convolution.c](../../drivers/npu/npu_convolution.c) and two
  headers for each model family. Proposal now: define each family's buffer
  table once and derive `A733_NPU_RESERVED_BYTES` from it with a
  `_Static_assert`. Plan-derived layouts wait for a concrete multi-family
  consumer, because they would invalidate the recorded layout and page-table
  evidence and grow every default image by a 1 MiB model copy.
- **Two assemblers** (`npu-6`).
  [drivers/npu/nbg_sh_nt.c](../../drivers/npu/nbg_sh_nt.c) re-declares the
  prefix, footer, INIT template and byte helpers of
  [nbg_assembly.c](../../drivers/npu/nbg_assembly.c). Share those now. A
  record-driven NN/SH/NT assembler waits for a real consumer model. Each
  patch stays validated before buffers are written.
- **Patched bootstrap** (`npu-10`). Every board consumer memmoves the
  controller's 192-byte bootstrap output to insert two SRAM states and an
  END pair. Proposal: emit the 208-byte sequence from `a733_npu_bootstrap`,
  delete the patching and the unused macro, and update the model checks that
  emit the 192-byte form.

### 12.8 Storage

- **SMHC duplication** (`storage-8`).
  [drivers/storage/sunxi_mmc.c](../../drivers/storage/sunxi_mmc.c) and
  [sunxi_sdio.c](../../drivers/storage/sunxi_sdio.c) implement the same
  controller reset, clock update and FIFO loop with different terminal error
  masks. The write filters re-encode command flags as raw numbers. Proposal:
  `include/sunxi_smhc.h` with register offsets, flag macros, a documented
  terminal mask and a slot helper (index, base, module). Build the expected
  CMD24 and CMD6/ACMD51 words in the filters from the same macros. Any change
  to the mask itself is a separate decision with evidence, because an extra
  terminal bit fails closed.

### 12.9 Radio

- **Entropy refusal is terminal** (`radio-net-11`, medium). The network's
  random-number hook takes the entropy reservoir with a non-blocking lock and
  faults the network permanently on contention or shortage. On the SMP port
  the IP task can run while the owner publishes a refill. This behaviour is
  documented in [docs/radio-entropy.md](../radio-entropy.md); it is a design
  weakness. Proposal: a short bounded retry on the lock (the critical section
  is a 256-byte copy) and a non-terminal `pdFALSE` from
  `xApplicationGetRandomNumber` on shortage. Keep a small reserve for the ISN
  hook, which has no failure result and still closes output on shortage.
  Fault terminally only on crypto-engine or clock health failure. The
  supplicant is a second consumer, so a single-producer single-consumer ring
  does not fit.
- **Transport structure** (`radio-net-12`, medium). The SDIO transport holds
  pointers to every worker and decides TX source, sent dispatch, response
  routing and EAPOL policy itself. Record framing is decoded in four places.
  Two receive modes exist. Proposal: one framing helper
  `aic8800_sdio_next_record()`; a fixed-priority TX slot array that keeps
  today's order and gating (runtime, Wi-Fi, security, data; WIFI_WAITING
  blocks; data credit rule `value >= 3`; security buffers zeroed after send);
  attach-time validation of the object graph while keeping per-call argument
  checks. Delete the legacy receive mode only after owned receive covers
  setup, identity and firmware.
- **Header encoders** (`radio-net-13`). Four workers hand-encode the command
  header with CRC-8; the bytes agree today. Export the codec's encoder for
  firmware, runtime, Wi-Fi and security, and only the CRC function for the
  data path.

### 12.10 Bluetooth

- `bluetooth-10` is covered in section 7.2. The lower-stack disjointness walk
  in the SDP server cannot move to init, because the L2CAP channel is bound
  later; run it once when L2CAP becomes ACTIVE, with a latch cleared on
  re-initialization.

### 12.11 Crypto engine

- **Secrets left in the DMA page** (`security-misc-8`). After a successful
  task, only the key field is zeroed. The last plaintext input, decrypted
  output, digest and SHA continuation state stay in the permanent DMA page.
  Proposal: scrub `input`, `cipher_output`, `output` and `iv` at the
  consumed-task boundary, with a sync mode that cleans those lines and the
  same bounded-failure handling as key clearing; never scrub after uncertain
  failure. Update the header contracts and
  [docs/crypto-hash.md](../crypto-hash.md).
- **Duplicate constructor** (`security-misc-9`).
  `a733_ce_entropy_native_init` repeats `a733_ce_session_native_init`. Share
  one static helper that takes the bind step as a parameter, run every entropy
  precondition first, and update the disassembly checks that assert symbols
  inside each constructor.

### 12.12 Buses and PCIe

- `buses-pcie-9` is covered in section 7.1.
- `buses-pcie-10` is covered in section 13.
- `buses-pcie-11` and `buses-pcie-13` are clean-up items in section 15.

## 13. Theme: build structure

Findings: `build-system-8`, `build-system-13`, `build-system-14`,
`supervisor-apps-8`, `x-duplication-4`, `x-duplication-5`, `buses-pcie-10`,
`x-bounds-1`.

### Problem

- **Copied recipes** (`x-duplication-5`, medium). About 20 board-derived
  recipes repeat the same steps: include the board recipe, copy its sources,
  remove a few, patch `boot.c` and `platform.c`, copy its options and
  definitions, re-add contract magics and hash an identity. Their build
  identities are inconsistent. Board-derived identities hash inputs and the
  compiler version but no flags, while single-core and GPU identities hash the
  actual options, definitions and `CMAKE_C_FLAGS`. Eight board-derived
  recipes write no `manifest.json`. Changing `CMAKE_C_FLAGS` therefore
  changes those images without changing their build identity.
- **Recompilation** (`build-system-8`). Because each image defines its
  contract magics target-wide, about 11 images (thermal, crypto, RTC, PCIe,
  SPI, ADC, monitor, UART, storage-read, HEVC, deinterlace) recompile the
  board sources, which they otherwise share unchanged.
- **Copied helpers** (`build-system-13`). `apply_build_patch` is defined in
  about 30 files in four equivalent spellings. The base compile flag line is
  repeated in many fragments. The root uses `SUNSTONE_SOURCE_ROOT` without
  setting it and depends on a later include to define it.
- **Patch fuzz** (`supervisor-apps-8`). No copy of `apply_build_patch` passes
  `--fuzz`, so GNU patch's default fuzz of 2 applies. The board single-core
  `main.patch` hunks already apply 26 and 31 lines away from their recorded
  positions with fuzz 2.
- **Single-core variants** (`x-duplication-4`, `buses-pcie-10`). The thermal,
  RTC, PWM, crypto and board single-core recipes differ from each other by
  10-26 lines after normalization; their linker scripts differ in retained
  section and symbol names; `cached.h` is identical in seven directories. The
  PWM image applies the storage-read single-core patch and then its own patch
  on top, swapping `SUNSTONE_THERMAL_SINGLE_CORE` for
  `SUNSTONE_PWM_SINGLE_CORE` in long `#if` lists. The PWM recipe's exclusion
  regular expression names `sunstone-a733-board` instead of its own target,
  which may be a copy error.
- **Launcher generation** (`x-bounds-1`). About 14 `prepare.py` scripts
  rewrite the shared launcher's deadline, magic and session-size validation
  with unguarded `str.replace`. A missed anchor fails closed, because image
  admission ([apps/board/admission.c](../../apps/board/admission.c) lines
  91-94) rejects any deadline other than the configured ones, but the refusal
  is opaque.
- **TinyUSB requirement** (`build-system-14`). The root guards TinyUSB use
  with `if(TINYUSB_DIR)` but unconditionally includes a fragment that patches
  TinyUSB, so a missing TinyUSB fails with an unrelated patch error.

### Proposed design

1. **`cmake/sunstone-common.cmake`,** included first by the root and every
   standalone project. It defines `SUNSTONE_SOURCE_ROOT`, a single
   `apply_build_patch` with `--fuzz=0 --no-backup-if-mismatch`,
   `sunstone_kernel_flags(target)` for the common base flags (targets append
   their own), and `sunstone_build_identity(target TAG INPUTS ...)`.
2. **`sunstone_build_identity`** always hashes the actual compile options,
   definitions, `CMAKE_<LANG>_FLAGS` and compiler and linker binaries, is a
   superset of every identity's current extra inputs, writes `build-id.c`
   with `configure_file`, and always emits `manifest.json` with the final ELF
   digest.
3. **`sunstone_board_application(NAME ... CONTRACT_MAGIC ... RESULT_MAGIC ...)`**
   for the board-derived images. Contract magics and application identity go
   into a generated per-image header included only by `boot.c` and
   `admission.c`. The board base, storage base and SMP kernel become `OBJECT`
   libraries linked into each image of the 11 compatible images. Images that
   override `FreeRTOSConfig.h` or `application.h` keep their own compilation.
4. **`sunstone_single_core_variant(NAME ... PATCHES ... DEFINES ... WORKLOAD ...)`**
   with a linker-script template whose per-variant section and symbol names
   are generated by `configure_file`, one shared `cached.h`, and one
   `SUNSTONE_SINGLE_CORE` define plus per-application hook sources selected by
   CMake for the real differences (IRQ hooks, extra defines).
5. **Launcher parameters as `-D` flags.** The shared launcher reads
   `A733_BOARD_SUPERVISED_SECONDS`, `A733_BOARD_STARTUP_SECONDS` and new
   definitions for the magic, version and session size, as board and
   Bluetooth already do. In the interim, route every launcher edit through
   one helper that requires an exact count for each anchor.
6. **An early `FATAL_ERROR`** when `TINYUSB_DIR` is unset; remove the dead
   guards.

### Migration steps

1. Add the common fragment and switch every project to it. Expect
   `--fuzz=0` to reject existing hunks; regenerate them in the same change.
2. Move build identities to the shared function. Every identity changes once.
3. Convert the board-derived recipes in batches; count compile commands for
   `apps/board/session.c` in `compile_commands.json` to confirm sharing.
4. Convert the single-core variants, starting with the stacked PWM patch.
5. Convert launcher generation.

### Requirements to preserve

- **Evidence identity.** Frozen images stay buildable from their tagged
  commits. The identity function must not drop an input any current identity
  hashes.
- **Per-image boot sources.** Patched or hook-specific `boot.c` and
  `platform.c` remain separate small sources per image.
- **Startup ownership.** The single-core consolidation keeps the
  secondary-off and cached-memory startup that the shared patch provides.

## 14. Theme: host tools and the check harness

Findings: `host-tools-5`, `host-tools-6`, `host-tools-7`, `host-tools-11`,
`checks-harness-2`, `checks-harness-3`, `checks-harness-5`,
`checks-harness-6`, `checks-harness-7`, `checks-harness-13`,
`checks-harness-15`.

### 14.1 Board access tools

All physical access goes through the shared session function `run_session` in
[tools/check-a733-smp-supervised.py](../../tools/check-a733-smp-supervised.py)
and about 24 per-device wrappers.

- **Copied claim validation** (`host-tools-6`, medium). Twenty-four wrappers
  each define `claimed()`, 19 `reference_bytes()` and 15 `freeze_bundle()`.
  The G2D and ISP0 wrappers differ from HEVC's in 10-12 lines after renaming.
  Proposal: shared `require_claim`, `reference_bytes`,
  `freeze_prepared_bundle` and software-record validation helpers, and a
  descriptor per device. Keep the result-key whitelist and per-consumer
  capture limits in one central reviewed table; moving them into
  caller-supplied metadata would weaken the bound on one-attempt sessions.
- **Queue not tied to a port** (`host-tools-5`). The queue database records
  no port, so a claim in one board's queue can run on, and halt, another
  board's work. The reviewed admission still binds the image to its port, so
  the exposure is bookkeeping. Proposal: store the port at queue
  initialization and check it in the shared claim helper; let the dashboard
  read a fixed list of queues.
- **Copied FEL loops** (`host-tools-7`). About ten runners re-implement the
  FEL transaction and observe loops with 15, 30 or size-aware 120-second
  timeouts. Only one checks that the physical port is unchanged before each
  request. The re-enumeration budget uses two keys, `sunstoneos-a733` and
  `sunstoneos-a733-<port>`, which splits one board's events across two
  budgets so the board can exceed its cap. Proposal: one `FelSession` helper
  with a size-aware timeout, a per-request port, identity and ROM check, and
  one budget key per port. Changing the key resets the budget history kept
  under the old key.
- **Unreachable success exit** (`host-tools-11`). After a capture,
  `run_session` holds board ownership in an endless loop, as
  [docs/a733-smp-supervised.md](../a733-smp-supervised.md) documents, so its
  final `print` and `return 0` never run. Proposal: print the summary inside
  `retain_after_capture` after saving and before quarantine, and remove the
  dead branch.

### 14.2 Check harness

[checks/](../../checks/) holds 535 Python drivers, 335 C sources and a few
headers and patches, with no shared module.

- **No shared module** (`checks-harness-3`, medium). Hundreds of scripts copy
  a module loader, hashing, command recording, refusal matching and source
  pinning. There are 15 `refused()` variants with different exception sets
  and 4 `pinned()` variants, two of which use `assert` and so stop checking
  under `python -O`. Proposal: `checks/harness.py` with `load`, `sha`,
  `record_command`, `refused(fn, *exceptions, match=...)` (exception set as a
  parameter), `pinned`, toolchain resolution and sanitizer flags. Fix the
  `assert`-based pins first. A `pinned(name)` backed by
  [sources.lock.json](../../sources.lock.json) needs new lock entries or a
  digest fallback, because many checked files have no lock entry.
- **Evidence directories as test inputs** (`checks-harness-2`, medium). Fifty-six
  checks read baselines from dated directories under
  `~/.cache/sunstoneos/evidence/`. Most are hash-pinned and fail loudly. A few
  are not: `radio-reconnect-wpa.py` runs a QEMU command line read from an
  evidence file, `dsi-replay.py` rebuilds a compile command from one, and
  `bluetooth-mimus.py` builds against an unpinned mimus worktree. Proposal:
  move the QEMU and compile command templates into source, make
  `bluetooth-mimus.py` assert the worktree revision from
  [apps/mimus/source.json](../../apps/mimus/source.json), and report missing
  evidence as a distinct status. Committing the small hashed baselines under
  `checks/fixtures/` with their producer is optional, because the documented
  practice derives fixtures from preserved evidence.
- **Checks executing other checks' text** (`checks-harness-5`). The G2D
  checks slice another check's source at literal anchors and `exec()` it,
  four levels deep. Proposal: move the shared classes into an importable
  module, keeping the hash records of parent models and external references
  that evidence identities depend on.
- **Thirteen launcher checks** (`checks-harness-6`). The SRAM launcher model
  is copied into 13 scripts. Proposal: one `checks/sram-launcher.py` with a
  per-device profile (magic, version, sizes, deadline, extra outputs, CLI
  modes, optional `prepare.py` hook, scope strings).
- **A test file as a library** (`checks-harness-7`). The ELF and DWARF layout
  class lives in the SD0 check `checks/storage-read-native.py`, and 46 files
  load it, including application build scripts that snapshot it into
  evidence. A second copy exists in `checks/jpeg-encode-native.py`. Proposal:
  one side-effect-free `tools/elf_layout.py`, still snapshotted into evidence.
- **Hardcoded paths** (`checks-harness-13`). About 37 files embed
  `/home/sunil/...` paths and 55 embed `/usr/bin/clang-21`. Proposal: resolve
  the cache, compiler and linker once in the harness, using the same linker
  path as `tools/`.
- **Dead headers** (`checks-harness-15`): see section 15.

### Requirements to preserve

- **Bounded sessions.** Every board session keeps a central capture limit,
  fixed timeout ranges and fail-closed quarantine.
- **Retention.** Board ownership is retained after capture.
- **Check semantics.** Each check's refusal contract (its exception set) is
  unchanged. Recorded hashes of inputs remain in `inputs.json`.

## 15. Theme: dead code and stale documentation

These items need no design work. Status records are a project deliverable, so
a stale record sends bring-up work to the wrong place.

### Dead or misleading code

| Finding | Item | Action |
| --- | --- | --- |
| `cpu-startup-7` | `start.S` MPIDR check tests Aff0 | Use `tst x0, #0xffff` (section 6) |
| `cpu-startup-8` | Unreachable `.Lreturned` report; repeated SCTLR masks | Remove; use shared macros |
| `usb-10` | Always-false generator expression on the USB2 host library | Delete line 96 term; keep line 164 |
| `build-system-14` | TinyUSB treated as optional | Early `FATAL_ERROR` |
| `buses-pcie-11` | Orphaned [drivers/spi/a733_spi_native.c](../../drivers/spi/a733_spi_native.c) | Delete with its header and check; repoint [docs/spi.md](../spi.md) to [docs/spi-platform.md](../spi-platform.md) |
| `buses-pcie-13` | SPI uses the TCR `XCH` name for the GCR soft-reset bit; UART DMA uses raw offsets | Add `GCR_SRST` and named UART DMA registers checked against the manual |
| `checks-harness-15` | Seven identical, unreferenced `checks/*-port.h` headers | Delete |
| `host-tools-11` | Unreachable success exit in `run_session` | Section 14.1 |
| `display-10` | Stale DE header contract, unused `a733_de_plan_tv1`, a gap in replay step numbers, magic G2D opcode | Update the contract, delete the planner, renumber or comment the steps, use `A733_G2D_FILL` |
| `clock-power-pins-14` | Write-only `audio_profile`, unused `MODULE` macro | Expose or delete (with its ABI asserts and checks); delete the macro |

### Stale documentation

| Finding | Document | Correction |
| --- | --- | --- |
| `smp-9` | [docs/smp.md](../smp.md), [docs/smp-runtime.md](../smp-runtime.md), [docs/a733-smp.md](../a733-smp.md) | All eight cores have physically reached first-task entry and passed the production barrier; the interrupt-preparation refusal is explained in the reference audit; mark the uncached image superseded or known incompatible |
| `dma-iommu-6` | [docs/dma.md](../dma.md), [docs/dma-pool-session.md](../dma-pool-session.md), [docs/iommu.md](../iommu.md) | Board registration, service serialization and clock setup exist; one reader and one writer may share a FIFO address; cyclic profiles exist; the 4 GiB IOVA clamp applies to every profile |
| `storage-9` | [docs/mmc.md](../mmc.md), [docs/mmc-write.md](../mmc-write.md), [docs/mmc-platform.md](../mmc-platform.md) | 4-bit and high-speed SD are implemented; writes exist only through the bound storage-update writer; backup, readback and journal exist |
| `audio-13` | [docs/i2s.md](../i2s.md) | Platform binding, DMA service, playback and cyclic modes exist; the core still calls the DMA controller directly |
| `camera-11` | [include/a733_camera_platform.h](../../include/a733_camera_platform.h), [docs/camera.md](../camera.md), [docs/camera-dma.md](../camera-dma.md) | A native provider exists; the native boot path captures ordinary images; fix run-together words in several headers |
| `gpu-8` | [docs/gpu.md](../gpu.md) | Completed-job admission is implemented; the frozen two-job image must still be replaced before physical use |
| `npu-11` | [docs/npu-controller.md](../npu-controller.md), [docs/npu-native.md](../npu-native.md), [docs/npu-mmu.md](../npu-mmu.md), [docs/npu.md](../npu.md) | The components are linked into several board images; size comments are per profile; an interrupt profile exists |
| `video-11` | [docs/video-codecs.md](../video-codecs.md) | H.264 decode includes VGA and full-HD Baseline streaming and a High-profile build; HEVC is partial and unintegrated |
| `clock-power-pins-14` | [docs/clock-leases.md](../clock-leases.md) | Document LRADC (bit 72) and RTC32K (bit 73); bits 74-127 are invalid |
| `supervisor-apps-14` | [docs/board-application.md](../board-application.md), [include/a733_board_application.h](../../include/a733_board_application.h) | Header word 1 is the payload kind 1-4; radio profile versions 3-6 are 1,872 bytes; describe the full workload range; remove the duplicate `_Static_assert` |
| `bringup-gaps-13` | [docs/bringup-sequence.md](../bringup-sequence.md) | Watchdog expiry does not gate unrelated work ([docs/goal.md](../goal.md) line 33); keep the recovery gate on persistent writes and describe the BOOT/GND procedure as documented but not yet qualified |

## 16. Recommended order of work

The order puts low-risk, behaviour-preserving changes first and defers changes
that alter image identity or need board evidence.

1. **Documentation and dead code** (section 15). No behaviour change.
2. **Build safety.** One `apply_build_patch` with `--fuzz=0`, an early
   TinyUSB check, the core-count `#error` in a shared header, and anchored
   launcher generation (sections 6 and 13).
3. **Shared C helpers** (sections 7 and 8): task context, range and deadline
   helpers, counter reads, the MMIO header with unchanged barriers, one
   cache-op enum, the mapping helper and the HDMI audio shareability fix.
   Each conversion keeps object behaviour and is checked by the existing host
   checks.
4. **Host harness module and FEL session helper** (section 14), which make
   later evidence cheaper to produce.
5. **USB0 service** (section 10), migrated for new builds only.
6. **Build identity and recipe functions** (section 13). Every identity
   changes once.
7. **CPU startup consolidation and SMP variant retirement** (section 6),
   validated by system-register trace diffs.
8. **Contract, session and interrupt tables** (sections 4 and 5).
9. **Provider authorities and subsystem consolidations** (sections 9, 11
   and 12).
10. **Integrated composition** (section 3): the shared provider root, the
    accelerator composition under the SMP image, and the descriptor table when
    a concrete composed workload exists.

## Appendix: finding index

| Section | Findings |
| --- | --- |
| 3 Application composition | `supervisor-apps-1`, `supervisor-apps-10`, `camera-7`, `gpu-5`, `mimus-perf-6`, `mimus-perf-12` |
| 4 Boot hooks and IRQ dispatch | `supervisor-apps-2`, `x-concurrency-5`, `x-concurrency-6` |
| 5 Contract, session and layout | `supervisor-apps-3`, `bringup-gaps-11`, `supervisor-apps-6`, `bringup-gaps-12` |
| 6 CPU startup and SMP variants | `cpu-startup-4`, `cpu-startup-6`, `cpu-startup-7`, `cpu-startup-8`, `cpu-startup-9`, `smp-7`, `smp-8`, `build-system-11`, `x-coherence-10` |
| 7 Shared runtime helpers | `clock-power-pins-12`, `security-misc-10`, `buses-pcie-9`, `x-concurrency-8`, `x-duplication-6`, `x-bounds-4`, `x-duplication-9`, `bluetooth-10`, `radio-net-13`, `gpu-6` |
| 8 Mapping, cache and MMIO | `x-coherence-2`, `x-coherence-3`, `x-coherence-4`, `x-coherence-6`, `x-coherence-7`, `gpu-7`, `runtime-memory-6` |
| 9 Device-memory ownership | `runtime-memory-5`, `runtime-memory-7`, `runtime-memory-8`, `dma-iommu-5` |
| 10 USB0 transport | `usb-2`, `usb-10`, `supervisor-apps-5`, `x-bounds-3`, `x-duplication-7` |
| 11 Provider authorities | `clock-power-pins-9`, `clock-power-pins-10`, `clock-power-pins-13`, `security-misc-6`, `security-misc-7` |
| 12 Subsystems | `dma-iommu-3`, `dma-iommu-4`, `dma-iommu-7`, `dma-iommu-9`, `audio-1`, `audio-5`, `audio-7`, `audio-11`, `display-4`, `display-5`, `camera-8`, `video-6`, `video-7`, `gpu-9`, `npu-4`, `npu-5`, `npu-6`, `npu-10`, `storage-8`, `radio-net-11`, `radio-net-12`, `security-misc-8`, `security-misc-9` |
| 13 Build structure | `build-system-8`, `build-system-13`, `build-system-14`, `supervisor-apps-8`, `x-duplication-4`, `x-duplication-5`, `buses-pcie-10`, `x-bounds-1` |
| 14 Host tools and harness | `host-tools-5`, `host-tools-6`, `host-tools-7`, `host-tools-11`, `checks-harness-2`, `checks-harness-3`, `checks-harness-5`, `checks-harness-6`, `checks-harness-7`, `checks-harness-13`, `checks-harness-15` |
| 15 Dead code and stale documentation | `cpu-startup-7`, `cpu-startup-8`, `usb-10`, `build-system-14`, `buses-pcie-11`, `buses-pcie-13`, `checks-harness-15`, `host-tools-11`, `display-10`, `clock-power-pins-14`, `smp-9`, `dma-iommu-6`, `storage-9`, `audio-13`, `camera-11`, `gpu-8`, `npu-11`, `video-11`, `supervisor-apps-14`, `bringup-gaps-13` |
