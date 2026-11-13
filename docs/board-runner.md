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

## Choose the DRAM mode

Every run resets the board and initializes DRAM from the training record before
the first upload (the `reset` and `dram` stages). Nothing that a run leaves in
DRAM survives into the next run, and no later stage reads bytes that a saved
copy would restore: the post-ROM rail helper uploads its own image and result
buffer, and the host decodes only the reports it copied out of DRAM after the
return. Saving the image's DRAM before launch and writing it back after
retirement therefore serves only a claim that the run leaves DRAM as it found
it. A development run makes no such claim.

A development run is the default. It skips the DRAM save before launch, the
restoration after retirement and the restoration readback. It keeps every proof
of retirement: the CPU return report, the native NPU stop report, the output
capture, the breadcrumbs, the watchdog handling, the USB source checks and, with
`--restore-rails`, the post-ROM rail helper. The pre-launch rail helper still
saves and restores its own two regions (the helper load region and its result
buffer, about 0.3 MB), because it writes into the region the image is about to
load and the image should start on the DRAM that initialization produced, not on
helper residue. The post-ROM helper saves and restores nothing in a development
run. The result records `dram_mode` as `development`, `dram_unsaved` as true and
`dram_restored` as false. A development run never quarantines for DRAM
ownership: no DRAM was promised, so no run leaves it unresolved. A run still
quarantines when native retirement is unproven, when a transfer fails, or when no
instance returns.

`--preserve-dram` selects the qualification mode, which saves and restores the
DRAM as described below and records `dram_mode` as `preserved`. A run is a
qualification run, and requires the flag, when any of these holds:

- Its claim includes that the returning lifetime leaves DRAM as it found it,
  for example a qualification of the returning CPU or accelerator lifetime.
  Other passing records come from development runs; each record states its
  `dram_mode`.
- It exercises the save, restoration, readback or quarantine path itself, such as
  the helper DRAM preservation and the return chain.
- It stores state in DRAM for a later image on the same boot, or is followed by a
  storage write that depends on DRAM contents.
- The coordinator asks for it.

Any other run, including iterations on an image under development, a hypothesis
probe and a run that only reads hardware facts, uses the default. The mode is
frozen in the queue entry (`preserve_dram`).

Measured on 2026-11-01 for a generic image that preserves 27,262,976 bytes in
seven regions, a preserving run takes about 171 seconds. Reading the backup takes
about 47 seconds of the 52-second `launch` stage. Restoration takes about 101
seconds of the `capture` stage: about half writing and half verifying the
readback, at about 0.55 MB/s in each direction. The remaining stages take about
23 seconds in total, and a development run omits the two transfers.

## Submit a frozen image and hypothesis

```sh
$board submit /absolute/image.elf --kind cpu-return --port any \
  --worker cpu-integration \
  --hypothesis 'The corrected cache cleanup permits all cores to retire' \
  --reference docs/cpu-clock.md#operating-points-come-from-the-26-mhz-table-columns
$board status
$board watch
```

