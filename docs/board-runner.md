# Run and queue A733 images

`tools/board.py` owns one physical port from identity checking through result
capture. The host implementation has fake-board and offline-image checks.
Physical qualification remains the coordinator's responsibility. Workers submit
images without opening a USB device.

`tools/dev-run.py` is a compatibility wrapper around this runner. Its existing
`cpu-return`, `gpu-return`, `npu` and `rail-helper` commands remain available;
`usb-class` selects the native USB probe route described below.
`--state` always selects offline preparation; force and skip-normalization
options cannot bypass admission. Offline application checks use `board.py check
IMAGE --kind cpu-return --state CAPTURE --evidence OUTPUT`; native probes keep
the default `probe` kind. The [admission inventory](launch-admission.md) lists
retired transports, retained hardware checks and manifest-generated READMEs.

Use `~/.cache/sunstoneos/venv/bin/python3 -B tools/board.py` in this checkout.
The examples abbreviate that command as `$board`:

```sh
board="$HOME/.cache/sunstoneos/venv/bin/python3 -B tools/board.py"
$board serve
```

The daemon runs at most one entry per board. The two ports can run concurrently.
A second daemon refuses the daemon lock. Every device command uses the existing
per-port lock and checks the selected instance; an outside runner holding that
lock leaves queued work pending.

## Submit a frozen image and hypothesis

```sh
$board submit /absolute/image.elf --kind cpu-return --port any \
  --worker cpu-integration \
  --hypothesis 'The corrected cache cleanup permits all cores to retire' \
  --expect /absolute/expected-output.bin
$board status
$board watch
```

`submit` copies the image, inputs, session template and expectation into
`~/.cache/sunstoneos/board-queue/entries/`. It blocks until a result exists and
prints its JSON path. `--no-wait` prints the future result path immediately.
`watch ENTRY_ID` waits for that entry; `watch` prints status when it changes.
`--port 3-1` or `--port 3-3` fixes the target. `any` selects the first eligible
board, respecting submission order among entries eligible for that board.

An existing `--expect` file requires an exact byte match, unless it declares
`schema: sunstone-board-expect-u32-v1`. That JSON format has a `words` array
covering the entire little-endian 32-bit payload. Each entry is either an exact
unsigned word or an inclusive `{"min": 0, "max": 199999}` range for a measured
hardware duration. Missing words, extra bytes and malformed ranges are refused.
The runner validates both original files and frozen queue specifications before
device access. Other expectation text requires a substring in the UTF-8 output.
The queue freezes the expectation at submission. `--input FILE` supplies each manifest input in order. Without an
input argument, hello-compute receives `SunstoneOS board control` followed by a
newline. Its result is the little-endian Adler-32 checksum, a zero reserved
word and the input length, packed as `<IIQ`.

`--template DIR` selects a typed-session bundle containing `session.bin` and
`input-N.bin`, or a legacy bundle containing `board/session-template.bin` and
`assets/`. Generic images preserve typed service extents and regenerate manifest
input facts. Preparation refreshes the JPEG extent's embedded board prefix with
the outer session identity. The default CPU template supplies the
cached board, thermal and CPU grant records. GPU ladder images use the cached
GPU template and host source tree; `--host-tree DIR` selects another compatible
legacy decoder. The OS's generic path has no mimus dependency.

Queued entries also hash the host preparation sources, FEL executable, rail
helper, DRAM builds and training records. Changed inputs cause a refusal before
launch. Status includes running entries, eligible queued entries and the last
20 completed entries per board. `ledger.jsonl` appends each queue submission,
start, result, pause and resume. Entry files and earlier evidence are retained.
`--queue DIR`, before the subcommand, selects an isolated queue.

## A run establishes its prerequisites

```sh
$board run /absolute/image.elf --port 3-1 --kind cpu-return \
  --expect /absolute/expected-output.bin
```

A direct run prints a one-line outcome and result path. It does not enter the
queue. Its evidence directory contains the frozen image, session, transfers,
readbacks, outputs, return report, decoded breadcrumbs and `result.json`.
`--evidence DIR` selects a new destination.

