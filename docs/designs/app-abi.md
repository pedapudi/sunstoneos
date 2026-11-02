# SunstoneOS application interface and OS core build

**Status: implemented.** One primary application with an optional fixed GPU
peer, or no application. Host-tested; the board-qualified application paths are
in [the application status](../status/application.md).

An application on SunstoneOS is code linked into the OS image together with a
manifest: a fixed-layout record that declares the devices, CPUs, memory,
deadlines and host-message operations that the application needs. The OS
builds its start list, supervision table, interrupt routes and memory checks
from the manifest, and admits a session only when the session grants what the
manifest declares. Applications reach the OS through `ss_*` functions. The OS
core contains no application; external applications such as the mimus speech
recognizer build against this kit ([layering audit](../layering-audit.md)).

An image holds zero or one primary application. A primary GPU application may
also admit one trusted GPU application through an immutable `ss_gpu_share`
descriptor. The peer has its own manifest, task and GPU-only grant. Both
applications share one program and identical region records. The
primary manifest remains the session, input and result authority. General
composition with separate input, result or host-operation records is
unsupported.

## The kit

- `include/sunstone_app.h`: the manifest layout and the application calls.
- `include/sunstone_session.h`: the session, host frame and result layouts.
- `tools/sunstone-manifest.py`: generates the manifest section and the
  application's function prototypes from `manifest.json`, and decodes the
  section again.
- `apps/hello-compute/`: an application in this repository that returns the
  Adler-32 checksum of an input region.
- `checks/sunstone-app-abi.py`: compiles hello-compute and a synthetic
  manifest for the host and for `aarch64-none-elf`, compares their sections
  with the generator, runs hello-compute at input boundaries, and checks
  malformed manifests. Repeatable `--manifest PATH` arguments add external
  manifests. Application-specific input and runtime-bound assertions live in
  each application repository.
- The CPU-return launch path for any application: `apps/os/prepare.py` builds
  the SRAM launcher, launch request and contract, `apps/os/cpu-return-host.py`
  reads the return layout and performs one returning run, and
  `tools/dev-run.py` prepares the typed session from the image's manifest and
  runs the image. Its default image is hello-compute built with the CPU-return
  lifetime.

## The manifest declares what an application needs

Each application has a `manifest.json`. At configure time,
`tools/sunstone-manifest.py` validates it, assigns addresses to regions that
do not pin one, and generates a C file that places a 552-byte
`struct ss_manifest` in the section `sunstone_manifest`, and `<name>-app.h`
with the prototypes of the application's functions and its region indices.
CMake reads the JSON form before anything is compiled; the section is the
authority for a linked image.

