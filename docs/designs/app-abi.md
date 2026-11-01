# SunstoneOS application interface and OS core build

An application on SunstoneOS is code linked into the OS image together with
a manifest: a fixed-layout record that declares the devices, CPUs, memory,
deadlines, host-message operations and parameter block that the application
needs. The OS builds its start list, supervision table, interrupt routes and
memory checks from the manifests, and admits a session only when the session
grants what the manifests declare. Applications reach the OS through a small
set of `ss_*` functions. This design is step 2 of the
[layering refactoring](../layering-audit.md); mimus, the speech recognizer,
becomes the first application of an OS core that no longer contains it.

**Status.** Design, with a host-checked prototype:

- `include/sunstone_app.h`: the manifest layout and the application calls;
- `include/sunstone_session.h`: the session, host frame and result layouts;
- `tools/sunstone-manifest.py`: generates the manifest section from
  `manifest.json` and decodes it again;
- `apps/hello-compute/`: a second application that returns the Adler-32
  checksum of an input region;
- `apps/mimus-board/manifest.json`: a draft mimus manifest that pins the
  addresses, sizes and digests of the current build;
- `checks/sunstone-app-abi.py`: compiles both manifests for the host and for
  `aarch64-none-elf`, compares the compiled section bytes with the tool's
  packing, runs hello-compute on a host model of the calls, and checks the
  mimus draft against today's constants.

No OS image implements the calls or links an application through them yet.

## The manifest declares what an application needs

Each application has a `manifest.json`. At configure time,
`tools/sunstone-manifest.py` validates all manifests of one image together,
assigns addresses to regions that do not pin one, and generates a C file that
places a 560-byte `struct ss_manifest` in the section `sunstone_manifest`.
The JSON form exists because CMake must read the manifest before anything is
compiled; the section is the authority for a linked image.

| Field | Meaning |
| --- | --- |
| `name` | 1–15 characters `[a-z0-9-]`. The application's functions are `<name>_init` (optional, before the scheduler) and `<name>_main`, with `-` written as `_`. |
| `devices_required`, `devices_optional` | Device bitmaps: USB storage, USB keyboard, I2S capture, headset capture, radio, GPU, NPU. The session grants every required device and any subset of the optional ones, with at most one capture device. USB0, the host channel, and thermal supervision are always present. |
| `privilege` | Exception level of the main task: EL0 or EL1. |
| `cluster`, `workers` | Preferred cluster of the main task and the number of worker tasks. The main task gets the first core of the cluster's pair: core 6 for the A76 pair, or core 2 for the A55 pair when the A76 cores are not admitted. The first worker gets the second core of that pair, and further workers the remaining cores in ascending order. This is today's mimus placement rule. |
| `priority`, `stack_bytes`, `heap_bytes` | FreeRTOS priority and stack of the main task, and the bytes the application adds to the image's C and C++ heap. |
| `regions[8]` | Physical ranges with kind, access and digest. INPUT regions are uploaded by the host and read-only; `bytes` is the maximum content length, or the exact length when the manifest pins a SHA-256. WORK regions are private working memory; OUTPUT regions are written by the application and read by the host after the run. Access bits are CPU read, CPU write, GPU read and GPU write; a WORK region with GPU write and without CPU write is a GPU output region that the CPU only reads. Unpinned regions are placed on 2 MiB boundaries in the application window `0x44000000`–`0x47000000`. |
| `audio` | Rate, channels, sample bits and the maximum frames of one capture, when a capture device is listed. |
| `run_deadline_us` | Longest time from the start of `<name>_main` to `ss_finish`. Zero means the session deadline less the return reserve bounds the run. |
| `return_reserve_us` | Time between the latest permitted `ss_finish` and the session deadline, which the return path uses. |
| `progress_window_us` | Longest permitted gap between `ss_progress` calls from any task of the application. |
| `output_limit` | Maximum bytes of the `ss_finish` result. |
| `host_op_first`, `host_op_count` | The application's host-channel operation numbers, inside `0x00`–`0x7f`. The OS owns `0x80`–`0xff`. Ranges of applications in one image are disjoint. |
| `params_limit` | Maximum bytes of the opaque parameter block that the session supplies. |