The runner verifies the A733 ROM instructions, protects the `readl` helper's
SRAM scratch, and uses the measured four-second WDT0 sequence to obtain a fresh
FEL instance. It waits for the new instance's udev permissions. While the board
re-enumerates, a sysfs attribute read can fail with ENODEV or ENOENT; discovery
treats both as an absent device and the wait continues until its deadline
([`checks/usb-access-discover.py`](../checks/usb-access-discover.py)). DRAM setup reuses
`tools/run-ddr.py` under the same lock. A matching SID selects a cached training
record; missing or rejected restoration falls back to full training. The default
record directory is `~/.cache/sunstoneos/ddr-records`;
`--training-dir`, `--ddr-restore` and `--ddr-build` override those inputs.

Images with the CPU entry guard run the SHA-256-pinned rail helper first. It
normalizes both CPU rails to 800 mV after checking the speed bin and boot
clocks. It accepts either rail already at 800 mV, writes raised DCDC3 before
DCDC5, and verifies each changed rail. Helper refusals stop the run. A reset alone does not establish PMIC rail values.

The pinned helper returns without disarming WDT0 or writing breadcrumbs. For
this helper, the runner verifies the raw 26 MHz oscillator and selects its
divided watchdog clock. The interval is 512,000 cycles of DCXO/750, about
14.769 seconds. Return and disarm must complete strictly before 14 seconds,
measured with Linux `CLOCK_BOOTTIME`, which includes host suspend. A later or
altered return is refused without reading DRAM. Ordinary returning images use
their breadcrumb contract and the existing RTC watchdog clock.

Preparation creates fresh session identities, checks the native allocations and
CPU grants, and builds the SRAM launcher for the image's watchdog mode and
session lifetime. Every upload is read back and compared. WDT0 supervises DRAM
helpers, rail normalization and the submitted image. The returned watchdog state
and retained RTC breadcrumbs are classified before any DRAM result read.

## Interpret the result and recovery state

| Outcome | Meaning |
| --- | --- |
| `returned` | Matching return breadcrumbs, valid retirement report and matching output. This does not qualify the device's complete functionality. |
| `failure-return` | The image linked the [failure return](designs/failure-return.md) and returned normally after its first failure. Retirement is valid and DRAM is restored; the breadcrumbs and the returned `ss_failure` record describe the failure. The result is failed (exit status 1) and `first_failure` holds the record. The port is not quarantined and not paused. |
| `result-mismatch` | Uploaded bytes, the native report or the returned output differ from the required values. A failure return whose record differs from `--expect` is also reported here, after DRAM restoration. |
| `watchdog-reset` | A new permitted FEL instance returned with WDT0 still enabled. Breadcrumbs identify the last stage and any first fault. DRAM is lost. |
| `no-return` | No accessible new FEL instance appeared within the watchdog window and margin. Recovery is unresolved. |
| `refused-before-launch` | Image, session, helper or prerequisite validation stopped the submitted image before launch. Earlier setup helpers may have run. |
| `host-error` | Host code, transport or evidence handling failed. Inspect the first failure and quarantine fields. |

Each result records the image SHA-256, Git revision, per-stage durations,
evidence path and first failure. A watchdog reset uses the decoded fault ELR
when available, otherwise the last breadcrumb stage. A host exception after a
confirmed normal return does not quarantine the returned instance.

Quarantine applies only to an instance whose transport state is unknown, or a
board that misses its recovery deadline. A failed transfer ends commands to that
instance. If WDT0 may be armed, the runner retains the port lock and observes
host metadata through its recovery window. A verified fresh instance can be
disarmed and released. There is no automatic repeat of the submitted image.

The daemon pauses a port after:

- A no-return or a quarantined result.
- Two consecutive failed DRAM bring-ups.
- A worker resubmits the same image hash with the same hypothesis.
- Three failures at the same first-failure site under the same hypothesis.

A duplicate entry receives a refusal result. An `any` duplicate pauses the
previously assigned board; if neither entry has an assignment, it pauses both.
A generous backstop refuses a worker's 41st submission within an hour.
There is no smaller per-run count limit.

`paused-PORT.json` states the reason. After investigating it, the coordinator
uses `$board resume 3-1`. Resume archives the pause file. It does not clear a
failed USB instance or repeat an image. Daemon restart collects an existing
child result after that process stops; it never replays an interrupted image.
Resume changes only the selected port's pause. Only the daemon collects orphan
results; recovery skips a live runner even after it has written its result.
An interrupted spawn with no recorded child identity remains paused for
coordinator inspection. Shutdown waits for existing children.

## Retain the electrical and storage checks