| Field | Meaning |
| --- | --- |
| `name` | 1-15 characters `[a-z0-9-]`. The application provides `<name>_main(const struct ss_grant *)` and optionally `<name>_init(void)`, with `-` written as `_`; the generated header declares both. |
| `devices_required`, `devices_optional` | Device bitmaps: USB storage, USB keyboard, I2S capture, headset capture, GPU, USB mouse, USB serial, USB camera. The session grants every required device and any subset of the optional ones, with at most one capture device. USB0, the host channel and thermal supervision are always present. The radio station and the NPU client are OS services that the image selects. |
| `cluster`, `workers` | Preferred cluster of the main task and the number of worker tasks. The main task gets the first core of the cluster's pair: core 6 for the A76 pair, or core 2 for the A55 pair when the A76 cores are not admitted. The first worker gets the second core of that pair, and further workers the remaining cores in ascending order. |
| `priority`, `stack_bytes`, `heap_bytes` | FreeRTOS priority and stack of the main task, which runs at EL0, and the bytes the application adds to the image's C and C++ heap. A heap above 4 MiB is a region of upper memory, zero filled as the allocator takes it ([application-memory-layout.md](application-memory-layout.md)). |
| `regions[8]` | Physical ranges with kind, access and digest. INPUT regions are uploaded by the host and must not be written by the trusted application; `bytes` is the maximum content length, or the exact length when the manifest pins a SHA-256. WORK regions are private working memory; OUTPUT regions are written by the application and read by the host after the run. Access bits are CPU read, CPU write, GPU read, GPU write and scratch (work or output that the board runner does not back up or restore). A WORK region with GPU write and without CPU write is a GPU output region that the CPU only reads. Unpinned regions are placed on 2 MiB boundaries in the application window `0x44000000`-`0x47000000`. An optional `sunstone_region_initializers` section names a zero prefix that EL3 clears and publishes in a GPU-written WORK region. |
| `audio` | Rate, channels, sample bits, and the minimum and maximum frames of one capture, when a capture device is listed. |
| `run_deadline_us` | Longest time from the start of `<name>_main` to `ss_finish`. Zero means the session deadline less the return reserve bounds the run. |
| `return_reserve_us` | Time between the latest permitted `ss_finish` and the session deadline, which the return path uses. |
| `progress_window_us` | Longest permitted gap between `ss_progress` calls from any task of the application. A supervised session ends within 60 s (board admission refuses a longer one), so the generator bounds the window by 60 s there; a resident image (`SUNSTONE_BOARD_RESIDENT`, `emit --resident`) has no session deadline and its window is bounded only by the 64-bit supervision table field. |
| `output_limit` | Maximum bytes of the `ss_finish` result. The image reserves `56 + output_limit` bytes for the record, so the only bound is the image RAM window `0x41000000`–`0x41f00000` that holds it; the link and the host's result geometry check refuse an image whose record does not fit. |
| `host_op_first`, `host_op_count` | The application's host-channel operation numbers, inside `0x00`-`0x7f` and outside `0x05`-`0x08`. The OS owns `0x05`-`0x08` and `0x80`-`0xff`. |

