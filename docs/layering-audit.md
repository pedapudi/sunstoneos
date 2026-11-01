# SunstoneOS layering: application details below the application layer

SunstoneOS builds OS-only, hello-compute and external-application images.
Mimus lives under `sunstoneos/` in its own repository, pinned by
[the source record](../apps/mimus/source.json) to `port/sunstoneos-split`
at `c2dcb5a9`. The [application interface](designs/app-abi.md) and
[image guide](os-image.md) describe the merged composition.

## Current merged boundary

The OS owns device services, session admission, supervision and generic
application launch/result handling. The application owns recognition, its
models, manifests, capture wrappers and recognition checks. The merged
reference cleanup keeps the shared USB emulator in SunstoneOS and accepts
explicit application inputs for preparation and validation.

The legacy GPU recognition path still crosses this boundary through
`drivers/gpu/pvr_mimus_service.c`, `platform/a733/gpu_mimus.c` and their
interfaces. The generic GPU queue and its mimus port remain unmerged.
Application separation and host checks establish no new physical qualification;
[the handoff](handoff.md) records the latest board failures and open work.

## Scope of the original audit

The findings below describe `bringup` at `222b070c`, before application
separation. Their source paths and line numbers identify that baseline.
They preserve the target layering and the reasons for the refactoring;
they are not a claim that every original defect remains in the merged code.
Inspect current source before assigning work from a historical finding.

## Target layering

| Layer | Contents | Application-specific content |
| --- | --- | --- |
| L0, SoC and board support | Register-level drivers, clocks, rails, PMIC, the EL3 monitor, boot | No |
| L1, OS kernel and services | Scheduling, memory and DMA, interrupt routing, power, clock and thermal policy, and device services with generic interfaces: GPU compute queue, NPU job submission, audio capture, USB transport, storage | No |
| L2, application framework | Image composition, the application manifest and interface, the session contract, the host launch and return protocol, the host channel, the test harness | Only through manifests |
| L3, applications | mimus and other applications: their models, kernels, input sizes, output formats, deadlines and policies | Yes |

A **leak** is L0–L2 code that names, is sized for, or branches on one
application, or an interface whose shape fits only one application. A
**reverse leak** is OS mechanism that lives inside an application.

Each finding below carries one of three severities:

- **Blocks a second application:** another application cannot use the
  facility without editing L0–L2.
- **Awkward:** usable, but the design is distorted.
- **Cosmetic:** naming or file location only.

## Layers that already follow the model

- **GPU firmware and power:** the firmware transport and lifecycle code
  (`pvr_ccb`, `pvr_cccb`, `pvr_mips`, `pvr_firmware`, `pvr_gpu_start` and
  `pvr_gpu_stop`), the A733 GPU power and resource code, and the shared
  compute-job encoders in `include/pvr_compute_job.h`.
- **NPU core:** NBG decoding, the general assembler and relocation, the NPU
  MMU, memory and controller code, `npu_service` (OPEN, SUBMIT, WAIT, LOAD
  and CLOSE with up to eight clients), and `npu_runtime` and `npu_session`,
  which size every allocation from the model's own plan.
- **Platform:** the GIC save and restore in `platform/a733/monitor.c`, the
  declarative interrupt table in `platform/a733-smp-cached/irq_table.c`, the
  I2S format and clock plan, the DMA job service, storage and FatFs, the
  device-memory lease allocator, clock, rail and PMIC leases, and the thermal
  driver.
- **Host tools:** the board test queue, the one-attempt FEL transport in
  `tools/a733-returning-host.py`, port selection and locking in
  `tools/usb-access.py`, and the `--asset PATH@LOAD` option of
  `tools/boot-slots.py`.

## Defects, most severe first

### 1. The OS image is built from the mimus build target

`cmake/a733-os.cmake:102-117` copies the source list, compile options and
link options of `sunstone-a733-mimus-board`, then removes and patches files.
The OS cannot be configured without a mimus checkout
(`cmake/a733-mimus-board.cmake:2`). `apps/os/os.h:3` describes the image as
"the mimus board composition plus" OS features.

- **Correct structure:** an OS core target built from L0 and L1 sources, and
  an `add_sunstone_application()` CMake function that links one or more
  applications and their manifests. mimus becomes one caller.
- **Severity:** blocks a second application. **Effort:** large.

### 2. No application interface exists; images are told apart by numbered workloads

- **Fixed start list:** `apps/os/start.c:9` names `"mimus"`, and
  `apps/os/services.c:22` gives it a fixed 3 s supervision window.
  `apps/os/prepare.py:329` and `checks/os-image-link.py:290` refuse any
  other application list.
