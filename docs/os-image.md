# Native OS images and applications

SunstoneOS builds a FreeRTOS OS image independently of any recognizer. The
OS owns board startup, session admission, device services, supervision,
interrupt routing, thermal policy and return to the host. An image contains
zero or one statically linked application. A session grants the services that
the image links ([services are granted at admission](#services-are-granted-at-admission)).

The application kit consists of `include/sunstone_app.h`,
`include/sunstone_session.h`, `tools/sunstone-manifest.py`, and the CMake
functions `add_sunstone_application()` and `add_sunstone_image()`.
[The application interface](designs/app-abi.md) defines their records and
calls. [hello-compute](../apps/hello-compute/) is the in-tree example: it
returns the Adler-32 checksum of an input region.

Mimus owns its recognizer, manifest, WAV adapter, result protocol, tensor
placement, GPU kernel, preparation tools and application checks under
`sunstoneos/` in the mimus repository. Its `SUNSTONEOS_DIR` selects the kit.
An OS build reads that application only when `MIMUS_DIR` is supplied.
[The optional source record](../apps/mimus/source.json) records its revision
and branch; it is metadata rather than an OS source dependency.

## Build an OS image

The existing host configuration supplies Clang 21, cached lld, FreeRTOS,
TinyUSB, the radio stacks and newlib. Build directories stay outside source.
Run from the SunstoneOS repository root. The optional source record pins
mimus `port/sunstoneos-split` at `c2dcb5a9` in the checkout below:

```sh
os="$PWD"
mimus="$HOME/git/mimus-worktrees/os-refs-1031"
build="$HOME/.cache/sunstoneos/build/docs-current-1031"
cfg=$HOME/.cache/sunstoneos/bin/cfg.sh

"$cfg" "$os" "$build/os" -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON -DSUNSTONE_APPLICATION=
cmake --build "$build/os" --target sunstone-a733-os -j12

"$cfg" "$os" "$build/hello" -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON -DSUNSTONE_APPLICATION=hello-compute
cmake --build "$build/hello" --target sunstone-a733-os-hello -j12

"$cfg" "$os" "$build/mimus" -DMIMUS_DIR="$mimus" -DSUNSTONE_A733_OS=ON -DSUNSTONE_APPLICATION=mimus
cmake --build "$build/mimus" --target sunstone-a733-os -j12

"$cfg" "$os" "$build/control-hello" -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON -DSUNSTONE_APPLICATION=hello-compute \
  -DSUNSTONE_A733_OS_CPU_RETURN=ON
cmake --build "$build/control-hello" --target sunstone-a733-os-hello -j12
```

The last image selects hello-compute with the CPU-return lifetime.
`tools/dev-run.py cpu-return --image "$build/control-hello/sunstone-a733-os-hello.elf"`
prepares it through the generic application interface and normalizes CPU rails
by default. Its fixed default image path still names the cached control build.
The 2026-10-31 board attempt finished the application while the CPU clock owner
was in REQUEST; the merged return path requires FOLLOW and stopped.
[The handoff](handoff.md#board-state-and-recovery) records the failure and open fix.

`sunstone-a733-os-core` is an object library built from the board composition
and OS sources. It does not copy the mimus board target. Application sources
form a separate object target and inherit the OS compilation settings.
Image composition binds the application manifest, source and library inputs,
effective settings, generated tables and OS inputs into the build identity.
The link check verifies the manifest, start and supervision tables, interrupt
routes, image identity and the application's external symbol references.

## Lifetimes

`add_sunstone_image()` selects `supervised`, `resident` or `return`.
The default supervised image uses the bounded board session. The resident
profile sets `SUNSTONE_A733_OS_RESIDENT=ON`; its watchdog owner renews the
lifetime from service progress. `SUNSTONE_A733_OS_CPU_RETURN=ON` selects the
success-only returning profile and its existing retirement requirements.
Faults retain resources unless their owning path proves retirement.

`tools/dev-run.py cpu-return` runs one CPU-return image on one board under
the host-armed watchdog. It builds the typed session from the image's
manifest, uploads the INPUT contents given with `--input`, and decodes the
result with the application's decoder module: `apps/<name>/host.py` for an
application in this tree, or the module given with `--decoder`. For
hello-compute, a run passes when the returned checksum equals the Adler-32 of
the input. `--check-only` prepares the bundle and compares the board's
read-only clock, GPADC and SID state with the image's entry checks without a
launch.

## The session lifetime follows the manifest

An image with an application derives its session lifetime
(`A733_BOARD_SUPERVISED_SECONDS`) from that application's manifest at configure
time ([session_lifetime.py](../tools/session_lifetime.py)):

    lifetime = max(60 s, ceil(1 s startup + 2 s setup + run_us + return_reserve_us))

The 1 s is `A733_BOARD_STARTUP_SECONDS`. The 2 s is an allowance (with the 1 s startup bound, 2.5 times the worst time outside the application measured in the 2026-11-02 runs, 1.20 s; see `tools/session_setup_time.py`) for service
start, application admission and input digests before `main`; an application
that needs longer declares a longer `run_us`. The 60 s floor is the lifetime
of an application that needs less, so its services and the runner's failure
recovery keep their earlier timing. An image with no application keeps the
header's 60 s. The OS session decoder refuses a manifest whose `run_us` ends later
than the lifetime minus `return_reserve_us`; the derivation makes that
impossible, and the image link check
(no host check) refuses an image
whose watchdog marker, manifest and compiled lifetime disagree.

The longest session is 3600 s (`MAX_SECONDS`). No hardware limit sits below it:
the counter and the contract's deadline fields are 64 bits wide, the OS supervisor
restarts the 16 s watchdog interval, and the DRAM backup, the restore and the RTC
breadcrumbs do not depend on elapsed time. What grows with the lifetime is the
thermal reading buffer (56 bytes every 100 ms, sized from the lifetime in
`apps/os/thermal.c`: 2.0 MB at 3600 s) and the time the runner holds a board. The
breadcrumb timestamp wraps after 22,906 s of power-on time
([sunstone_crumb.h](../include/sunstone_crumb.h)). The 600 s limit in the
[probe contract](board-probe-contract.md) applies to raw probe images only.

A test that needs five minutes declares `run_us` of 300 s and gets a 308 s
session. The storage-write test derives its write guard from the session's own
deadline (six seconds before it, for the sync and close), not from a fixed time.

## Admission, startup and memory

The linked `sunstone_manifest` section is the application's authority.
The manifest declares CPU placement, input and working regions, device
requirements, host operations and deadlines. Generated tables supply startup,
supervision and interrupt routing. The OS authenticates INPUT contents before
calling the application main function. No application creates no main task.

A session contains the 592-byte board prefix, a 64-byte header binding the
manifest digest, and aligned typed extents. Device grants replace application
workload policy; the board prefix retains its transitional startup value.
The stage-2 loader checks the opaque session header and board identity,
stamps the prefix epoch and binds the complete session into the contract.
OS admission validates the service records and application grant.

The CPU access bits declare what trusted applications may do. General INPUT
mappings remain writable in hardware; they do not isolate a faulty or hostile
application. The legacy GPU profile separately enforces its CPU read-only
output mapping. [Memory protection](memory-protection.md) records the
system's trust model.

## Services are granted at admission

A session grants services; the image does not fix them. The decoded session
(`struct os_session`) holds the NPU, I2S, CPU clock and DMA records of every
image and a mask, `granted`, of the services that this session carries:

- **I2S capture:** the I2S device bit and the receive record, together. A record
  without the device bit, or the device bit without the record, is refused.
- **Radio station:** the board prefix's workload 1 (and, in a build that has the
  radio, only with USB storage).
- **CPU clock grant and the performance service:** the grant record
  ([cpu-clock.md](cpu-clock.md)), for an application whose manifest declares
  `cpu.clock_grants` (and `cpu.perf` for the performance service). Every image
  has this code; a session without the record reads nothing of the CPU clock tree.
  A cluster on the A76 cores and a grant both need CPU-B on its boot PLL point,
  which the thermal start checks once.
- **NPU, DMA:** the service's record.

The wire form is unchanged: a record is present only for a granted service, and
the thermal record's size field names the board prefix plus the records present.
A service whose code the image does not link cannot be granted. I2S capture, the
radio station, the A76, CPU clock and performance code are in every image. The
headset service, with the Bluetooth stack and mSBC, is a library linked into an
image whose application declares the `audio-headset` device. The NPU service, its
EL0 ring and its diagnostic observer are in every image too: the manifest's `npu`
section grants the service, `in_flight` above one selects the ring, and `observe`
admits the observer in a returning image. `SUNSTONE_A733_OS_RESIDENT` and
`SUNSTONE_A733_OS_CPU_RETURN` decide the rest until each is replaced by its run-time
form ([the design](designs/one-os-image.md)).

A service that is not granted touches no hardware. Its start entry returns before
it authorizes a pin, binds a callback, creates a task or reads a register, the
supervisor requires no progress from it, and the EL3 identity hook reads the radio
profile and the chip ID only for the radio workloads 1 and 3, or for a session
that grants the headset. Every service has its id in every
image (`OS_SERVICE_*`).

The compositions that admission refuses are one table, `refused_compositions` in
`apps/os/grants.c`, each row with its reason: the NPU with I2S capture, the radio
or the headset, I2S with the DMA service, resident CPU grants with I2S or the
radio, and the NPU with CPU grants outside a recorded or performance-mode image.
Image-level rows refuse I2S capture, the radio and the headset in a returning
image (the scanning image keeps the radio), in an image with the USB camera
decoder and in a supervised GPU image.
`checks/os-session-grants.py` lists them and checks the wire sizes.

### Adding a service

The services are declared once, in `apps/os/os-services.h`. Its lists name, for
each service, its grant bit and the build option that links it
(`OS_GRANT_ROWS`), its session record (`OS_RECORD_ROWS`), its entry in the fixed
start list (`OS_START_ROWS`) and the tasks the supervisor watches
(`OS_SUPERVISED_ROWS`). The grant bits, service ids, start list, supervision
table, record decode, wire size and the host session builder's record table
(`apps/os/host.py`) are derived from these rows, and the host models include the
header instead of copying it.

The application ABI numbers stay where applications read them: the device bits
in `include/sunstone_app.h` and the extent types in `include/sunstone_session.h`.
The manifest tool and `apps/os/host.py` parse them from there, and the rows name
them.

A new service is therefore:

1. its device bit or extent type, if it needs one, in the SDK header;
2. a row in each list of `apps/os/os-services.h` that applies;
3. its code: the start entries, the record's field in `struct os_session` and
   its validation in `os_session_valid` (`apps/os/admission.c`), and any rows of
   `refused_compositions` that a shared resource requires;
4. its host check: a session that does not grant it reaches none of its
   registers.

## Host messages and results

The OS owns USB0 and the 64-byte frame header. Operations 5–8 preserve fault,
NPU, thermal and performance reports. Operation `0x80` reads the generic
`ss_result_record`; its 56-byte header binds epoch, generation, status and
payload length. Application payloads are opaque to the OS.

Application requests use a declared operation range. The USB owner wakes the
responder through a task notification. It admits the next request only after
the previous reply completes on USB. Mimus owns operations 0–4 and transcript
decoding. `ss_finish()` publishes the generic result, then ends the task or
starts the image's returning path.

## Capture services

The OS I2S and headset services expose PCM through `ss_capture()`, with 64
writable bytes before the samples for an application container header.
I2S capture uses the admitted exact frame count. Headset capture returns an
actual segment no longer than the immutable profile's bound. Mimus writes
its WAV header in that headroom and processes the same sample buffer.
The generic headset provider validates capture geometry and the complete
profile digest; application model identities belong to the application.

## Thermal service

The thermal owner, CPU clock grants and performance modes remain OS services.
`ss_throttled()` exposes the load hint. The activity and clock-information
calls preserve application thermal-window and clock records without putting
device access in the application. Detailed thermal mechanisms and historical
measurements are in the [service record](os-image-history.md#thermal-service).

## One owner serves the AIC8800 module

The radio and headset profiles share the existing radio owner and firmware
load. The OS owns UART, SDIO, Wi-Fi and Bluetooth device service state.
[The shared-owner record](os-image-history.md#one-owner-serves-the-aic8800-module)
retains the supported wiring, firmware and scheduling constraints.

## Performance-mode service: USB0 operation 8

Performance-mode requests remain OS operation 8. CPU, GPU and NPU owners
retain their existing resource coordination and failure bounds.
[The service record](os-image-history.md#performance-mode-service-usb0-operation-8)
contains the operating-point and protocol details.

## Supervised preparation and launch

`apps/os/prepare.py` produces an offline manifest-bound input package and typed
session. It marks that package as requiring launch admission. It grants no
physical authority and launches no device. Application-specific preparation
and capture, including mimus's full reviewed board admission, live with the
application. The board coordinator must validate those inputs and own the
board lock before execution.

## First bounded launch with recorded audio

Mimus's recorded, storage and repeated keyboard modes use its external
application and host tools. Physical evidence applies to the exact frozen
images recorded in [the mimus evidence](mimus-evidence.md) and the handoff.
Compilation and emulation do not establish physical operation of the split.

## Checks

The generic ABI, native runtime, typed-session and image-link checks live in
SunstoneOS. Recognition, application protocol, placement and model checks
live under mimus's `sunstoneos/checks/` and `sunstoneos/apps/`.
[Validation policy](validation.md) selects checks by changed behavior.
The integration gate builds OS variants with an empty `MIMUS_DIR`; it adds
external application checks with `--application CHECKOUT` or the
`SUNSTONE_EXTERNAL_APPLICATIONS` environment variable. The optional
`MIMUS_DIR` compatibility selector names the mimus example checkout. `--build-root` keeps a task's new builds and logs together.

## Not yet integrated

The generic GPU compute queue remains a reviewed design with prototype
headers. Mimus's GPU path requires both `SUNSTONE_A733_OS_GPU_LINEAR=ON` and
`SUNSTONE_A733_OS_LEGACY_MIMUS_GPU=ON`. The compatibility implementation still
includes `apps/os/gpu.c`, `drivers/gpu/pvr_mimus_service.c`,
`drivers/gpu/pvr_compute_mimus.c`, `drivers/gpu/pvr_compute_mimus_usc.c`,
`platform/a733/gpu_mimus.c` and their `include/*mimus*.h` interfaces.
The generic GPU and NPU drivers stay in SunstoneOS. Replacing this session
with [the compute queue](designs/gpu-compute-queue.md) remains separate work.

No new board qualification is claimed. Board recovery, accessories and
performance requirements remain in the hardware matrix and handoff.

## Hardware facts

The [A733 hardware facts](hardware/a733-facts.md) (and [the probe](hardware/a733-facts-probe.md)) record what the hardware presents (GIC views, CPU
identity, NPU admission words) with the view, core and source register of each value. The `hw-facts`
application measures them in one board run, and host models and checks read the file.