`--reference PATH#ANCHOR` is required. PATH is a file under `docs/` that is
tracked in the host tree (`--host-tree`, otherwise this checkout), as an absolute
path or relative to that tree's root. ANCHOR is the anchor GitHub derives from a
heading in that file: the heading text in lowercase, with punctuation removed,
spaces replaced by hyphens, and `-1`, `-2` appended to repeated headings. The
heading holds the subsystem's vendor sequence and the diff of our code against
it, so the run tests only predictions from that diff. `submit` refuses a missing
flag, a missing or untracked file, a file outside `docs/` and an unknown anchor,
before it creates an entry. The entry and the result record the reference
(`reference`, with the file's SHA-256 in the entry), and `tools/evidence.py add`
copies it into the evidence record.

`--reference none` exempts a run whose purpose is to read hardware facts, such as
an image that only reports register values. It is accepted only with
`--approved-by coordinator` and a nonempty `--reference-reason`; the reason is
stored with the entry and the result.

`submit` copies the image, inputs and session template into
`~/.cache/sunstoneos/board-queue/entries/`. It blocks until a result exists and
prints its JSON path. `--no-wait` prints the future result path immediately.
`--wait` also prints the outcome, the stage, the first failure and the decoded
summary before the path, and exits 0 for a returned image whose decoder reports
no failure or the exact expected resident stop described below.

### Build, package, submit and wait

Four commands take a source tree to a board result. None needs a script of its own:

```sh
python3 tools/configure.py SRC BUILD -DSUNSTONE_A733_OS=ON ...        # every pinned dependency path
PATH=~/.cache/sunstoneos/host/usr/lib/llvm-21/bin:$PATH cmake --build BUILD --target sunstone-a733-os -j8
python3 tools/session.py build BUILD/sunstone-a733-os.elf --out DIR --input IN0 --input IN1 \
    [--npu-declaration MANIFEST.json] [--decoder FILE]
python3 tools/board.py submit BUILD/sunstone-a733-os.elf --kind cpu-return --port any --worker NAME \
    --template DIR/template --hypothesis TEXT --reference PATH#ANCHOR --wait
```

`tools/configure.py` takes the dependency paths from `sources.lock.json` through
`checks/image_dependencies.py`, and refuses with the fetch command when one is
missing. `tools/session.py build` writes the typed session of the image: the
image identity and manifest digest, the service records the image links (thermal,
CPU clock grant, NPU), the NPU catalogue digest, client model and pool from the
image's symbols, and the board and evidence facts of `tools/session-facts.json`.
It uses the encoder of `apps/os/host.py`, which the runner and the host admission
also use, and it reads no earlier queue entry. A field that no source determines
is an error that names it; an image with a DMA service is refused because the facts file pins nothing for it.
The session grants the device classes that the manifest declares and the image can serve, with no option to set bits: the GPU
(required or optional) when the image links the GPU service, which has no session record and needs only the device bit and the
control profile's workload 2. The NPU is a service, not a device bit; the manifest's `npu` section selects it. Every other class
(USB storage and keyboard, I2S and headset capture, the USB class devices, the radio) needs facts the file does not pin: a required
one is an error that names what is missing, an optional one is left out and listed as `devices_left_out` in `session.json`.
`tools/session.py check DIR [--admit]` decodes the session, compares it with the
image and, with `--admit`, runs the image's own EL3 admission on it. An NPU image
needs `--npu-declaration` (a manifest with an `npu` section, or a JSON object of
`slots`, `in_flight`, `graphs` and `links`), because the image does not carry it.
`--decoder` copies the application's decoder, which the application's own tool writes (for the graph study,
`apps/npu-graphs/host.py`).
`watch ENTRY_ID` waits for that entry; `watch` prints status when it changes.
`--port 3-1` or `--port 3-3` fixes the target. `any` selects the first eligible
board, respecting submission order among entries eligible for that board.

The hypothesis is the prediction: it states the result that the diff of our code against the vendor sequence leads
us to expect, and the runner does not compare the output with any expected value. The coordinator compares the
decoded result with the hypothesis in review. The queue freezes the image, inputs and template bytes at submission;
the runner refuses a claim whose frozen bytes changed. It uses the host code that the daemon started with, and the
result records that code's Git revision. `--input FILE` supplies each manifest input in order. Without an
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
start, result, withdrawal, pause and resume. Entry files and earlier evidence are retained.
`--queue DIR`, before the subcommand, selects an isolated queue.

## Resident completion test

`--lifetime resident` selects the linked resident contract and requires
a typed session and the immutable diagnostic marker
from `SUNSTONE_A733_OS_RESIDENT_STOP_ON_FINISH`. The default lifetime remains
returning. A resident test uses development DRAM mode; `--preserve-dram` is
refused because the expected reset destroys DRAM.

The launcher publishes no diagnostic deadline, an initial expiry four seconds
after its timestamp, WDT0 mode `0x41`, configuration 1 and output 2. It requires
zero FEL and redirect fields, timestamps immediately before one WDT0 restart,
and verifies the readbacks. The runner creates a fresh session identity and
authenticates the complete typed session. Its observation bound remains the
image's session duration, 60 seconds for the hello-compute diagnostic.

The expected end retains outcome `watchdog-reset` and records
`expected_reset: true`. Success requires the session's RTC tag, an EL3 stop on
core 0 with stage `0x0111` and value `0x5000`, no fault or board-failure record,
and no cleanup failure. Every other reset fails. The runner disarms WDT0 on the
new FEL instance and reads no DRAM result or application output. The status and
submit commands accept this exact expected end as successful.

[Resident FEL entry](designs/resident-fel.md) accepts the same dynamic-OFF state
as returning entry. Cold card boot retains stage 2's static-OFF contract. The
coordinator lands the runner changes and restarts the daemon before submission.

## Deliver a Wi-Fi credential

`--wifi NAME` selects a credential that the user stored with `tools/wifi-credential.py`
([Wi-Fi credentials](wifi-credentials.md)). The password never enters the queue:

- `submit` loads the file to compute its fingerprint and refuses a missing file or a file with unsafe modes before it
  creates an entry. The entry, `state.json`, the ledger line and the command line of the runner carry only
  `wifi: {name, fingerprint}`.
- When the daemon claims the entry, and again in the runner process before any device work, it reads the file from
  `~/.config/sunstoneos/wifi`. A missing file, a directory that is not mode 0700, a file that is not a plain mode 0600
  file owned by the user, or a fingerprint that differs from the submitted one refuses the run as
  `refused-before-launch` with the reason. The result carries `wifi: {name, fingerprint}`.
- The image must link `ss_wifi_credential`; any other image is refused before device access.
- At launch the runner uploads the 256-byte record to `0x43004000` as a separate transfer, after the image's uploads.
  The source and the readback are anonymous memory files, so the record is written to no file, and it is compared
  in memory. The host copy is zeroed after the upload and the result records `wifi_delivered`. A preserving run
  includes the record's extent in its saved and restored DRAM.

## The runner checks the image's own admission before the board

`check` and `submit` run, on the host, the same EL3 admission code the image runs
([host_admission.py](../tools/host_admission.py)). The tool builds
`a733_smp_contract_refusal` and `os_session_decode_why` (`apps/os/admission.c`
or its cpu-return copy, `apps/os/session.c`, `apps/board/admission.c`) with the
compile definitions of the image's own configured build, links the image's build
ID and manifest section, and runs them on the exact session bytes the runner
uploads and the contract the launcher completes (arming time, startup, the
image's lifetime). A refusal stops `check` and `submit` before any device access
and names the term the board's stop would name:

    EL3 admission would stop the board with a733_smp_stop(1, 0x168): session_decode (run_deadline);
    manifest run_us 300000000 plus return_reserve_us 5000000 exceeds the 60 s session lifetime of this image by
    245000000 us; the decoder needs a lifetime of at least 305 s, and the build derives 308 s with the 1 s startup
    and 2 s setup allowance

`submit` builds the session from the frozen inputs with the recorded board words
(`board_images.offline_words`) in place of a live read. The result is stored in
the queue entry as `preflight`, copied into the run's `result.json`, and printed.
The harness is cached by the image's SHA-256, so the queued copy reuses it. The
terms and their sub-terms are in
[a733_admission_refusal.h](../include/a733_admission_refusal.h);
`tools/a733-crumbs.py` decodes the same table from the breadcrumbs of a board
stop (`last.admission_refusal`). [admission-preflight.py](../checks/admission-preflight.py)
checks the refusals of a built image.

The pre-flight is never skipped silently. When it cannot run for an image that
has a typed session, `submit` and `check` refuse with exit status 2 and queue
nothing. The reasons are an image the tool cannot inspect, no recorded board
words on this machine, a session the offline build cannot complete (for
example a missing NPU entry gate), an image outside its build directory with no
cached harness, a write-capable image without its approval record, and a plan
without a host admission. `--no-preflight REASON` records an override: the
reason is stored in the entry's `preflight.override` and in the result, and the
run is queued. An empty reason is refused. An image that is not a typed-session
OS image (a probe, a USB class or rail-helper image, a finite NPU image) has no
host admission; its entry records `preflight: not applicable` with the reason.

A pre-flight is valid only if it ran the host code the daemon will run. The daemon
records its checkout in `daemon.json` in the queue directory, and the runner it
starts prepares and admits the session with that checkout's files. `submit` and
`check` from another checkout compare every file that prepares or admits a
session with the daemon checkout's `HEAD`. The set is derived, not listed:
`board.session_files` follows what `tools/board.py`, `tools/board_images.py` and
`tools/session.py` import, load and run, and the headers the C sources include.
If any file differs, the tool refuses with `the daemon runs REVISION; these
session files differ: ...; land them and restart the daemon first`. The daemon
keeps running only the code on `bringup`. A daemon started before this record
existed has no `daemon.json` and is not compared until it restarts.

Open work: the harness takes its list of admission sources from the daemon
checkout's `tools/host_admission.py`, while it compiles those sources from the
image's own tree. A series that changes that list cannot pass pre-flight until it
lands: series 4 of the one-OS-image work links `apps/os/npu-declaration.c` into
every image, but `bringup`'s list adds it only under `-DSUNSTONE_A733_OS_NPU=1`, so
the harness stubs `os_npu_declaration_admitted` and stops (its board runs record
`--no-preflight` with the branch tree's own admission result). The fix is for
`board_images.preflight_admission` to load `host_admission.py` from `--host-tree`
when one is given. It belongs to the gate-streamline work, because it changes the
daemon-checkout rule above.

### The recorded board words

The offline build needs board words that a live run reads under the port lock:
the CPU, thermal and clock state words of the session, and the NPU entry state (the
three entry gates at `0x070101ac`, `0x020025c0`, `0x020025e0` and the two PCK4
words at `0x07064000` and `0x07064008`). The runner keeps them in
`~/.cache/sunstoneos/board-words.json`, each with the run that read it, and
refreshes the file whenever a run completes: the NPU entry state from the
result's `npu_on_admission` when its status is ON, and the session state words
from `live_words` when the run returned. A refused entry state never refreshes
the file. When the file does not exist, the first pre-flight builds it from the
finished runs already in the queue. The evidence capture of the CPU thermal state
is the base the recorded words are laid over. The words are consistency inputs,
not liveness: admission validates them against each other. A machine with no
finished run that read the NPU entry state, and so no recorded gate words, has
to take one fresh read from the board (the first NPU run, with
`--no-preflight`) before its NPU images pre-flight.

## Board runs keep their timing while the gate runs

The host stages of a run (DRAM bring-up, preparation) are timed against the disk and CPUs the integration gate also uses. When the
gate's power-loss sweep wrote 170 to 290 MB/s, those stages took 29 to 74 s and 11 to 18 s instead of about 2.3 s and 0.6 s, on both ports
and for every worker. [host_priority.py](../tools/host_priority.py) sets the priorities, in code, so a restart keeps them:

| Process | Where it is set | Setting |
| --- | --- | --- |
| The gate and everything it starts (builds, checks, sweeps) | first statement of `main()` in `tools/integration-gate.py` and `tools/land.py` | nice +10, idle I/O class (`ionice -c3`) |
| The runner, its run processes and the queue daemon | first statement of `main()` in `tools/board.py` | best-effort I/O class at level 0 (the default is 4); CPU niceness unchanged |

The gate's image-copying checks write sparse copies (`harness.copy_image`, `cp --sparse=always --reflink=auto`): the `fat32-rw` sweep wrote
271 GiB in 5,339 copies of a 52 MiB image of which 55 of 13,312 blocks hold data, and writes about 1.4 GiB now.

Every result carries `host_pressure.start` and `host_pressure.end`: the kernel's `/proc/pressure/io`, `cpu` and `memory` figures
(`some` and `full`, `avg10`, `avg60`, `avg300` and `total`) read when the run began and when it finished. The totals are microseconds of
stall, so their difference is the stall time of the run itself, which attributes a slow host stage to the host.

## A run establishes its prerequisites

```sh
$board run /absolute/image.elf --port 3-1 --kind cpu-return
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
14.769 seconds. The countdown runs on the board, so the runner decides from
what the board and the kernel report, not from how long the host took to notice:
the kernel's creation time of the new instance's device node (`statx` birth time)
minus the host's stamp before the arm must be under 14 seconds, WDT0 must read
back armed on that instance and then clear after the disarm, and the host must
have talked to that one instance throughout (a later expiry would have removed
its node and failed the transaction). Under a loaded host the old host-elapsed
figure reached 16.8 s on a normal run (usual 2 s) and refused a healthy board.
A return the kernel dates later than the cutoff is refused without reading DRAM;
a node the kernel cannot date falls back to the host's `CLOCK_BOOTTIME` elapsed
time. A wait for a new instance extends by 30 s once an instance has enumerated
but udev's permission grant is late. Ordinary returning images use their
breadcrumb contract and the existing RTC watchdog clock.

Preparation creates fresh session identities, checks the native allocations and
CPU grants, and builds the SRAM launcher for the image's watchdog mode and
session lifetime. Every upload is verified before the image is launched, by
reading it back or by an on-board digest (see
[Verify uploads by digest](#verify-uploads-by-digest)). WDT0 supervises DRAM
helpers, rail normalization and the submitted image. The returned watchdog state
and retained RTC breadcrumbs are classified before any DRAM result read.

The post-ROM rail helper (`--restore-rails`, the default) is built to take DCDC5 from the 1000 mV an image left it at to
800 mV, and refuses ("CPU supplies") a board whose rails are not at that entry point. An image that never raised the rails
(the NPU loader tests) leaves them at 800 mV, which is the restore target. The runner treats that refusal as a verified no-op
when the helper's own readbacks show it refused before any operation, DCDC5 and DCDC3 at 800 mV, and the three boot clock
rows; the result records `rail_restore.outcome` as `already-at-target` (or `restored`). Any other refusal leaves the application's
outcome as it was, records `rails_restored` false, `rails_unrestored` true and the helper's facts, and pauses the port until
someone checks the rails.

## Verify uploads by digest

The FEL loader in the boot ROM is a USB full-speed device (12 Mbit/s). It moves
about 0.56 MB/s in each direction: in the DRAM stage of a run on 2026-11-12, the
246,776 bytes of boot0 and its entry code took 0.442 s to write and 0.442 s to
read back. Reading every upload back therefore doubles the upload time. A speech
recognition session that uploads about 44 MB of image, session and inputs spends
about 155 s of its 170 s run in the `launch` stage, about 79 s of it reading
bytes back.

`--verify digest`, the default, writes each DRAM upload once and has the board
compute its SHA-256. `--verify readback` reads each upload back in the
transaction that writes it and compares the bytes. The digest path works as
follows:

1. The runner builds the digest helper
   ([boot/a733/upload-digest.S](../boot/a733/upload-digest.S)), 1,152 bytes of
   A32 code, and uploads it with a readback to `0x52000`, the start of the
   launcher's SRAM reservation (`0x52000..0x52d80`).
2. It writes the image segments, session, inputs and other DRAM uploads without
   reading them back.
3. For each batch of ranges, it arms WDT0 in mode `0xb1` (16 s), writes the
   request block at `0x52800`, executes the helper and reads the block back,
   all in one FEL transaction, then disarms WDT0. The helper enables the
   floating-point unit, hashes each range with the SHA-256 instructions,
   restores CPACR and FPEXC, and returns to the ROM. The block holds at most 34
   ranges, the number that fits between `0x52800` and the end of the
   reservation; a longer upload is split into several ranges.
4. The runner compares each digest with the SHA-256 of the bytes it sent.
5. It uploads the launcher, request and report into the reservation, with a
   readback, and launches.

A helper call lasts about 2 s, a fifth of the 10 s USB transfer timeout of
`sunxi-fel` (the request that follows the execute command waits for the helper
to finish). The first batch assumes the readback rate, 550,000 bytes/s; each
later batch is sized from the rate the previous call measured. A board whose
core has no SHA-256 instructions (ID_ISAR5.SHA2 is 0), or a first call no
faster than the readback rate, makes the runner read the remaining ranges back
instead; the result records the reason as `upload_verify.fallback`.

On port 3-3 on 2026-11-13 the helper hashed 6.2 to 6.8 MB/s, and writes moved
0.565 MB/s, so a digest upload takes about 1/0.565 + 1/6.7 = 1.92 s per MB,
against 3.54 s per MB with readback. A `launch` stage therefore lasts about
1.92 s per MB of DRAM upload plus 2 to 5 s of other transfers, as in these runs:

| Run | DRAM upload | Verification | First upload to launch | `launch` stage | Total |
| --- | --- | --- | --- | --- | --- |
| Recognition, CPU only, 1794107014170351336-e7854a09 | 16.3 MB | readback | 59.6 s | 60.5 s | 78.6 s |
| The same image, 1794596384126694694-84ec8555 | 16.3 MB | digest: 28.8 s writing, 2.4 s in 3 helper calls | 31.7 s | 33.3 s | 48.1 s |
| Recognition with the NPU, 1794595892235825416-6bdcd39c | 43.8 MB | readback | 155.8 s | 155.4 s | 171.0 s |
| The same image, 1794596079966938149-24a0a868 | 43.8 MB | digest: 76.9 s writing, 6.5 s in 5 helper calls | 86.6 s | 89.1 s | 102.9 s |

Both digest runs completed every helper call with status `DONE` and no fallback.
The CPU-only run returned the reference result. The NPU run's application then
stopped at an NPU completion fault that a readback-mode run of an earlier image
(1794506252076994875-247497f5) also showed; the upload had been verified.

The helper adds 0.565/6.7, about 8.4 %, to the write time. To bring upload time
within 1 % of the write time alone, the helper would need about 57 MB/s, about
8.5 times its measured rate. It executes about 120 instructions per 64-byte
block ([checks/upload-digest.py](../checks/upload-digest.py)), so 6.7 MB/s is
about 12.5 million instructions a second: the helper runs with the instruction
cache off (`sunxi-fel` turns it off for the ROM's FEL code) and fetches every
instruction from SRAM. Enabling the instruction cache for the helper's duration is the
first change to measure.

The digest path applies to a plan that executes `0x52000` and uploads into the
launcher's reservation: generic, legacy and finite NPU images. It verifies each
DRAM upload whose address is a multiple of 4 and that no later upload overlaps;
the helper's loads need 4-byte alignment, because with the MMU off a data access
is to Device memory, where an unaligned access faults. These transfers keep the
readback in either mode: the ROM scratch restoration, the rail helpers and their
saved DRAM, the DRAM stage's SRAM payloads, the Wi-Fi credential, every upload of
a probe or first-stage recovery plan, and the DRAM restoration of a preserving
run, which runs after the returned ROM owns SRAM.

The result's `upload_verify` records the mode, the reason when the digest path
does not apply, the helper's SHA-256, ID_ISAR5 and each batch's ranges, bytes,
seconds and status. `check` reports which uploads each mode verifies.
[checks/upload-digest.py](../checks/upload-digest.py) runs the helper's bytes
under Unicorn against hashlib and holds it to the accesses described in its
source; [checks/board-runner.py](../checks/board-runner.py) runs both modes on
the fake board, including a flipped byte and a short write.

The images authenticate only part of what they receive. EL3 admission checks the
typed session's SHA-256 against the contract, the radio service checks its
profile against a digest the session carries, and the OS application checks
each manifest input's SHA-256 before the application starts (`ss_fail(0x753)`,
which the runner reports as an image failure). The image's load segments are
bound only by the 32-byte build identity that the contract names. The contract,
the request, the launcher and the zeroed result and report pages have field or
sentinel checks at most. The runner's verification therefore covers every
upload itself, before launch.

## Interpret the result and recovery state

| Outcome | Meaning |
| --- | --- |
| `returned` | Matching return breadcrumbs, a valid retirement report and a success result record. The output is recorded, and `decoded` holds the application decoder's result. This does not qualify the device's complete functionality. |
| `failure-return` | The image returned normally with a result record that is not a success record: either it linked the [failure return](designs/failure-return.md) and returned after its first failure, or its result record carries a nonzero status, a foreign session or a malformed report. Retirement is valid (an NPU image may publish the never-started stop report, `finished` 2, which is accepted only here) and DRAM is restored; the breadcrumbs and the returned `ss_failure` record describe the failure. The result is failed (exit status 1) and `first_failure` holds the record. The port is not quarantined and not paused. |
| `watchdog-reset` | A new permitted FEL instance returned with WDT0 still enabled. Breadcrumbs identify the last stage and any first fault. DRAM is lost. The runner disarms WDT0 on the fresh instance and releases it: the result records `dram_released_by_reset`, the instance is not quarantined, and the port is not paused. |
| `no-return` | No accessible new FEL instance appeared within the watchdog window and margin. Recovery is unresolved. |
| `refused-before-launch` | Image, session, helper or prerequisite validation stopped the submitted image before launch. Earlier setup helpers may have run. |
| `upload-fault` | An upload read back, or hashed on the board, differently from the bytes the runner sent, before launch. No image ran and the instance is not quarantined; an unchanged resubmission may pass. `first_failure` names the method (`upload-readback` or `upload-digest`), the address, size, source file and offset, and the expected and observed SHA-256 or the first differing byte. |
| `host-error` | Host code, transport or evidence handling failed, including an upload that reads back differently after launch and a result that the decoder cannot decode. Inspect the first failure and quarantine fields. |

Each result records the image SHA-256, Git revision, per-stage durations,
evidence path and first failure. A watchdog reset uses the decoded fault ELR
when available, otherwise the last breadcrumb stage. A host exception after a
confirmed normal return does not quarantine the returned instance.

Quarantine applies only to an instance whose transport state is unknown, or a
board that misses its recovery deadline. A failed transfer ends commands to that
instance. If WDT0 may be armed, the runner retains the port lock and observes
host metadata through its recovery window. A verified fresh instance can be
disarmed and released. There is no automatic repeat of the submitted image.

A watchdog reset loses DRAM, and every run initializes DRAM from the training
record before launch, so DRAM saved before a reset has no later use. After a
reset the runner therefore releases a fresh instance that has `dram_lost` and
`rom_returned` recorded and a read-back disarmed WDT0, even when the run had
unrestored DRAM backups (`dram_dirty`, `npu_dirty`). The following state
survives a reset and is not trusted across runs: PMIC rails and CPU supplies
(a run normalizes them), the NPU power domain, the RTC breadcrumb words (the
next arm clears them) and the host-side DDR training record (validated before
use). A later NPU image reads the gated PPU and fabric state read-only and is
refused when the domain is not coherently on. Unresolved DRAM ownership still
quarantines a normal return on the same instance whose restoration or
retirement failed, and every no-return, transport failure or unresolved return.

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

`$board withdraw ID --reason TEXT` removes one queued entry before it starts;
the coordinator alone uses it. It takes the queue lock, accepts only an entry in
the queued state, and records the ordinary `refused-before-launch` result with
the reason and the withdrawal in `ledger.jsonl` (events `result` and
`withdrawn`). It refuses an unknown ID, a missing reason, and a running or
finished entry. It changes no pause and applies no admission rule. The daemon
skips a withdrawn entry because the entry is no longer queued. The daemon runs
`tools/board.py` from its checkout, so the command is available after the daemon
restarts on a checkout that contains it.

`$board release PORT --reason TEXT` clears a quarantine that an earlier runner
version recorded for a fresh post-reset instance; the coordinator alone uses it.
It reads sysfs metadata and the device node identity and never opens the USB
device. It accepts only the instance currently enumerated on PORT, only when the
quarantine reason is the unresolved-DRAM-ownership reason, and only when the
finished run that ended on that instance has outcome `watchdog-reset`,
`rom_returned` and `dram_lost` true, or was refused before any launch (outcome
`refused-before-launch`, `launch_attempted` false) with the rail helper's disarm
read back clear and a reset to a fresh instance, on an instance whose sysfs ROM
descriptor is the A733 service; either way with no cleanup failure. A run refused
before launch leaves no DRAM owner: no image ran, the helper wrote only its two
runner-owned regions, and the next run's DRAM stage reinitializes DRAM from the
training record before anything depends on it. It moves the
quarantine record to `~/.cache/sunstoneos/trash/recovery-required-released/`,
records the reason and run in `ledger.jsonl` (event `released`), and refuses a
no-return, transport failure, unresolved return or another instance. It does not
change a pause; `$board resume PORT` does.

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
both the returning watchdog contract and an application that declares the NPU
(`ss_application_npu`), and use
typed service sessions. `--host-tree` selects their geometry, session producer
and return validators; submission hashes that source tree as well as the runner.
With `--preserve-dram`, before image uploads the runner saves the image and monitor DRAM, manifest WORK
and OUTPUT regions, uploaded INPUT spans and the linked NPU pool. It merges
overlapping preservation spans and refuses uploads outside protected DRAM or
the launcher SRAM reservation. Backups use chunks of at most four MiB with
SHA-256 hashes.

After a matching normal return without a retained fault, the runner copies the
CPU return report, application output, native NPU stop report and optional
telemetry. Mandatory CPU and NPU retirement proofs authorize DRAM restoration
and, in both modes, the post-ROM rail helper. A preserving run validates every backup and its complete range inventory before writing any
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

## First-stage FEL recovery uses its own SRAM proof

`--kind probe --approved-by coordinator` admits the
[first-stage recovery images](designs/card-fel-recovery.md) through their
immutable `a733_card_fel_profile`. Each image holds only the exact AArch32 gate
code at `0x52000`, followed by its WDT0 policy word `0xb1`, and the read-only
profile at `0x52400`. The profile's entry field selects the request image or the
warm-entry image. Neither image carries input, storage, DRAM, clock or
power-transition authority, and the runner initializes no DRAM. The runner
refuses any FEL binary other than the reviewed one, which disables the
instruction cache before each register command, and runs a frozen copy.

At first contact with a board, before the session's watchdog reset, every
session reads the cold-boot marker (RTC `IC_CHARA`, `0x070901f0`), the GP2 gate
record, the crystal control word and the WDT0 configuration and output words,
and records them as `power_epoch`. It then clears the gate record and, if the
marker's low half does not read `0x16aa`, writes the keyed value `0x16aa16aa`.
A session whose marker or cleared record does not read back stops before
launch. The marker is an RTC-domain register; it survives watchdog resets and
reads 0 only after VDD_RTC loses power. After each watchdog return the runner
records the same words, decodes and then clears any gate record, and treats
the recorded WDT0 mode at gate entry as the mode at reset.

`--marker-clear-test`, which requires `--approved-by coordinator`, tests
whether software can clear the marker. At first contact, after the marker is
set and with WDT0 read disarmed, the runner writes `0x16aa0000` to the marker,
reads it, writes `0x16aa16aa` and reads it again, all in one FEL command.
Both readbacks are recorded as `marker_clear_test`. If the marker does not
read `0x16aa` after the second write, the run stops before launch. WDT0 is
disarmed throughout, so no reset can take the gate's cold path while the
marker reads clear.

The runner uploads both loads, arms WDT0 in mode `0x41`, sets and reads back
the session tag and the entry state, and executes `0x52000`. The request image
sets the vendor RTC request `0x5aa5a55a`. The warm-entry image sets no request
and requires the marker set at first contact; the gate then takes the path a
watchdog-reset card boot takes. Either path re-arms WDT0 for 16 seconds and
branches to the ROM's FEL entry from inside that execute command. The ROM
normally reports the command's status before running the code. If the
disconnect loses that status, the runner ends the transaction after 2 seconds;
a failed or ended execute transaction is the expected USB disconnect, but only
for this command after it has started with the watchdog and entry state armed.
The runner then sends nothing to the old instance and waits for a new one.
Acceptance requires WDT0 still at `0xb1` when first read, the new instance
within 16 seconds of the execute request, the matching tag and no fault word.
The request image also requires the cleared request and LAST `0x0c020005`. The
warm-entry image requires the gate record `0x0c0240NN` in GP2, where `NN` is
the armed mode, LAST unchanged at 0 and the marker still set. The runner then
disarms WDT0. If the ROM does not re-enumerate, WDT0 resets the board to FEL;
the result is `watchdog-reset` with the RTC words, crumbs and elapsed time
recorded. Any other invalid receipt quarantines the instance. The evidence
qualifies only these SRAM paths. Card loading remains a separate test.

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

The host checks build a deliberate watchdog probe:

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
$board run "$hello" --port 3-1 --kind cpu-return
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
800 mV. The helper preserves its own DRAM uploads. With `--preserve-dram`, before
image uploads the runner saves the complete 18 MiB NPU DRAM reservation; a
development run does not. Immediately before watchdog arming it
checks the reviewed ROM instructions and native NPU entry state again.

After normal return, the interrupt-profile parser validates the monitor,
platform log and cleanup receipt before restoring the saved DRAM, if one exists. A numerical
difference in the decoded result still permits restoration after validated retirement; the reviewer reads it. An invalid receipt or incomplete restoration quarantines the
instance. The new ROM retains ownership of SRAM; the adapter never restores
old SRAM over it. Watchdog recovery follows the shared runner path and does
not restore potentially invalid DRAM. These host paths do not qualify hardware.

The development wrapper accepts a finite NPU session file through `--records`
and an exact build record through `--manifest`. Both inputs reach the same
runner preparation as `--template`; `--state` selects offline checking.
A decoder's `decode` result is recorded as `decoded`, and the value of its optional `failures` function as `decoder_failures`; neither changes the outcome.
Malformed or incomplete thermal captures yield `failure-return` while retaining
the native return, application, GPU and thermal bytes.
