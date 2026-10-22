# Board-test bundles for the memory, GPU and NPU paths

Status: designed; not implemented. Bundles run only on the board, so every
path they carry stays implemented and host-modeled, with physical behavior
pending, until a board session records the evidence named in its row.

One host tool, `tools/board-bundles.py`, turns the build output of each new
memory, GPU and NPU path into a read-only bundle and a board-test queue
submission. It never contacts the board and never submits. The hand-written
ranked table in [docs/audit/board-test-plan.md](../audit/board-test-plan.md)
orders the tests by their risk of losing the USB connection.

## Recovery classes

The plan names three ways the board returns to the boot ROM's USB recovery
mode (FEL). Each bundle row has one of five classes:

- **ROM** and **Monitor**, as in the plan. A Monitor image runs on one A55
  core under the EL3 monitor, whose secure timer forces a return to the ROM
  2 s after entry (`counter() + 2 * COUNTER_HZ` in
  [platform/a733/monitor.c](../../platform/a733/monitor.c)). Only images
  built with the USB runtime renew that deadline.
- **Native USB.** The image takes USB0 and hands it back, as rank 9 does.
  It ranks after rank 9.
- **Terminal.** The runner keeps the A733 port lock after every outcome,
  including a successful capture, until whole-board power removal is
  confirmed. Every `tools/check-a733-*-supervised.py` runner that ends in
  the shared supervised session does this; see `run_session` in
  `tools/check-a733-smp-supervised.py`. A Terminal test ends the board
  session. While the board cannot be reconnected, no Terminal test runs.
- **Excluded.** The test needs an accessory, a voltage or CPU clock change,
  or an intentional hang. The tool prepares no bundle for it.

The tool derives the class instead of reading a declared value. A row whose
runner is a supervised runner, or whose validated `prepared.json` records
`retention` equal to `whole-board-power-removal`, is Terminal. This mapping
errs toward Terminal, which is the safe direction. A row is Monitor only if
its runner is the FEL result-block launcher that rank 4 uses and its
validated bundle records one core, a FEL result channel and the excluded
DDR range described below. As a second check, the tool refuses a Monitor
row whose image defines any `dcd_*`, `hcd_*`, `tud_*`, `tuh_*` or `usb0_*`
symbol (the USB device and host stacks and the shared USB0 loop) or any
`a733_cpu_start_*` or `a733_cpu_secondary_*` symbol (secondary-core
power-up). That code is the only code in the tree that drives USB0 or
starts another core.

## Rows

Each row names the path's preparation module and its arguments (for
example `apps/os/prepare.py`, `apps/npu-finite/host.py` or
`apps/npu-convolution/prepare-supervised.py`), its runner, the evidence
size and job budget used for the timeout, the expected result and stop
condition, and the `docs/hardware.md` item that the evidence confirms.

| Plan position | Bundle | Class |
| --- | --- | --- |
| After rank 6, before rank 7: twelve sessions | Memory pattern image, one range of at most 1 GiB per session | Monitor |
| Waiting for power removal, one per board session, in this order | NPU service in the OS image; GPU service in the OS image; GPU and NPU jobs under the memory and power arbiter; recognition dispatcher | Terminal |
| Waiting for physical recovery | Full memory qualification image (march, bank and DMA reachability tests, results over USB0); GPU and NPU hang reset; voltage and clock changes; display output | Terminal or Excluded |

NPU comes first among the Terminal rows because GPU firmware writes memory
on its own ([docs/remote-writers.md](../remote-writers.md)), so a GPU
failure has more causes to separate.

**Memory pattern sessions.** Each session runs a returning, single-core
build of the memory qualification image. The memory qualification package
must provide it with a `prepare()` and a `validate_bundle()`. One image
serves all twelve sessions; each session's input block at `0x43000000`
names its range.

- *Ranges.* DDR starts at `0x40000000`. The sessions exclude the first
  128 MiB, `[0x40000000, 0x48000000)`, which holds the image at
  `0x41000000`, the result block at `0x42000000`, the input block, the
  monitor's stack and the page tables. The validated bundle records this
  excluded range. ROM and FEL SRAM lie outside DDR, so no DDR pattern
  reaches them. Eleven sessions test 1 GiB each and the twelfth tests
  896 MiB, which covers 12,160 of 12,288 MiB, or 99.0%. Rank 3's reported
  `dram_mib` must equal 12,288 before the first session.
