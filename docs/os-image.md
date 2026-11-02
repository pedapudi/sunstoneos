# Native OS images and applications

SunstoneOS builds a FreeRTOS OS image independently of any recognizer. The
OS owns board startup, session admission, device services, supervision,
interrupt routing, thermal policy and return to the host. An image contains
zero or one statically linked application.

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
  -DSUNSTONE_A733_OS_CPU_RETURN=ON -DA733_A76_ADMITTED=ON -DSUNSTONE_A733_OS_CPU_CLOCK=ON
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

The [A733 hardware facts](hardware/a733-facts.md) record what the hardware presents (GIC views, CPU
identity, NPU admission words) with the view, core and source register of each value. The `hw-facts`
application measures them in one board run, and host models and checks read the file.