The generator refuses a manifest that breaks these rules: an INPUT with
write access, a region outside the window or overlapping another
application's region, overlapping host ranges, a required capture device
together with a second capture device, or a capture device without a format.

## Host tools and admission read the manifest section

The base linker script keeps `sunstone_manifest` in the read-only image block
with `__start_sunstone_manifest` and `__stop_sunstone_manifest` around it, so
the section is an array of manifests in image order.

- **Host tools** read the section from the ELF image with the decoder in
  `tools/sunstone-manifest.py`. The upload step takes INPUT addresses, sizes
  and pinned digests from it. The return step takes the WORK and OUTPUT ranges
  to back up and the result size. The session writer takes the SHA-256 of the
  section, the device rules and the parameter limit.
- **The image link check** decodes the section and compares it with the
  generated start list, service table, interrupt routes and region table.
- **OS admission** runs once, before the start hooks. It computes the SHA-256
  of the section and compares it with the session header, so a session
  prepared for another build of the applications is refused. It then checks
  the session against each manifest, as described next.

## The session is a board prefix, typed extents and application blocks

The session page at `0x41f00000` holds up to 4,096 bytes, the size of the
sealed copy in `apps/board/session.c`.

| Offset | Bytes | Contents |
| --- | --- | --- |
| 0 | 592 | `struct a733_board_session`, the board prefix, in today's layout |
| 592 | 64 | `struct ss_session_header`: magic, version, total bytes, extent count, granted device bitmap, flags, SHA-256 of the manifest section |
| 656 | variable | Extents: an 8-byte `{type, index, bytes}` header and its payload, each padded to 8 bytes |

Extent types:

- **OS service extents** carry today's records unchanged, found by type
  instead of by fixed offset: thermal facts (216 bytes, the current OS
  extension), I2S receive facts (184), NPU client facts (272) and the CPU
  clock grant (96).
- **INPUTS**, one per application with INPUT regions: a length and SHA-256
  for each INPUT region, in manifest order.
- **PARAMS**, one per application: the SHA-256 of the opaque parameter block,
  then the block.

The mimus session with NPU, CPU grant, three inputs and a 16-byte parameter
block is 1,448 bytes.

**Stage-2 loader.** The loader stops including `apps/os/os.h` and performs
these steps:

- check that the slot's session extent length equals the header's `bytes`
  and lies between 656 and 4,096 bytes;
- check the prefix magic, version, generation and lease, which the contract
  copies;
- check the SID digest when the header's `SS_SESSION_BIND_SID` flag is set;
- stamp the prefix epoch, then bind the length and SHA-256 into the contract
  as today.

It reads no extent and no workload number. A CPU grant whose epoch is zero
takes the prefix epoch, so the loader no longer writes into the grant.

**OS admission** checks:

- the board prefix with the board validator, as today;
- the header magic, version and length, and the manifest-section digest;
- that the extents are in bounds, padded, and each type and index appear at
  most once;
- every OS service extent with its existing validator;
- that the granted device bitmap contains every application's required
  devices and no device that no application lists. Each application's grant
  is the bitmap restricted to its own required and optional devices, and
  holds at most one capture device;
- each INPUTS extent: one entry per INPUT region, length within the region,
  and digest equal to the manifest's pinned digest where one exists;
- each PARAMS extent: length within `params_limit` and a matching SHA-256;
- for finite lifetimes, that the session deadline leaves each application's
  run deadline and return reserve.

## Applications call the OS through plain functions that are safe at EL0