The OS image derives its session lifetime from `run_deadline_us` and
`return_reserve_us` ([os-image.md](../os-image.md#the-session-lifetime-follows-the-manifest)).
No manifest can exceed it.

The generator refuses a manifest that breaks these rules. Examples are an INPUT with write access, overlapping regions or a region outside the window; a
host range that meets an OS operation. It also refuses a required capture device listed
together with the other capture device, a capture device without a format, and an unknown device or CPU key.

CPU access flags are declarations for trusted applications. General INPUT
regions retain writable hardware mappings; only the GPU output profile
enforces a separate read-only CPU mapping. Per-region CPU protection is not
implemented.

## Host tools and admission read the manifest section

The base linker script keeps `sunstone_manifest` in the read-only image block
between `__start_sunstone_manifest` and `__stop_sunstone_manifest`.

- **Host tools** read the section from the ELF image with the decoder in
  `tools/sunstone-manifest.py`. The upload step takes INPUT addresses, sizes
  and pinned digests from it. The return step takes the WORK and OUTPUT ranges
  to back up and the result size. The session writer takes the SHA-256 of the
  section and the device rules.
- **The image link check** decodes the section and compares it with the
  generated start list, service table, interrupt routes and region table. It
  also refuses an application object that references a symbol outside its own
  sources, the `ss_*` calls, FreeRTOS and the C and C++ libraries.
- **OS admission** runs once, before the start hooks. It computes the SHA-256
  of the section and compares it with the session header, so a session
  prepared for another build of the application is refused. It then checks the
  session against the manifest.

## The session is a board prefix and typed extents

The session page at `0x41f00000` holds up to 4,096 bytes, the size of the
sealed copy in `apps/board/session.c`.

| Offset | Bytes | Contents |
| --- | --- | --- |
| 0 | 592 | `struct a733_board_session`, the board prefix |
| 592 | 64 | `struct ss_session_header`: magic, version, total bytes, extent count, granted device bitmap, flags, SHA-256 of the manifest section |
| 656 | variable | Extents: an 8-byte `{type, bytes}` header and its payload, each padded to 8 bytes |

Extent types (`include/sunstone_session.h`):

- **OS service extents**, found by type: thermal facts (1), audio receive
  facts (2, whose `capture_samples` is the capture length), NPU client facts
  (3), the CPU clock grant (4), the JPEG session (5), the USB memory
  reservation (6) and the adopted DMA0 service facts (7).
- **INPUTS** (16): a length and SHA-256 for each INPUT region, in manifest
  order.

A session with NPU, CPU grant and three INPUT regions is 1,392 bytes.

**Stage-2 loader.** The loader includes no OS header and performs these steps:

- check that the slot's session extent length equals the header's `bytes` and
  lies between 656 and 4,096 bytes (refusal detail 1);
- check the prefix magic, version, generation and lease, which the contract
  copies (detail 2), and the header magic and version;
- check the SID digest when the header's `SS_SESSION_BIND_SID` flag is set
  (detail 4);
- stamp the prefix epoch, overwriting any stored value, then bind the length
  and SHA-256 into the contract.

It reads no extent and no workload number, and does not write into the CPU
grant. A malformed grant or extent therefore enters the image, where contract
admission stops with status 1, detail 1 while WDT0 is armed, so recovery is
the watchdog reset. The card writer runs the same session check offline, so a
card with a malformed grant is refused before it is written.

**OS admission** checks:

- the board prefix with the board validator, and the header magic, version,
  length and manifest-section digest;
- that the extents are in bounds and padded, and that each type appears at
  most once;
- every OS service extent with its validator. For the CPU grant, admission
  copies the extent, stamps the prefix epoch into the copy when the grant
  names clusters, and calls `a733_cpu_clock_grant_load`; a grant without
  clusters keeps the zero epoch that the function requires;
- that the granted devices contain the required devices, lie within the
  required and optional devices, and hold at most one capture device;
- that an image without the USB class services grants no mouse, serial or
  camera device and grants a keyboard only with USB storage;
- that a grant without USB storage has no capture device, no radio service and the
  `SS_SESSION_BIND_SID` flag (the recorded mode without storage, which the
  loader binds to the board's SID words);
- that the audio extent's `capture_samples` lies within the manifest's
  `min_frames` and `max_frames`;
- that the INPUTS extent has one entry per INPUT region, with each length
  within its region and each digest equal to the manifest's pinned digest
  where one exists;
- for finite lifetimes, that the session deadline leaves the run deadline and
  the return reserve.

The board prefix keeps a `workload` word that must agree with the device
grant (`apps/os/session.c`). It is 0 for storage, 4 for storage with keyboard, 2 for
no storage, 0 for any USB class device in an image with the class services,
and 1 in an image with the radio service. Applications derive their mode from
the grant, not from this word.

## Applications call the OS through plain functions that are safe at EL0

FreeRTOS tasks on SunstoneOS run at EL0 unless the image selects EL1, and EL0
tasks call kernel functions directly; kernel data, stacks and heap are mapped
read-write at EL0 ([memory protection](../memory-protection.md)). Each call
is an atomic store or load in shared memory, with an EL1 owner task doing the
device work. A system-call trap for each call would add an exception entry and
exit and would isolate nothing while EL0 can write kernel data. Applications
are trusted code, and the calls check no ownership; device calls enforce
grants.

| Call | Behaviour |
| --- | --- |
| `ss_grant()` | Immutable record of the grant: devices, main core, worker cores, region spans (actual INPUT lengths), capture length and the finish-by time. |
| `ss_live()` | 1 when the session is live and the granted devices are ready, 0 while they are starting, -1 once the session has ended. |
| `ss_progress()` | One atomic store into the application's supervision record. |
| `ss_throttled()` | Thermal load hint. |
| `ss_clock_ready()` | 1 once the CPU clock owner has finished the grant's requests, or when the image grants no CPU operating points. |
| `ss_capture(sequence, &pcm, &bytes)` | PCM in the manifest format, with `capture_frames` exact for I2S and an upper bound for headset segments. The 64 bytes before the samples belong to the caller, so an application writes a WAV header in place without a copy. |
| `ss_host_recv(&request, wait_us)`, `ss_host_send(&reply)` | A mailbox for one 16-byte request in the application's range, and its reply of up to 1,024 bytes plus a 16-byte application tag. The USB0 owner wakes the waiting task with a task notification. It passes the next request only after the USB transfer of the previous reply completed, so a request also confirms delivery of the previous reply. The OS sends nothing for an application request until the application replies. |
| `ss_storage_read(sector, data)`, `ss_key_report(&report)` | One 512-byte read from the USB storage leaf, and the next boot-keyboard report, both served by the OS USB2 owner. |
| `ss_npu_model_count()`, `ss_npu_model_info(index, &model)`, `ss_npu_model_find(name, &index)` | Discover immutable image-linked model names, tensor counts and concatenated byte sizes. Find resolves a semantic name to the image-local index. |
| `ss_npu_run(model, input, input_bytes, output, output_bytes, timeout_us)` | Submit and poll one image-linked NPU model through ordinary memory and atomics. Identical repeated calls return 0 while pending, 1 after copied output, -2 for refused work, or -1 when unavailable or failed. |
| `ss_npu_close()` | Submit and poll CLOSE after the final consumed result. The application must close before finishing. |
| `ss_finish(result, bytes)` | Copies the result into the OS result record and ends the application. A returning lifetime then returns to the host; the resident lifetime keeps the record readable over the host channel. |
| `ss_fail(detail)`, `ss_fail_value(detail, value)` | Record the first application failure and end the session. In the returning lifetime the board then stops every core and returns to the ROM with the failure record ([failure return](failure-return.md)); other lifetimes end at the watchdog. An application that owns an NPU client closes it when `ss_live()` turns -1. |

The header also declares calls for large-region access (`ss_large_*`), CPU
topology, work splitting and observations (`ss_cpu_topology*`,
`ss_work_split*`, `ss_cpu_observations`), thermal status, counter ticks,
activity, clock and heap-peak telemetry, NPU graph registration, fault
information and the NPU diagnostic observer. The header comments define them.

The NPU service is selected by the image. Application buffers remain borrowed
until the request completes; a failed request retains them for that session.
Unknown model indices and incorrect byte extents are refused before posting a
request. Input and output are concatenated tensors in model order, each
direction at most one MiB. `timeout_us`, at most one second, bounds hardware
SUBMIT. The session bounds the mailbox and LOAD; OS send and collect each use
a two-second wait bound. [The SDK diagnostic](../../apps/npu-sdk/README.md)
records the owner, failure-race and privilege checks and their physical
limits.

`ss_fail()` calls `board_fail_at()` with the application failure group, the
detail and the caller's return address. SVC `0x5c0e` preserves that record
through watchdog reset and through the failure return. [The watchdog
record](../watchdog.md#rtc-words-retain-progress-and-the-first-failure)
defines the fields. `checks/harness.py` steps over that SVC in function-level
models without exception vectors.

The mailbox keeps application code out of the EL1 USB0 owner: a fault or block
in application code cannot stop the OS fault and thermal reports.

**Main task.** The OS creates the main task from the manifest. Before it calls
`<name>_main`, the task waits for `ss_live() == 1` and authenticates every
INPUT region in 4 KiB steps with progress reports. The CPU clock wait is the
application's. Returning from `<name>_main` is a failure.

**Result record.** `struct ss_result` holds magic, version, record size,
epoch, generation, status and payload length, followed by the payload. The
image reserves one record of 56 bytes plus `output_limit` in the symbol
`ss_result_record`. OS operation `0x80` reads that record. The application has
a host-side decoder for its payload. A failed result carries a `struct
ss_failure` payload written once for the first failure.

**Host channel.** The OS owns the USB0 exchange and its 64-byte frame header,
including the magic `0x424d494d` and the storage-state word at offset 24. The
fault, NPU, thermal and performance reports use operations 5-8; OS operations
take numbers from `0x80`. The OS fills the frame header; the application
supplies the version word, state, total, tag and payload of its own replies.

## The build composes an OS core with one application

- **`sunstone-a733-os-core`**: an object library with the board composition
  (the board sources without the board's USB0 exchange, USB2 client,
  identity hook, admission and supervised boot). It also contains the OS services, admission,
  start, supervision, thermal and watchdog code, the interrupt table and fault
  capture, and the C and C++ runtime. Each device service (I2S capture,
  headset, GPU) and each selectable OS service (radio station, NPU client) is
  a separate object library.
- **`add_sunstone_application(name SOURCES ... MANIFEST ...)`**: an object
  library of the application's sources, compiled with the core's flags and the
  generated `<name>-app.h`, and the path of its manifest. The manifest passes
  through `configure_file`, so a field may name a CMake variable.
- **`add_sunstone_image(target LIFETIME supervised|resident|return APPLICATION name)`**:
  the image. It runs the generator over the manifest and the OS device table,
  links the core, the device services that the manifest lists and the
  application, and adds the manifest and the application sources to the build
  identity. An image is a choice of lifetime and application; the session grants
  the OS services, the NPU among them.

```cmake
add_sunstone_application(hello-compute
  SOURCES apps/hello-compute/main.c
  MANIFEST apps/hello-compute/manifest.json)
add_sunstone_image(sunstone-a733-os-hello LIFETIME return APPLICATION hello-compute)
```

The generator writes, from the manifest and an OS-owned device table:

- **Start list:** the OS service entries, then the application's entry with
  generated `task_privileged`, `pins` and `start` functions. `pins` checks the
  region mappings from the region table; `start` calls `<name>_init` and
  creates the main task.
- **Supervision table:** board services, then the application's service with
  its `progress_window_us`, then the OS services.
- **Interrupt routes:** each device and OS service row of the device table
  names its INTID, handler and core rule. Thermal 71 and I2S DMA 143 go to
  core 0, NPU 97 to the NPU core, and GPU 95 to the application's main core.
  The return path saves the generated routes and no others.
- **Region table:** address, size, kind and access of every region. The `pins`
  hook checks it; EL3 publication cleans INPUT regions; the return terminal
  cleans WORK and OUTPUT regions; host tools back up WORK and OUTPUT regions.
- **Image settings:** the service compile definitions that the OS options set,
  and the heap size.

## Two trusted GPU applications share one admitted memory map

`ss_gpu_share` names the primary manifest, peer manifest, peer entry, shared
program descriptor and the mask of shared GPU regions. Admission runs before
scheduling. It checks every region record against the primary, requires an
existing GPU grant and consumes one granted worker core for the peer. The peer
cannot add devices, regions, inputs, heap, workers or host operations. Its
stack is bounded by the OS's fixed 16 KiB storage.

The peer calls `gpu_queue_context_join` after the primary creates the context.
Each task receives its own client handle, submission slot and completion
queue. Only the primary creates the context or controls device-wide close,
open and recovery waits. A task cannot use the other task's handle. Each
application keeps its job's buffers stable until its own terminal record
arrives. Concurrent mutable ranges must be disjoint or synchronized by the
applications.

Returning from the peer entry drains its owed record before marking it
finished. The primary's `ss_finish` waits for peer completion within the
admitted absolute deadline, quiesces GPU work and then publishes the result.
Resident completion also quiesces the GPU before suspending. Failed isolation
retains memory and prevents successful completion. The composition supplies no
isolation between trusted applications sharing the GPU address space. The
[generic GPU compute queue](gpu-compute-queue.md) admits application programs
and region leases and serializes work from the primary and its peer.

## External checkouts supply their build hooks and checks

`SUNSTONE_EXTERNAL_APPLICATIONS` is a CMake list of external checkout paths.
Each checkout supplies `sunstoneos/cmake/register.cmake`, which sets
`SUNSTONE_APPLICATION_<name>_ROOT` to its port directory. The OS includes
optional `cmake/engine.cmake` before its board targets and optional
`cmake/targets.cmake` after them. When `SUNSTONE_APPLICATION` selects the
registered name, the OS includes the port's `cmake/application.cmake` and
links that application through `add_sunstone_image()`.

An empty application selection builds only OS services. The in-tree
hello-compute application uses the same interface. Optional compatibility
variables come from records in `apps/*/source.json` that contain
`application_directory`; provenance-only records define no selector. The
selector is the uppercase directory name with hyphens replaced by underscores,
followed by `_DIR`; `MIMUS_DIR` selects the external mimus checkout. An OS-side
source record may carry a `dashboard` object with `title` and `document` for
the progress view (`tools/progress.py`).

The integration gate accepts repeatable `--application PATH` arguments or the
semicolon-separated `SUNSTONE_EXTERNAL_APPLICATIONS` environment variable, and
the optional compatibility variables. Each selected checkout supplies
`sunstoneos/gate.json` with its name and checks:

```json
{
  "name": "example",
  "checks": {
    "example-model": ["example-model", [
      "{application}/sunstoneos/checks/model.py", "--image", "{build}/example-model.elf"
    ]]
  }
}
```

Each check declares a build target and an argument list. The gate substitutes
`{application}`, `{build}`, `{cache}`, `{qemu}` and `{evidence}`. Selected
checks build their application's image and targets in a separate build
directory; `--full` builds every selected application. Core builds clear all
external selectors. Application names and check names must be unique, and the
gate records and rechecks every selected checkout's Git state.

### Device composition hooks preserve dependency and manifest identity

A registered port may define the following CMake functions. The OS calls them
while composing the I2S and Bluetooth diagnostic images. `kit_root` is the
absolute SunstoneOS source directory; `build_directory` is the image's private
generated-file directory. Every output name is a caller variable set with
`PARENT_SCOPE`. A hook returns an empty list when the application's feature is
not selected.

| Hook | Contract |
| --- | --- |
| `sunstone_external_i2s(target kit_root build_directory out_sources out_inputs out_identity)` | Called after the I2S target exists. Configure application libraries, includes and options on `target`. Return application source paths, all files that affect the application image, and a stable feature-selection string. The OS adds the sources and hashes the inputs and identity string. |
| `sunstone_external_bluetooth_prepare(kit_root build_directory out_sources out_inputs out_manifest_arguments)` | Called before the Bluetooth target exists. Generate application headers under `build_directory`. Return sources, dependency paths including generated files, and arguments for the OS Bluetooth manifest generator. This phase must not modify a target. |
| `sunstone_external_bluetooth_target(target kit_root build_directory)` | Called after the Bluetooth target exists. Configure application libraries, include paths, compile definitions and link options on that target. The prepare phase must already have returned every affected input. |

Hooks receive their OS inputs through the arguments and the documented kit
functions. They must not read or write the caller's `i2s_*` or `bluetooth_*`
variables, or load an OS-side record for one named application. They use their
own source records for application provenance.

`cmake/application-kit.cmake` provides `sunstone_device_cxx(target)`, which
adds the maintained SMP C++ runtime, headers, archives and reentrancy
settings, and `sunstone_device_cxx_inputs(out_inputs)`, which returns the
runtime sources, archives and CMake files that must enter image identity. The
OS includes these helpers before invoking a device hook. The dependency
options `NEWLIB_ROOT`, `NEWLIB_SMP_ROOT`, `SUNSTONE_BT_SECURITY_DIR` and
`SUNSTONE_BT_SBC_DIR` name pinned source or toolchain roots, and
`SUNSTONE_BLUETOOTH_MSBC` selects the wideband provider.

### Session hooks validate and publish bounded inputs

A port that supplies a session header for the storage-derived launch path
defines `SUNSTONE_APPLICATION_SESSION`, which enables the boot hook
`sunstone_application_session_prepare(const uint8_t *session, uint64_t deadline)`.
The hook may be a macro or a static inline function visible through that
header. The OS calls it after publishing the immutable session page and before
cached CPU startup. The deadline uses the architectural counter's tick units.

The hook validates its application fields and completes publication of
admitted application inputs before the deadline. It returns nonzero on success
and zero on refusal or publication failure; zero stops boot with code `0x309`.
It starts no device work and grants no storage writes. Without the selector,
boot runs only the common OS session path.

An application-specific SRAM request additionally defines
`A733_APPLICATION_REQUEST_MAGIC` and `sunstone_application_wire_valid(session)`.
The launcher checks that request identity and validator before entry
(`platform/a733-storage-launch/launch.c`). The validator returns nonzero only
for a supported, bounded immutable session; its failure uses the launcher's
ordinary refusal path.

### Preparation takes an explicit application admission callback

The Bluetooth preparation functions accept the keyword callback
`application_admission(record, profile, manifest, image_hash, cache, frozen)`.
The OS first validates the common session, profile, firmware spans, provider
identities and evidence. The 832-byte application profile then requires the
callback, which validates application-specific memory reservations, input
extents and hashes, and raises on refusal; returning normally means success.
It must not communicate with a device.

The cache maps admitted absolute evidence paths to immutable bytes. When
`frozen` is true, the callback uses only those cached bytes and refuses a
missing input. When false, it may read a declared input, verify its digest and
retain its bytes in the cache. The same callback goes to `prepare()` and
`validate_bundle()`. The OS refuses an extended application profile without a
callback.

CPU-return clock validation requires the explicit keyword `input_reservation`
naming the application's reviewed DDR record. The generic validator supplies
no application field name.

## USB input, serial and camera services

Applications request `usb-keyboard`, `usb-mouse`, `usb-serial` and
`usb-camera` independently in their manifest. Images select these services
with `SUNSTONE_A733_OS_USB_CLASSES=ON`. A class grant does not grant USB
controller register access. The USB owner task receives interrupt completions
and application request notifications, and dispatches class work in task
context.

`ss_input_event` copies keyboard changes and pointer motion or button changes
into an application event. `ss_key_report` remains available for applications
that consume the eight-byte keyboard compatibility report. An application can
consume events only for its granted classes.

`ss_usb_serial_acquire` returns a read-only receive buffer.
`ss_usb_camera_acquire` returns a read-only list of frame slices. Payload bytes
stay in the OS-owned USB transfer buffers. The application releases each lease
token with the matching release call after its last read. The owner task
processes release requests before rearming the buffers. Tokens increase across
attachments; repeated or stale releases fail.

`ss_usb_serial_write` borrows the application's immutable buffer. A successful
submission retains the buffer until `ss_usb_serial_write_result` reports
actual completion or failure after controller drain. Disconnect alone does not
release a pending write. `ss_usb_serial_configure` sets baud rate, data bits,
parity, stop bits and flow control through the upstream class driver's
bridge-specific requests; the serial demo configures 1,000,000 baud, eight
data bits, no parity and one stop bit before receiving.

Class calls return one for a result, zero while pending and minus one on
refusal or disconnect. `ss_usb_wait` sleeps until a class completion or
timeout; interrupt completions and owner-task publications wake the
application. Disconnect retires queued unread leases and input events; held
leases remain readable until explicit release. The demos `usb-input`,
`usb-serial` and `usb-camera` each have their own manifest and build without
external application sources. The first returns its first input event, the
second echoes one receive buffer and returns the receive and transmit byte counts. The third checksums a frame in place, releases it and returns its
sequence, byte count, checksum and dimensions. The software ownership contract
does not establish physical DMA drain or hotplug qualification.

`SUNSTONE_A733_OS_USB_JPEG=ON` adds one hardware MJPEG decode to the camera
demo. The session supplies JPEG extent type 5, a complete 856-byte
`a733_jpeg_session` with version 4 and the matching board prefix, which binds
reviewed electrical, initial-isolation, memory and supervised recovery
evidence. Fixture and reference hashes are zero because the camera supplies
runtime content. The OS validates the vendor's 624 MHz operating point at
800 mV and distinct resource owners before accepting the extent.

`ss_usb_camera_decode` retains the frame lease while the OS gathers its slices
once into the decoder's contiguous input. The OS reuses the native JPEG
platform, IOMMU setup, command executor and retained output checks, and returns
packed active planar Y, Cb and Cr bytes in a private retained buffer. It
permits one decode per image; completion retains the decoder resources and does
not establish DMA drain or authorize a return to ROM.

USB class images reserve a 16 MiB non-cacheable pool at
`0x47000000..0x48000000`. Session extent type 6 carries four little-endian
64-bit words (version 1, physical base `0x47000000`, size `0x01000000` and
application alias base `0x7a000000`) followed by a 32-byte reservation-evidence
hash. The extent identifies reviewed physical ownership and remote-writer
exclusion for the additional RAM range under the board's memory authority; the
boot-image DDR record alone does not establish that authority. The sealed
session validates the extent before the MMU maps the pool.

The USB host's first 5 MiB allocation starts at the pool base. Only its shared
payload pages, at offsets `0x40000..0x440000`, have a read-only application
alias. Controller rings, contexts, scratch space and later JPEG allocations
remain inaccessible through that alias. The OS identity mapping permits writes;
both aliases use identical non-cacheable attributes. The native serial owner
translates a retained receive view back to its OS address for a zero-copy echo
transfer. A trusted application may process only its own completed leases;
grants control service admission and give no per-class confidentiality against
an application that guesses another payload address.

## Recorded PCM can use the capture interface without a device

An application can declare an audio format and an authenticated INPUT region
without requesting a capture device. The image reserves capture storage from
the manifest's maximum frame count. `ss_capture_replay_open(region, offset,
frames, paced)` selects raw PCM in that region. The offset can skip a container
header; the service does not parse containers or convert samples.

The source format is the manifest's sample rate, channel count and signed
little-endian sample width. Replay accepts one or two channels and 16-, 24- or
32-bit containers. The region length after the offset contains whole frames,
and the requested window fits the source, capture storage and manifest frame
bounds. A granted I2S or headset device excludes replay. Opening replay changes
no device ownership and is allowed once per image lifetime.

One application task then calls `ss_capture` with consecutive sequences
starting at 1. Success returns one copied window in the ordinary capture
buffer, with 64 writable bytes before the PCM. Input bytes remain unchanged.
Repeating a completed sequence returns the same output. Requesting the next
sequence releases the earlier output; stale sequences are refused even while
pacing is pending. EOF returns -1 without changing the output arguments. A
shorter final window succeeds only when it meets the manifest's minimum frame
count.

With `paced=1`, the first request starts a recording clock. A window becomes
available after its cumulative frame duration, rounded up to the architectural
counter tick. Later windows follow that same clock, so a late consumer catches
up without adding drift. Calls return 0 while waiting; applications use
`ss_progress` and `ss_usb_wait` between polls. A counter regression retains a
failed replay. With `paced=0`, copied windows are available immediately.

## Validation and limits

- `checks/sunstone-app-abi.py`: manifest layout, generator and decoder
  agreement, malformed manifests.
- `checks/os-audio-replay.py`: the production capture implementation compiled
  without I2S or headset support, under AddressSanitizer,
  UndefinedBehaviorSanitizer and AArch64 instruction emulation. No host check
  executes the linked instructions of a generic application with this capture
  path.
- Admission costs one SHA-256 over the 552-byte manifest per boot. Input
  authentication hashes each INPUT region in 4 KiB steps in the main task
  (14.2 MB for the mimus inputs). Each `ss_*` call is a direct function call
  with atomic accesses and no exception entry.

Layout figures and host-check results are records in [app-abi.jsonl](../evidence/app-abi.jsonl).

## Open work

1. Time the staged recorded run from entry to the first recognition, and the
   host's 16 KiB transcript collection (16 reads), on the board; both
   predictions are recorded as not run.
2. Enforce per-region CPU protection; CPU access flags are declarations.