- *Pattern and placement.* Every 8-byte word holds its physical address
  XORed with a per-session seed, which detects address aliasing and stuck
  data bits. The image maps DDR as cacheable normal memory in 1 GiB
  blocks, with 2 MiB blocks at range edges. For each 2 MiB block it writes
  with NEON `STNP` pairs, then cleans and invalidates that block by virtual
  address, so the ascending verify pass reads DDR rather than cache. It does
  no per-page work and reads the counter once per 2 MiB block.
- *Window.* A 1 GiB session moves 2 GiB: one write and one read. The image
  stops itself 1.6 s after the monitor's start counter, 0.4 s before the
  forced return. Finishing in 1.5 s of working time needs 1.43 GB/s in each
  direction from one A55. That rate is an estimate of what one A55 sustains
  on LPDDR5; it has not been measured. A session that stops early reports
  the bytes tested and the measured rate, and the chain stops. A bundle
  re-prepared with smaller ranges is then a changed condition for a retry.
- *Result channel.* The image writes its result after the monitor's fields
  in the 16 KiB block at `0x42000000`. The host reads that block through
  FEL after the return, as `tools/check-a733-kernel.py` does for rank 4.
- *Expected result.* Monitor status 0; bytes tested equal to the range;
  zero mismatches; write and read bytes per second from the counter; the
  CPU clock that rank 6 reported. Status 2, a forced return, is a failure,
  and so is an early stop.

Single-core coverage is sufficient for this tier, because twelve sessions
reach 99.0% of DDR. A multicore image would add bandwidth measurement,
which [docs/validation.md](../validation.md) defers past bring-up. Its
secondary cores could also hang outside the monitor's return, which is the
risk of rank 7. The multicore image depends on the primary agent's startup
work, so this package ranks no row for it.

**Terminal rows.** Each expected result includes three counts per job:
completions observed by interrupt, completions observed by the one-tick
fallback in `pvr_gpu_service.c` or `npu_service.c`, and client wake-ups.
Zero fallback completions over a session is the evidence that removes the
tick wait under section 1.3 of
[docs/performance-architecture.md](../performance-architecture.md). Where
an image does not report these counts yet, the owning service package adds
them. The NPU evidence is one admitted model whose output matches its host
reference. The runner then retains the board until power removal.

## Data flow and ownership

1. **Inputs.** Each owning package builds its image under
   `~/.cache/sunstoneos/build/<package>/`. The tool builds nothing.
2. **Preparation.** For each row, the tool creates
   `~/.cache/sunstoneos/evidence/board-bundles-<date>/<row>/bundle/`;
   creation fails if the directory exists. The tool calls the module's
   `prepare()` into it and then `validate_bundle()`, the path's admission
   check. The tool adds no admission rule.
3. **Freezing.** The bundle stays where `prepare()` wrote it and is the
   runner's `--bundle` input, so its layout (for example `board/` and
   `assets/` beside the top-level `prepared.json`) is unchanged. The tool
   then runs `tools/evidence-snapshot.py` into the sibling `snapshot/`,
   with the bundle files as artifacts and the path's source list as
   sources. The snapshot is a preserved copy. The tool refuses the row if
   any listed source appears in the manifest's
   `worktree_status_before_copy`, so it runs no separate `git` pass.
4. **Read-only.** After the manifest is written, the tool removes write
   permission from every file and directory in `bundle/` and `snapshot/`.
5. **Submission.** The tool writes `submission.json`. Its artifacts are
   `prepared.json` and every file named in the validated `prepared.json`
   `files` map, each at its resolved bundle path, with the SHA-256 from the
   snapshot manifest. The runners compare the queue against the same names;
   for example, `claimed()` in
   `tools/check-a733-gpu-display-supervised.py` builds its expected set from
   that map. The submission therefore meets each runner's comparison
   without a copy of each runner's list. The runner's full `claimed()`
   check still runs at claim time against the coordinator's review, which
   does not exist during preparation. The tool loads
   `tools/board-test-queue.py` by path, which runs no code beyond
   definitions, and validates each submission with its `submission()`
   function.

## Interrupts, memory and synchronization

The tool runs once and polls nothing. On the board, Terminal rows return
evidence over the shared USB0 loop. That loop polls once per tick because
USB0 interrupt delivery is not qualified, a wait that section 1.3 permits.
Monitor rows use no interrupt and no USB0.

Bundles are small: the eight-core launch bundle is 548 KiB, and the
snapshot streams files in 1 MiB blocks. Load addresses and cache
maintenance come from each path's prepared session and launcher, which
`validate_bundle()` checks. Tests run one at a time.

