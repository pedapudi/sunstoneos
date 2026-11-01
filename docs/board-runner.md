# Run and queue A733 images

`tools/board.py` owns one physical port from identity checking through result
capture. The host implementation has fake-board and offline-image checks.
Physical qualification remains the coordinator's responsibility. Workers submit
images without opening a USB device.

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

An existing `--expect` file requires an exact byte match. Other expectation text
requires a substring in the UTF-8 output. The queue freezes this distinction at
submission. `--input FILE` supplies each manifest input in order. Without an
input argument, hello-compute receives `SunstoneOS board control` followed by a
newline. Its result is the little-endian Adler-32 checksum, a zero reserved
word and the input length, packed as `<IIQ`.

`--template DIR` selects a typed-session bundle containing `session.bin` and
`input-N.bin`, or a legacy bundle containing `board/session-template.bin` and
`assets/`. Generic images copy service records and manifest inputs without
application-specific asset handling. The default CPU template supplies the
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
FEL instance. It waits for the new instance's udev permissions. DRAM setup reuses
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
| `result-mismatch` | Uploaded bytes, the native report or the returned output differ from the required values. |
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

`--kind npu` admits only an image with the returning watchdog contract and the
linked OS NPU service, using its typed service session. The current checkout's
standalone NPU images reject an armed watchdog, and its CPU-return build excludes
NPU. Such images are refused before device access. NPU execution remains
unsupported until a compatible native watchdog and return path is supplied and
checked; session serialization alone does not establish that support.

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