- **Numbered workloads:** one integer, `workload` (`apps/os/os.h:17-33`,
  `include/a733_board_application.h:54-59`), combines the device set with
  mimus interaction modes. The return path admits workload 2 only.
- **Boot loader reads OS session layout:** the stage-2 loader includes
  `apps/os/os.h` and branches on `workload == 2`
  (`boot/a733/stage2/loader.c:18,471-515`).
- **Per-application session overlays:** mimus overlays its own fields on the
  storage session at fixed offsets
  (`platform/a733-storage-maintenance/session.h:7-13`).
- **Correct structure:** a manifest in each application image that declares
  devices, CPU requests, memory regions, deadlines, host-message range and an
  opaque parameter block. The session becomes a generic prefix, a list of
  typed extents and the hashed application block. The boot loader checks the
  session length and digest only.
- **Severity:** blocks a second application. **Effort:** medium to large.

### 3. OS features reach applications through source patches

The build inserts OS behaviour into mimus source files with text patches:
capture source selection, CPU clock waits, worker binding and the GPU
session (`apps/os/application.patch`, `apps/os/recognition*.patch`). The
monitor is composed the same way, with more than 20 `apply_build_patch`
calls against `platform/a733-smp-cached/boot.c`, `link.ld` and
`continue.S`.

- **Correct structure:** stable L1 calls that applications make
  (`ss_clock_ready()`, `ss_capture()`, `ss_gpu_open()`), and explicit
  monitor hook points for interrupt routing, return save and restore, and
  the SVC table.
- **Severity:** blocks a second application. **Effort:** medium to large.

### 4. The launch and return protocol is shaped for recognition

- **Transcript result buffer:** the return API is
  `os_cpu_return_begin(text, bytes)` into a 16 KiB transcript buffer
  (`apps/os/cpu-return.c:10-13,52-63`).
- **Recognizer arena by address:** the EL3 terminal cleans the recognizer
  arena at a fixed `0x46000000`–`0x47000000`
  (`apps/os/cpu-return/terminal.patch:34`). The host runner backs up the
  same range (`apps/os/cpu-return-host.py:58,259`).
- **Hand-kept interrupt save list:** the list names the thermal and GPU
  interrupts and the recognition core (`apps/os/cpu-return-spi.c:1-20`).
  When the route count differs, the code skips the save without any report.
- **Build and entry checks:** the build refuses any worker count other than
  eight (`cmake/a733-os-cpu-return.cmake:2-7`). The entry guard fixes clock
  rates and a 70 °C trip (`apps/os/cpu-return-entry.c:154-159`).
- **Three fixed upload addresses:** the upload layout is three mimus files
  (two models and a WAV) at fixed addresses with pinned hashes
  (`apps/mimus-board/host.py:11-26`). The EL3 code publishes
  `mimus_asset_address[3]` (`apps/os/identity.c:226`).
- **Correct structure:** a generic `ss_finish(result, bytes)` that writes an
  OS-owned result record. Cleaned and backed-up ranges, upload regions and
  reserves all come from the manifest. The interrupt save list is derived
  from the interrupt table, and the host decodes results with the
  manifest's decoder.
- **Severity:** blocks a second application. **Effort:** medium.

### 5. The host channel is the mimus USB protocol, and OS diagnostics travel inside it

Operations 0–4 of the mimus frame (`0x424d494d`) carry status, transcript,
storage sector, keyboard trigger and acknowledgement. The OS fault, NPU,
thermal and performance-mode operations 5–8 are `#ifdef` branches in the
mimus handler (`apps/mimus-board/usb0.c:13-35,75-110`).

- **Correct structure:** an OS-owned host channel with reserved OS
  operations and an operation range assigned to each application.
- **Severity:** blocks a second application. **Effort:** medium.

### 6. The production GPU compute service is the mimus linear-layer job

- **mimus job record:** the only production compute path, `pvr_mimus_service`,
  accepts the mimus job record `{x_offset, ordinal, rows, x_stride}` against
  a table of admitted tensors (`include/pvr_mimus_service.h:52-55`,
  `include/pvr_compute_mimus.h:21-47`). It links the mimus kernel into the
  driver.
- **Worker branches on mimus:** the core GPU worker branches on the mimus
  component by name, reaches into its slot for the performance hold, and
  allows reopening only for mimus (`drivers/gpu/pvr_gpu_service.c:12-23,
  108-109,492-520,804-810`). The mimus session excludes every other GPU
  client.