FreeRTOS tasks on SunstoneOS run at EL0 unless the image selects EL1, and EL0
tasks call kernel functions directly; kernel data, stacks and heap are mapped
read-write at EL0 ([memory protection](../memory-protection.md)). The
existing EL0 calls follow one pattern: an atomic store or load in shared
memory, with an EL1 owner task doing the device work. `os_audio_capture`,
`BOARD_PROGRESS` and `board_fail` work this way. The `ss_*` calls use the same
pattern. A system-call trap for each call would add an exception entry and
exit and would isolate nothing while EL0 can write kernel data. If
applications later need isolation from the OS, these functions are the only
OS entry points that applications use, so they can become trap stubs then.

An EL1 application (`privilege` EL1) is admitted by the generated
`task_privileged` hook for its main task only; tasks it creates run at EL0.
It may also call L1 driver interfaces directly. The OS NPU client, which runs
at EL1 today, is the current case.

| Call | Behaviour | Current equivalent |
| --- | --- | --- |
| `ss_grant()` | Immutable record of the grant: devices, main core, worker cores, region spans (actual INPUT lengths), the parameter block and the finish-by time. | `mimus_asset_*`, `board.session.workload`, `A733_SMP_RECOGNITION_CORE` |
| `ss_live()` | 1 when the session is live and the granted devices are ready, 0 while they are starting, -1 once the session has ended. | `mimus_board_live`, `mimus_board_session_live` |
| `ss_progress()` | One atomic store into the application's supervision record. | `mimus_board_progress` |
| `ss_throttled()` | Thermal load hint. | `os_thermal_throttled` |
| `ss_capture(sequence, frames, &pcm, &bytes)` | PCM samples in the manifest format from the granted capture device. The 64 bytes before the samples belong to the caller, so mimus writes its WAV header in place without a copy. | `os_capture` returning a WAV |
| `ss_host_recv(&request, wait_us)`, `ss_host_send(&reply)` | One 16-byte request in the application's range, and its reply of up to 1,024 bytes plus a 16-byte application tag. The OS passes the next request only after the USB transfer of the previous reply completed, so a request also confirms delivery of the previous reply. The OS sends nothing for an application request until the application replies. | `mimus_board_copy` and the mimus USB0 exchange |
| `ss_storage_read(sector, data)`, `ss_key_report(&report)` | One 512-byte read from the USB storage leaf, and the next boot-keyboard report, both served by the OS USB2 owner. | the generated `mimus-client.c`, a patched copy of `apps/board/client.c` |
| `ss_finish(result, bytes)` | Copies the result into the OS result record and ends the application. A returning lifetime then returns to the host; the resident lifetime keeps the record readable over the host channel. | `os_cpu_return_begin(transcript, used)` |
| `ss_fail(detail)` | Ends the session. | `mimus_board_fail` |

`ss_storage_read` and `ss_key_report` are outside the step-2 list in the
layering document. They are included because mimus cannot leave the patched
USB2 owner without them.

**Main task.** The OS creates one main task per application from the
manifest. Before it calls `<name>_main`, the task waits for `ss_live() == 1`,
authenticates every INPUT region in 4 KiB steps with progress reports, and,
when the image grants CPU operating points, waits up to 1 s for the clock
owner to finish its requests. These three steps are copies of the mimus
recognizer's own startup and of the clock wait in the placement patch.
Returning from `<name>_main` is a failure.

**Result record.** `struct ss_result` holds magic, version, record size,
epoch, generation, application index, status and payload length, followed by
the payload. The image reserves one record of 56 bytes plus the largest
`output_limit` of its applications, in the symbol `ss_result_record`. Each
application has a host-side decoder for its payload.

**Host channel.** The OS owns the USB0 exchange and keeps today's 64-byte
frame header, including its magic `0x424d494d` and the storage-state word at
offset 24. The OS fills the header; the application supplies the version
word, state, total, tag and payload of its own replies.

## The build composes an OS core with applications

- **`sunstone-a733-os-core`**: an object library with the board composition
  (the `sunstone-a733-board` sources without the board's USB0 exchange, USB2
  client, identity hook, admission and supervised boot), the OS services,
  admission, start, supervision, thermal and watchdog code, the interrupt
  table and fault capture, and the C and C++ runtime. Each device service
  (I2S capture, headset, radio, NPU, GPU) is a separate object library.