The reviewed operating points are CPU-L 1794 MHz at 1000 mV, CPU-B 2002 MHz at
1050 mV, GPU 1008 MHz at 960 mV and NPU 852 MHz at 800 mV. The rail helper's
normalization point is 800 mV. Image descriptors and CPU session grants outside
that set require `--approved-by coordinator`. The flag does not bypass native
speed-bin, clock, voltage or session checks.

Native probes follow [the probe contract](board-probe-contract.md) and use
`board.py check` for host-only admission. A raw watchdog-reset diagnostic
requires `IMAGE.board.json`, schema `sunstone-board-probe-v1`, bound
to the image SHA-256. It names entry `0x41000000`, report `0x42000000`, watchdog
mode and session duration, `purpose: watchdog-reset-diagnostic`, storage-write
intent and any operating points.
Declarations describe a reviewed probe; they do not establish electrical facts.

Storage-write intent requires coordinator approval. This runner does not create
storage grants or execute storage-write sessions, even with that flag. Those
operations remain in the existing backup-first update path and must meet
[goal section 4](goal.md). Read-only execution grants no card or NVMe writes.

`--kind npu` supports two returning image formats. Generic OS images require
both the returning watchdog contract and the linked OS NPU service, and use
typed service sessions. `--host-tree` selects their geometry, session producer
and return validators; submission hashes that source tree as well as the runner.
Before image uploads, the runner saves the image and monitor DRAM, manifest WORK
and OUTPUT regions, uploaded INPUT spans and the linked NPU pool. It merges
overlapping preservation spans and refuses uploads outside protected DRAM or
the launcher SRAM reservation. Backups use chunks of at most four MiB with
SHA-256 hashes.

After a matching normal return without a retained fault, the runner copies the
CPU return report, application output, native NPU stop report and optional
telemetry. Mandatory CPU and NPU retirement proofs authorize DRAM restoration.
It validates every backup and its complete range inventory before writing any
saved bytes, then verifies each restoration by readback. The returning ROM owns
SRAM. Missing retirement, uncertain return or incomplete restoration quarantines
the instance and prevents the post-ROM rail helper. Malformed application output
or optional telemetry fails the result after established cleanup. With
`--restore-rails`, guarded CPU images run the pinned helper after restored DRAM
and verified boot clocks, including when functional result validation fails.
The post-ROM helper is separately pinned: it requires DCDC3 already at 800 mV
and DCDC5 at 1000 mV, then lowers DCDC5 to 800 mV. Each helper preserves its
full linked DRAM image, including BSS, stacks and heap, and its result buffer.
It restores those bytes after its validated normal return. Finite native NPU diagnostics use the preparation and
retirement rules described below. Neither format's serialization establishes
physical qualification.

## USB class probes require finite device cleanup

`--kind usb-class` uses the existing [native probe contract](board-probe-contract.md).
The ELF must link `a733_usb2_native_poll`, the immutable probe marker and the
strong device-quiescence and interrupt-restoration hooks. Preparation creates
the standard probe session and uploads; capture validates the standard nonce,
cleanup flags, retired CPU mask, disabled watchdog and payload checksum.
The route needs no CPU session template or captured CPU clock state.

Every USB class probe requires `--approved-by coordinator` after review of the
source and current-cutoff evidence. Selecting `--kind probe` for the same linked
USB image preserves that requirement. The approval field records coordinator
authorization; it supplies no electrical evidence. VBUS approval remains pending.
The development wrapper accepts the same kind, descriptor and approval fields.

The input, serial, camera and camera-JPEG OS images remain inadmissible. Their
supervised lifetime publishes application output while retaining controller
resources. It lacks native USB host and USB0 drain, interrupt restoration and a
finite normal return. Service-buffer release does not establish controller DMA
completion. The CPU-return supervisor rejects USB startup, and the native probe
requires all secondary CPUs to remain statically OFF. These lifetimes cannot be
substituted for each other. A power-removal marker is refused before device
access, including when coordinator approval is supplied.

A finite accessory-free control must first implement a source-backed stop and
restore sequence under the probe contract. Its source and selected image must
establish whether VBUS is touched. No control image or OS class image is frozen
while these prerequisites remain unresolved. A watchdog reset records failure
and loses DRAM; it does not authorize reading retained application output.

The generic session parser accepts an optional USB memory extent only with the
fixed `0x42200000..0x42a00000` arena and `0x7a000000` application alias. Its digest
does not establish RAM ownership. Backup geometry includes that arena only when
the actual writable, non-executable `.uncached_pool` load allocation and strong
linker bounds cover the exact range. Application result bounds remain unchanged.