- **Fixed buffer map:** the CPU page tables and EL3 boot step encode the
  mimus buffer map: two 8 MiB models, a 12 MiB arena and a 2 MiB read-only
  output region (`platform/a733-smp-cached/memory.c:109-117`,
  `apps/os/identity.c:34-47`, `platform/a733/gpu_mimus.c:74`).
- **Recognizer-tied option:** the build option `SUNSTONE_A733_OS_GPU_LINEAR`
  ("mimus linear layers on the GPU") is the only way to link the GPU
  service (`cmake/a733-os.cmake:284-320`).
- **Undersized generic service:** the generic compute service accepts one
  fixed job with at most 4 KiB of input and 3,968 bytes of output
  (`include/pvr_compute_package.h:7-8`), so it cannot serve real work.
- **Correct structure:** one L1 compute queue that takes a program handle,
  buffer bindings, a grid size and a small argument block. The GPU worker
  accepts registered components through a function table. Buffers are
  leased with per-buffer CPU and GPU permissions. The mimus kernel, tensor
  table, placement generator and CPU fallback move to the mimus
  application.
- **Severity:** blocks a second application. **Effort:** large.

### 7. The NPU admits only networks listed inside the driver, through one built-in client

- **Admission table in the driver:** the admission table, including
  `linear-i16-per-row`, is a source file in `drivers/npu`
  (`drivers/npu/npu_session_models.inc:19-22`). The table generator writes
  that file into the driver tree (`tools/npu-session-catalogue.py:23-24`).
- **One OS-created client:** the OS NPU service has one slot and one client
  task that the OS creates. Its policy is fixed: a 1 s submit cap and a 60 s
  work window (`apps/os/npu.c:185,248-250,291-297`).
- **Fixed model count:** both image builds fail unless the catalogue holds
  exactly five models (`apps/os/npu-catalogue.py:17`).
- **Recognizer build gate:** the NPU build option refuses audio, radio and
  headset with the message "requires recorded recognition"
  (`cmake/a733-os.cmake:69-79`).
- **Model profiles in L0:** an older single-model path reshapes the L0
  device-memory layout through compile-time profiles for particular test
  networks (`include/a733_device_memory_layout.h:16-35`,
  `drivers/npu/nbg_sh_nt.c:28-61`).
- **Correct structure:** a model table supplied at link time and composed
  from application manifests, runtime slots handed to registered
  application tasks, and per-session deadline limits. The single-model
  path is retired in favour of `npu_runtime`.
- **Severity:** blocks a second application. **Effort:** medium.

### 8. Audio capture is the recognizer's input path

- **Recognizer limits in the OS:** the capture call returns one complete
  16 kHz mono WAV of 1–5 s per "recognition generation"
  (`apps/os/os.h:70-72,158-163`). Session admission enforces the length
  limits (`apps/os/admission.c:49`).
- **Fixed rate:** the I2S platform binding refuses every rate other than
  16 kHz (`drivers/audio/a733_i2s_platform.c:272`), although the I2S core
  supports 8–192 kHz.
- **Fixed period size:** cyclic receive requires 4,096-byte periods
  (`drivers/audio/a733_i2s_stream.c:93`, `drivers/dma/dma_service.c:514`).
- **Correct structure:** a capture stream opened with a format descriptor,
  delivering a ring of periods into a buffer that the client supplies. WAV
  framing and length limits move to the application.
- **Severity:** blocks a second application. **Effort:** medium.

### 9. Fixed physical addresses stand in for a memory map

The address `0x43000000` holds the mimus model or the radio profile
depending on the build. The address `0x46000000` is the recognizer arena,
and the EL3 terminal also hard-codes it (`apps/mimus/recorded.h:9`,
`apps/os/os.h:181`).

- **Correct structure:** a region table generated by the linker, used by
  the monitor, the host tools and applications.
- **Severity:** awkward. **Effort:** medium.

### 10. Kernel core placement uses recognizer roles

`A733_SMP_RECOGNITION_CORE` and `A733_SMP_INFERENCE_CORE` are kernel
configuration (`include/a733_smp_placement.h:16-22`), and the GPU interrupt
is routed to the recognition core (`apps/os/irq.c:28`).

- **Correct structure:** the kernel publishes cluster masks. Applications
  request cores, and each interrupt route names its own target core.
- **Severity:** awkward. **Effort:** small.

### 11. OS mechanism lives in applications or in the mimus repository