- **`add_sunstone_application(name SOURCES ... MANIFEST ...)`**: an object
  library of the application's sources, compiled with the core's flags, and
  the path of its manifest. The manifest passes through `configure_file`, so
  a field may name a CMake variable; mimus uses this for its worker count.
- **`add_sunstone_image(target LIFETIME supervised|resident|return APPLICATIONS ...)`**:
  the image. It runs the generator over the applications' manifests and the
  OS device table, links the core, the device services that the manifests
  name and the applications, and adds the manifests and application sources
  to the build identity. This third function is needed because an image is a
  choice of core lifetime and applications.

```cmake
add_sunstone_application(hello-compute
  SOURCES apps/hello-compute/main.c
  MANIFEST apps/hello-compute/manifest.json)
add_sunstone_image(sunstone-a733-os-hello LIFETIME return APPLICATIONS hello-compute)
```

The generator writes, from the manifests and an OS-owned device table:

- **Start list:** the OS service entries, then one entry per application
  whose `task_privileged`, `pins` and `start` functions are generated. `pins`
  checks the region mappings from the region table; `start` calls
  `<name>_init` and creates the main task.
- **Supervision table:** board services, then one service per application
  with its `progress_window_us`, then the OS services. mimus keeps service
  ID 5, which today's resident stop details use.
- **Interrupt routes:** each device row of the device table names its INTID,
  handler and core rule. Thermal 71 and I2S DMA 143 go to core 0, NPU 97 to
  the NPU core, and GPU 95 to the main core of the application granted the
  GPU. The return path saves the generated routes and no others.
- **Region table:** address, size, kind, access and application of every
  region. The `pins` hooks check it; EL3 publication cleans INPUT regions; the
  return terminal cleans WORK and OUTPUT regions; host tools back up WORK and
  OUTPUT regions.
- **Image settings:** the device-service compile definitions that
  `SUNSTONE_A733_OS_AUDIO` and similar options set today, and the heap size.

The image link check also refuses an application object that references a
symbol outside its own sources, the `ss_*` calls, FreeRTOS and the C and C++
libraries.

## Migration of the mimus mechanisms