## Qualify on port 3-1

The host checks build a deliberate watchdog probe and the CPU expectation file:

```sh
~/.cache/sunstoneos/venv/bin/python3 -B checks/board-runner.py \
  --out /absolute/new/check-directory --hello /absolute/cpu-return-hello.elf
```

Without `--hello`, the check runs its fake board, queue and probe instruction
cases. Supplying the control ELF also checks real CPU and cached GPU preparation.
The following artifacts are already built in the local cache:

```sh
hello="$HOME/.cache/sunstoneos/build/board-runner-1031/merged-hello/sunstone-a733-os-hello.elf"
checks="$HOME/.cache/sunstoneos/build/board-runner-1031/probe-final"
gpu="$HOME/.cache/sunstoneos/build/gpu-bisect-1031/crumbs/ladder-wdt4/2-firmware/sunstone-a733-os.elf"
$board run "$hello" --port 3-1 --kind cpu-return \
  --expect "$checks/qualification/hello-expected.bin"
$board run "$gpu" --port 3-1 --kind gpu-return
$board run "$checks/qualification/watchdog-probe.bin" --port 3-1 --kind probe
```

Run these in order, inspecting each result before proceeding. The first expected
outcome is `returned`, with the exact hello-compute payload and seven retired
secondaries. The GPU control also predicts `returned`, with a decoded GPU
retirement report. A native GPU failure instead produces its retained first
failure; it is physical evidence requiring investigation.

The final probe records `EL3_ENTRY` and parks without touching storage or
processor operating points. Its expected outcome is `watchdog-reset`, with
`breadcrumbs.session=true`, last stage `EL3_ENTRY`, `dram_lost=true` and
`quarantined=false`. Its CLI exit code is 1 because a watchdog reset is a failed
image run. It tests recovery without repeating a failed workload. Stop after
any unexpected transport failure or no-return and preserve the result path.

NPU image preparation requires the domain already ON. Under the port lock,
after reset, DDR preparation and the pre-run rail helper, the runner reads the
PCK clock gate at `0x070101ac`. A closed gate refuses PPU access. It then
requires the existing NPU AHB and MBUS gates at `0x020025c0` bit 6 and
`0x020025e0` bit 18 before reading PCK4 policy and status at `0x07064000`
and `0x07064008`. Both low nibbles must be 8. This admission writes no gate,
clock or power register and accesses no NPU engine register. CPU-only images
skip these reads. The session journal retains the fresh raw observations.

Offline `board.py check` requires captured values for all five registers;
missing, OFF or uncertain values refuse preparation. Modeled values in host
checks are specimens and establish no physical admission. A historical ON
observation cannot replace the live runner check.

Finite NPU diagnostics use `--kind npu`. Supply a `--template` directory with
`session.bin`, the reviewed 456-byte native NPU session template. The matching
`manifest.json` may be in that directory or beside the ELF. Submission copies
both files into the queue entry and hashes the finite NPU host dependencies.
`--host-tree` selects the NPU source tree when it differs from the runner.
The runner checks the image, manifest and template before opening the board.

The finite adapter initializes DDR through the shared queue path, captures the
NPU system-register set and rebinds the native session through the existing NPU
preparation helper. Before each NPU run, the pinned rail helper confirms DCDC3 and DCDC5 at
800 mV. The helper preserves its own DRAM uploads. Before image uploads it saves
the complete 18 MiB NPU DRAM reservation. Immediately before watchdog arming it
checks the reviewed ROM instructions and native NPU entry state again.

After normal return, the interrupt-profile parser validates the monitor,
platform log and cleanup receipt before restoring the saved DRAM. Numerical
mismatch still permits restoration after validated retirement and remains a
failed result. An invalid receipt or incomplete restoration quarantines the
instance. The new ROM retains ownership of SRAM; the adapter never restores
old SRAM over it. Watchdog recovery follows the shared runner path and does
not restore potentially invalid DRAM. These host paths do not qualify hardware.

The development wrapper accepts a finite NPU session file through `--records`
and an exact build record through `--manifest`. Both inputs reach the same
runner preparation as `--template`; `--state` selects offline checking.
`--expect-result` compares the decoded JSON result for application decoders.
Malformed or incomplete thermal captures fail result validation while retaining
the native return, application, GPU and thermal bytes.