The tool is one process. The snapshot already refuses a source that
changes during its copy and a revision that changes during the snapshot.
Exclusive directory creation keeps two runs apart. The tool adds no
board-side synchronization.

## Guard against board contact

Preparation modules load libraries that also contain USB code. Before
loading any of them, the tool installs a Python audit hook
(`sys.addaudithook`), which cannot be removed. The hook resolves every path
with `os.path.realpath` before matching it, and refuses:

- `open`, `os.listdir` and `os.scandir` on `/dev/bus/usb`,
  `/dev/char/189:*`, `/sys/bus/usb`, any `/sys/devices` path with a USB
  device component, and `/tmp/sunstoneos-a733-*.lock`;
- an import of `usb` and a `ctypes.dlopen` of libusb;
- `subprocess.Popen`, `os.exec*`, `os.posix_spawn` and `os.system` of any
  executable outside an allowlist: the row's `clang`, `lld` and `objcopy`,
  `git`, and the Python interpreter running `tools/evidence-snapshot.py`;
- `sqlite3.connect` of any database.

Audit hooks do not reach child processes, so the allowlist admits only
programs whose behavior is known. A refused row is left without a
manifest.

## Budgets and measurement

`checks/board-bundles.py` runs the tool on fixture rows and reads the real
table. Its text names neither the queue nor the board tools, so
`checks/check-index.py` indexes it as pure-host. The tool it runs installs
the guard before it loads the queue module.

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| Board-contact events during preparation | No guard in per-path preparation | Zero | One fixture per hook branch (path access, import or `dlopen`, process launch, database connect), each run against a table of forbidden names |
| Bytes copied for session-independent files (image, segments, launcher) when inputs are unchanged | 548 KiB for the eight-core launch bundle, by `du` | Zero | Snapshot manifest rows without `linked_from`. `--reuse` names the snapshots already written in the run and the newest earlier `board-bundles-*` snapshot of the same row |
| Copies of per-session files (`session.bin`, `request.bin`, `contract.bin`, `prepared.json`) | Copied | Always copied | `prepare()` draws a fresh epoch, generation and lease on every call |
| Writable entries in `bundle/` or `snapshot/` | Not applicable | Zero | `stat` modes in the check |
| Submissions refused by the queue's `submission()` | Not applicable | Zero | The tool's own call |
| Plan placement errors | Zero today | Zero | One regex reads the plan's ranked components. Native USB rows follow rank 9, Monitor rows lie between ranks 6 and 7, no Terminal row is in the ranked table, and no Excluded row has a bundle |
| Monitor rows linking USB or CPU-start code | Not applicable | Zero | Symbol scan, with one mutation fixture per symbol family |
| Memory session coverage and time | Not applicable | 1 GiB per session (896 MiB in the last), 12 sessions, each done within 1.6 s at 1.43 GB/s per direction | Range table in the check; on the board, bytes tested and counter time |
| Memory loop instructions per 64-byte line (write, clean, verify) | Not applicable | At most 16, with one counter read per 2 MiB | QEMU instruction count over one 2 MiB block in the memory package's host model |
| Queue timeout per row | Not applicable | Monitor: 30 s, which covers a 1 MiB upload at an estimated 1 MB/s FEL rate, the 2 s window and a 16 KiB read with a margin above 5×. Terminal: twice the sum of evidence transfer at 13 ticks per 64 KiB, the job budget, image upload and re-enumeration, and at least the runner's capture window | Computed by the tool; the row records evidence bytes and modeled ticks so the session can compare them |

No baseline above was measured on the board.

## Left out

- **Board contact.** The tool does not submit, claim, run a runner or read
  board state. The coordinator holds the queue token and the board lock.
- **CPU startup and multicore bundles.** A76 bring-up, CPU power states,
  the multicore memory image and the remaining multicore steps belong to
  the primary agent, which adds their rows in the same format.
- **A generated plan table.** Ranks 1 to 9 are ROM queries, `tools/run-*.py`
  runs and frozen ELFs with no preparation module. Generating the table
  would need a second, looser row schema to keep a few new rows in order.
- **Recovery-state tracking and retries.** Pending recovery and retained
  sessions belong to the queue and the coordinator. A failed bundle is
  prepared again only when an input changes.
- **Temperature in the memory sessions.** The image reads no device
  register, which keeps bus stalls out of the Monitor sessions. Rank 6
  reports temperature before the first session.