| Current mechanism | Replacement |
| --- | --- |
| Workload integers 0, 1, 2 and 4 (`os_workload_valid`, `mimus_board_staged`, `mimus_board_keyboard`) | The session header's device bitmap. mimus derives its mode from the grant: keyboard granted means repeated recognition; storage without keyboard means one recognition with the concurrent sector read; neither means one staged recognition. During migration the OS writes the board prefix's `workload` from the bitmap: storage is 0, storage and radio are 1, none is 2, storage and keyboard are 4. Board-only workloads 6–14 are unchanged. |
| `mimus_asset_address`, `mimus_asset_bytes`, `mimus_asset_sha256` (`configure.py`, `identity.c`, `mimus_board_pins`, `host.py`, the link check's `--assets`) | Three pinned INPUT regions in the mimus manifest at today's addresses. EL3 publication, mapping checks, upload placement and the link check read the region table or the section; the main task authenticates the inputs. |
| The `"mimus"` start entry, `OS_SERVICE_MIMUS` and its 3 s window | A generated application entry and service with `progress_window_us`. `apps/os/prepare.py` and `checks/os-image-link.py` take the expected list from the manifest section. |
| `apps/os/recognition.patch` (capture source) | mimus calls `ss_capture` and writes its WAV header into the headroom. |
| `apps/os/recognition-placement.patch` (worker binding, clock wait, throttled checkpoint, task placement) | mimus places its workers from `ss_grant()`, checks `ss_throttled()` in its checkpoint, and relies on the main task's clock wait. |
| `apps/os/recognition-dispatch.patch` (GPU session, arena assertion, recognition core) | The GPU glue (`dispatch.c`, `gpu_hook.cc`, `placement.py`) moves to the mimus application and runs when the grant contains the GPU. The arena and GPU output limits come from the WORK regions. |
| `apps/os/cpu-return/recognition.patch` (stop workers, retire the GPU, begin the return) | mimus stops its workers and calls `ss_finish(transcript, used)`. The GPU retirement before the return stays in the current GPU service until the layering refactoring replaces it with the GPU compute queue. |
| `apps/os/application.patch` (workload rule, arena and GPU read-only mapping checks) | The generated `pins` hook checks every region, including the GPU output region's EL0 read-only mapping. |
| The 16 KiB transcript buffer `os_cpu_return_output` | `ss_result_record`, sized from `output_limit`. The CPU-return host decodes the payload with mimus's decoder. |
| The fixed `0x46000000`–`0x47000000` arena (`recorded.h`, `terminal.patch`, `cpu-return-host.py`, `dispatch.h`) | Two pinned WORK regions: a 12 MiB arena and a 4 MiB GPU output region. The terminal cleans and the host backs up WORK and OUTPUT regions from the table. mimus uses a 12 MiB arena in every build, since the recorded run peaks below 2.8 MiB. |
| USB0 operations 5–8 in `apps/mimus-board/usb0.c` (fault, NPU, thermal and performance reports) | OS operations `0x85`–`0x88` of the OS host channel. mimus keeps operations 0–4 as its range, answered by a mimus responder task through `ss_host_recv` and `ss_host_send`, so the mimus host frames stay byte-identical. |
| `OS_AUDIO_MIN_SAMPLES` and `OS_AUDIO_MAX_SAMPLES` | `audio.max_frames` (80,000) sizes the capture buffer and bounds `ss_capture`. The per-capture length is mimus's parameter block; the 1 s minimum is mimus policy. |

## Commit sequence

The branches that edit `apps/os/`, `cmake/a733-os.cmake` and the host runner
merge first, together with the step-1 moves (the C++ sections in the base
linker script and `usb_guard` in `tools/`). Each commit names the checks that
must pass unchanged.

1. **Prototype** (this branch): headers, manifest tool, hello-compute, draft
   mimus manifest, `checks/sunstone-app-abi.py`. No image changes.
2. **Manifest section in the OS image:** the base linker script keeps
   `sunstone_manifest`, and `cmake/a733-os.cmake` compiles the generated mimus
   manifest. The link check decodes the section and compares it with
   `assets.h`, `recorded.h` and `dispatch.h`. Checks: `os-image-link`,
   `os-image-identity`.
3. **OS implementation of the calls** in a new `apps/os/app.c`, as wrappers
   of the current functions, with no caller yet. Checks: every OS variant
   compiles and links.
4. **Capture:** mimus calls `ss_capture`; remove `recognition.patch`. Checks:
   `os-image-audio`, `os-image-headset`.
5. **Placement and clock wait:** mimus uses the grant's cores and
   `ss_throttled`; the main task performs input authentication and the clock
   wait; remove `recognition-placement.patch`. Checks: `os-inference-workers`,
   the worker emulator fixtures, `os-staged-recorded`.
6. **GPU glue into mimus:** remove `recognition-dispatch.patch`. Checks:
   `os-image-dispatch`, `os-gpu-return`, `gpu-mimus-layers`.
7. **Result record:** mimus calls `ss_finish`; `os_cpu_return_output` becomes
   `ss_result_record`; the CPU-return host decodes it; remove
   `cpu-return/recognition.patch`. Checks: `os-cpu-return`,
   `os-cpu-return-host`, `os-staged-recorded`, transcript parity with
   `mimus-parity`.
8. **Generated `pins` hook and region table:** EL3 publication, terminal
   cleaning and host backup read the table; remove `application.patch` and
   `mimus_asset_*`. Checks: `os-image-link`, `os-cpu-return`,
   `os-image-identity`.
9. **Generated start list and supervision table;** remove the `"mimus"`
   literal and `OS_SERVICE_MIMUS`. Checks: `os-image-link`, `os-card-bundle`,
   `os-npu-resident`.
10. **Generated interrupt routes;** the return path saves the generated list.
    Checks: `os-image-link`, `os-cpu-return`.
11. **Session extents in the OS and host tools:** the header, device bitmap,
    INPUTS and PARAMS extents, and service records found by type. Checks:
    `os-card-bundle`, `os-card-entry`, `os-contract-profile`,
    `os-staged-recorded`.
12. **Loader:** length, prefix and digest only; drop the `os.h` include.
    Checks: `os-image-stage2`, `os-card-entry`.
13. **Device bitmap replaces workloads** in the OS and mimus, and
    `ss_storage_read` and `ss_key_report` replace the generated
    `mimus-client.c`. Checks: the `apps/mimus-board` checks,
    `os-staged-recorded`, `mimus-usb-session`.
14. **OS host channel:** the USB0 exchange moves into the OS; OS operations
    move to `0x85`–`0x88` with their host decoders; mimus answers 0–4 through
    `ss_host_*`. Checks: `os-staged-recorded`, `mimus-usb-session`,
    `os-thermal-report`, `os-npu`.
15. **OS core target:** `cmake/sunstone-os-core.cmake` and
    `cmake/sunstone-application.cmake`; `cmake/a733-os.cmake` becomes one
    `add_sunstone_application(mimus ...)` and one
    `add_sunstone_image(sunstone-a733-os ...)`, keeping the target name; the
    integration gate configures the core without an application; the
    hello-compute image builds. Checks: every OS variant's link check and the
    integration gate.
16. **Link-check symbol rule** for application objects.

## Costs and measurements

- **Admission:** SHA-256 over 560 bytes per manifest and at most 2 KiB of
  parameters, once per boot.
- **Input authentication:** the same 14.2 MB of mimus inputs, hashed in the
  same 4 KiB steps, in the main task instead of the recognizer. Measure the
  staged recorded run from entry to the first recognition before and after
  commit 5; the times must agree within run-to-run variation.
- **Calls:** each `ss_*` call is a direct function call with atomic accesses
  and no exception entry.
- **Host messages:** a mimus request now passes through the responder task,
  which the USB0 owner wakes with a task notification. A 16 KiB transcript
  takes 16 reads and a few status queries, so a wake-up of a few
  microseconds per request adds well under 1 ms. Measure the host's
  transcript collection time in `os-staged-recorded` and on the board before
  and after commit 14.

## Decisions for the user

- **Several applications in one image.** The design allows it, so that the
  OS NPU client can become an application next to mimus. Limiting this step to
  one application per image removes the per-application extent index and the
  cross-application overlap checks.
- **Host messages through a mailbox.** Application code then never runs in
  the EL1 USB0 owner. The alternative is today's model, in which the USB0
  owner calls the application's copy function. It needs no responder task,
  but a fault or block in application code stops the OS fault and thermal
  reports.
- **OS operation numbers `0x80`–`0xff`.** This moves the fault, NPU, thermal
  and performance reports from 5–8 to `0x85`–`0x88` and changes their host
  tools, while the mimus frames stay unchanged. Keeping 5–8 reserved
  permanently instead leaves applications a fragmented range.
- **Frame magic `0x424d494d`.** It reads "MIMB" in ASCII. Keeping it avoids a
  host-tool change in every decoder.
- **Pinned mimus addresses.** The draft pins today's addresses so that host
  tools and checks keep their bytes. Dropping the pins later moves mimus's
  regions to build-assigned addresses.
- **Capture headroom or a PCM entry point in mimus.** The headroom keeps
  mimus's WAV input; a PCM entry point needs a change in the mimus
  repository.
- **Zero stored epoch for every card session.** The loader requires it only
  for workload 2 today. Confirm that radio and headset card sessions are
  written with a zero epoch before the loader requires it for all sessions.
- **Trusted applications.** The plain-call interface assumes that
  applications are trusted code, as all SunstoneOS code is today. Isolating
  applications would need an EL0 mapping that hides kernel data, which is a
  separate change.