- **USB safety guard:** `usb_guard`, the host USB re-enumeration budget and
  xHCI health check, is imported from `~/git/mimus/tools` by about 20 board
  tools (`tools/a733-returning-host.py:30-32`). No board tool runs without
  a mimus checkout. Severity: blocks a second application. Effort: small.
- **C++ runtime sections:** a mimus patch adds the C++ constructor and heap
  sections to the linker script (`apps/mimus/link.patch`).
- **NPU interrupt binding:** the binding lives in a demonstration directory
  (`apps/npu-interrupt/irq.c`), and the OS includes it from there.
- **Watchdog code:** five launchers arm the watchdog with their own
  literals, and the OS keeps a second renewal implementation
  (`platform/a733/maintenance_watchdog.c`, `apps/os/watchdog.c`).
- **Duplicate admission in mimus:** mimus keeps its own contract magic
  numbers, session admission and mapping checks
  (`apps/mimus/admission.c:7-22`).

### 12. Tooling has no generic launcher

- **Copied tooling:** 69 application directories hold 224 Python files,
  about 32,000 lines. One module-loading helper is defined in 116 files.
- **Per-application runners:** each probe has its own prepare script, host
  adapter and runner against fixed addresses.
- **Gate assumes mimus:** the integration gate configures every OS variant
  with the mimus application (`tools/integration-gate.py:225,232`).
- **Correct structure:** one launcher driven by the manifest, with prepare
  and run steps, and one shared helper module. The gate builds the OS
  without applications, then runs each application's own checks.
- **Severity:** awkward. **Effort:** large.
- **Status:** implemented for CPU-return images. `tools/dev-run.py` builds
  the typed session, uploads and backups from the image's manifest and runs
  hello-compute or an external application such as mimus through
  `apps/os/prepare.py` and `apps/os/cpu-return-host.py`. GPU-return and NPU
  images still have per-application runners.

### 13. Application headers and tools sit in OS directories

- **Demonstration headers:** thirteen `include/a733_*_application.h`
  headers are per-demonstration wire formats. Some drivers take them as
  arguments: `a733_g2d_commands` takes the G2D demonstration session, and
  the DisplayPort driver includes the demonstration's 720p constants.
- **mimus code:** the mimus float read-out (`include/pvr_compute_mimus_readout.h`)
  and the mimus NPU tools (`tools/npu-mimus-*`) also sit outside mimus.
- **Severity:** cosmetic to awkward. **Effort:** small to medium.

## Refactoring order

The order keeps each step small enough to review and lands the enabling
interface before the services that use it.

1. **Independent moves:**
   - move `usb_guard` into `tools/`;
   - add the C++ sections to the base linker script;
   - move the NPU interrupt binding into `platform/a733/`;
   - rename the placement roles to cluster masks;
   - move the demonstration headers into their application directories;
   - make the integration gate build the OS without an application.

   Each move is small and touches files outside the work in progress on GPU
   bisection, NPU inference and watchdog recovery.
2. **Application interface and OS core target:** define the manifest and
   the L2 interface header, build the OS core without mimus, and add
   `add_sunstone_application()`. In the same step:
   - generate the start list from the manifests;
   - replace workload numbers with a device bitmap;
   - make the boot loader treat the session as opaque.

   mimus becomes the first application, and a minimal second application
   proves the interface.
3. **Generic launch, return and host channel:**
   - `ss_finish()` for results;
   - ranges to clean and back up, taken from the manifest;
   - an interrupt save list derived from the interrupt table;
   - an OS-owned host channel with operation ranges;
   - one launcher driven by the manifest.
4. **Device services:**
   - the GPU compute queue, replacing both current compute services;
   - the NPU link-time model table and multi-client slots;
   - the audio capture stream with a format descriptor.

   These can proceed in parallel because each owns separate files.
5. **Retirement:** remove the single-model NPU path, the per-probe runners
   that the generic launcher replaces, and the remaining fixed addresses.

Steps 2 and 3 edit `cmake/a733-os.cmake`, `apps/os/` and
`apps/os/cpu-return-host.py`, which the current GPU, NPU and watchdog work
also edits. Those branches should merge before step 2 starts.

## Completion test

A new application that is not mimus, for example an image classifier on the
NPU with a GPU preprocessing kernel, must build, upload, launch and return
its results with these properties:

- it adds files only under its own `apps/<name>/` directory and its
  manifest;
- it edits nothing under `drivers/`, `platform/`, `boot/`, the OS core
  build files or `tools/`;
- mimus passes its existing recognition checks as an application of the
  same OS core.
