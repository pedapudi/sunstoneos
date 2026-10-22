# Board-test bundles for the memory, GPU and NPU paths

Status: designed; not implemented. Bundles run only on the board, so every
path they carry stays implemented and host-modeled, with physical behavior
pending, until a board session records the evidence named in its row.

One host tool, `tools/prepare-board-bundles.py`, turns the build output of
each new memory, GPU and NPU path into a read-only bundle and a board-test
queue submission. It never contacts the board and never submits. The
hand-written ranked table in
[docs/audit/board-test-plan.md](../audit/board-test-plan.md) orders the tests
by their risk of losing the USB connection.

## Recovery classes

Each row has one of three classes, which the tool derives from the row's
runner and image:

- **Monitor.** The runner is the returning-ROM transport
  `tools/a733-returning-host.py`. The validated bundle records one core and a
  result record that the host reads through FEL. The image defines no symbol
  with the prefix
  `dcd_`, `hcd_`, `tud_`, `tuh_`, `usb0_` (USB stacks and the shared USB0
  loop), `a733_cpu_start_` or `a733_cpu_secondary_` (secondary-core power-up),
  and no `monitor_sync`. `monitor_sync` handles `SMC` 0x734, which renews the
  monitor's deadline; it exists only in builds with `SUNSTONE_USB_RUNTIME`
  ([platform/a733/monitor.S](../../platform/a733/monitor.S)). Without it, the
  monitor's secure timer forces a return to the boot ROM's recovery mode
  (FEL) 2 s after entry, whatever the image does, unless a bus access stalls.
  The symbol scan stays even though the bundle reports one core, because that
  field is written by the image's own preparation module.
- **Terminal.** Every other runner. The supervised runners keep the A733 port
  lock after every outcome until whole-board power removal is confirmed
  (`run_session` in `tools/check-a733-smp-supervised.py`). A Terminal test
  ends the board session, so none runs while the board cannot be reconnected.
- **Excluded.** The test needs an accessory, a voltage or clock change, or an
  intentional hang. The tool prepares no bundle.

| Plan position | Bundle | Class |
| --- | --- | --- |
| After rank 6, before rank 7 | Memory qualification image: three map steps, then one DMA reachability session | Monitor |
| Waiting for power removal, one per board session, in this order | NPU service in the OS image; GPU service in the OS image; GPU and NPU jobs under the memory and power arbiter; recognition dispatcher | Terminal |
| Waiting for physical recovery | GPU and NPU hang reset; voltage and clock changes; display output | Excluded |

NPU comes first among the Terminal rows because GPU firmware writes memory on
its own ([docs/remote-writers.md](../remote-writers.md)), so a GPU failure has
more causes to separate.

## Memory sessions

The memory qualification image, specified in the memory qualification
package's `docs/designs/memory-qualification.md`, owns the tests, loops, cache maintenance and result
record. This design fixes only how its runs are split into Monitor sessions.

**Assumption.** A CPU access to DDR inside the trained 12,288 MiB completes.
Board evidence covers two pages; the assumption stays unverified for each map
step until that step's first session passes. Rank 3 must report `dram_mib`
equal to 12,288 first.

**Map steps.** The run contract names a map limit, and the image keeps every
translation above it invalid, so speculative and prefetch accesses stay below
it. The protected window and the contract address come from the image's
`validate_bundle()` record; the tool compares the window with the step table.

| Step | Tested range | Map limit | Address bits paired | First session's new risk |
| --- | --- | --- | --- | --- |
| 1 | `0x4400_0000`–`0x8000_0000`, 960 MiB | `0x8000_0000` | 6–29 | DDR outside the two checked pages |
| 2 | `0x8000_0000`–`0x1_0000_0000`, 2,048 MiB | `0x1_0000_0000` | 6–31 | Physical bit 31 |
| 3 | `0x1_0000_0000`–`0x3_4000_0000`, 9,216 MiB | `0x3_4000_0000` | 6–33 | First access above 4 GiB |

The steps cover 12,224 of 12,288 MiB; the 64 MiB protected window at
`0x4000_0000` holds the image and result record. No session crosses 4 GiB.

**Sessions.** Each step starts with a decoding session: data lines, the
address-line pairs above, and the page signature (one word per 4 KiB page)
over every tested page below the map limit. March sessions follow. Every
session uses one data seed, so a word's expected value depends only on its
address. Each session stops itself 1.6 s after the monitor's start counter, 0.4 s before the forced return, and records a resume
address. Reaching that time is not a failure. The tool prepares session k+1
from session k's record, which the coordinator saved; this is host-only work.
If the step 3 signature, 3,129,344 pages, does not finish in one session, the
next session continues it. DDR contents survive the monitor's return, as the
`--ram-input-present` path of `tools/check-a733-kernel.py` already requires.

**Coverage claim.** Each march session covers data faults and decoding within
its range. The address-line and page-signature passes of step 3 cover decoding
and aliasing across the full map, so a high address bit that maps one range
onto another fails there even though each march session passes.

**Session count.** March C- makes ten accesses per word. The memory
qualification design estimates 1.0 to 2.5 GB/s from one A55 at the 494 MHz
boot clock. Each record carries the CPU and DSU PLL words from
`a733_cpu_clock_report`, and rank 6 reports the boot clock first. With 64 MiB
sub-slices, about 1.5 s of march fits two to five sub-slices, 128 to 320 MiB.
The chain then needs 39 to 96 march sessions and three decoding sessions,
plus one if the step 3 signature continues. Each session ends in a ROM
re-enumeration, the path ranks 2, 4 and 5 already use.
The coordinator may stop at any session boundary; coverage is the union of the
recorded ranges.

**DMA reachability.** After step 3, one session runs the image's DMA0 and
crypto-engine cases with an admission record. Transfers are at most 1 MiB. If
a DMA0 deadline expires, the host makes no further upload until a FEL read
shows the channel quiet, as the memory qualification design requires.

**Requests to the memory qualification package.**

- Build the Monitor image without the renewal path.
- Take the time budget and map limit from the contract, and keep every
  translation above the limit invalid.
- Use march sub-slices of at most 64 MiB and set `TCR_EL1.IPS` to at least
  36 bits.
- Provide `prepare(previous_record)` and `validate_bundle()`.

The level-1 table words need the primary agent, as that design already states.

## Data flow and ownership

1. **Inputs.** Each owning package builds its image under
   `~/.cache/sunstoneos/build/<package>/`. The tool builds nothing.
2. **Run directory.** The outer process creates
   `~/.cache/sunstoneos/evidence/board-bundles-<date>-<n>/` exclusively,
   then runs the preparation step once in the sandbox described below. It
   loads no preparation module itself.
3. **Preparation.** For each row, the step calls the module's `prepare()`
   into `<row>/bundle/` and then `validate_bundle()`, the path's admission
   check. The tool adds no admission rule. The bundle layout stays what the
   runner's `--bundle` expects.
4. **Freezing.** One hashing pass over `bundle/` must match the validated
   `prepared.json` `files` map with no extra file. The tool then removes write
   permission from every entry in `bundle/`, which stays the only copy.
5. **Snapshot.** `tools/evidence-snapshot.py` writes `<row>/snapshot/` with
   the path's sources and one artifact, `prepared.json`, which binds every
   bundle hash. The tool refuses the row if a listed source appears in
   `worktree_status_before_copy`, and takes the source revision from the
   manifest.
6. **Submission.** `submission.json` lists `prepared.json` and every file in
   the `files` map at its bundle path, with the hashes from step 4. The kind
   is `peripheral`. Terminal prerequisites equal the runner's `PREREQUISITES`
   set, read from the runner. Where a runner binds a prerequisite to a bundle
   file, as `tools/check-a733-gpu-display-supervised.py` binds
   `supervised-ram-recovery` to `bundle/admission.json`, the tool checks that
   the file is in the `files` map. Monitor prerequisites are
   `independent-watchdog-rom-return`, plus `memory-session-previous` naming
   the previous record for every chained session. The tool validates the
   result with `submission()` from `tools/board-test-queue.py`, loaded by path.
   The runner's `claimed()` re-hashes the bundle at claim time.

Each submission's procedure requires ranks 1 to 5 to pass earlier in the same
power cycle; Terminal rows also require rank 9. Terminal expected results include,
per job, completions observed by interrupt, completions observed by the
one-tick fallback in `pvr_gpu_service.c` or `npu_service.c`, and client
wake-ups. Zero fallback completions over a session is the evidence that
removes that tick wait under section 1.3 of
[docs/performance-architecture.md](../performance-architecture.md). The NPU
evidence is one admitted model whose output matches its host reference.

## Guard against board contact

Preparation modules load code that also drives USB: `apps/npu-finite/host.py`
and `apps/gpu-finite/prepare.py` load `tools/check-a733-smp*.py` and
`tools/usb-access.py`. The preparation step therefore runs under
`/usr/bin/bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /sys --tmpfs /tmp
--bind <run directory> <run directory> --unshare-all --die-with-parent`. Inside
it, `/dev` holds only core, fd, full, null, ptmx, pts, random, shm, stderr,
stdin, stdout, tty, urandom and zero; `/sys` and `/tmp` are empty; the queue
databases are read-only; and there is no network. Child processes, including
the `git` that `evidence-snapshot.py` runs, see the same view. If `bwrap` is
missing or fails to start, the tool stops.

## Interrupts, memory and synchronization

The tool polls nothing; the outer process blocks in one wait on the sandbox.
Monitor sessions use the platform's 1 kHz tick and one counter read per 2 MiB
block, and no USB0. Terminal rows return evidence over the shared USB0 loop,
which polls once per tick because USB0 interrupt delivery is not qualified, a
wait that section 1.3 permits.

On the host, bundles are small (the eight-core launch bundle is 548 KiB) and
hashing streams 1 MiB blocks. On the board, placement and cache policy belong
to each path. The memory image maps tested DDR as Normal write-back
inner-shareable and keeps its result record in the non-cacheable 2 MiB block
at `0x4200_0000`. At the end of each march element it cleans and invalidates
at most the cache size from `CCSIDR_EL1`. It issues `DSB SY` before each
read-back and before the exit `SMC`.

Every board test runs on one core at a time. On the host, exclusive creation of
the run directory keeps two runs apart, and the snapshot refuses a source or
revision that changes during its copy.

## Budgets and measurement

`checks/board-bundles.py` runs the tool on fixture rows and reads the real plan
table. It names the tool, whose name matches the device-access pattern in
`checks/check-index.py`, so the index lists it as device-capable and runs it
only when named. No fixture names a USB path or lock file, so a broken sandbox
fails the check instead of reaching a device.

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| Device nodes, sysfs entries and `/tmp` entries visible to preparation and its children | Full host view | The 14 `/dev` entries above; empty `/sys` and `/tmp` | One fixture lists `/dev`, `/sys` and `/tmp` inside the sandbox, from the tool and from a child process |
| Sandbox start time | 31 ms for `bwrap` running `sh`, measured on this host on 2026-10-22 | 100 ms per run, one sandbox per run | Timed in the check |
| Copies of each bundle file | One, written by `prepare()` | One | Snapshot manifest lists `prepared.json` and sources only |
| Hashing passes over bundle files during preparation | Not applicable | One | Counted in the tool |
| Writable entries in `bundle/` | Not applicable | Zero | `stat` modes |
| Submissions refused by `submission()`, or prerequisite sets that differ from the runner's | Not applicable | Zero | The tool's own calls |
| Plan placement errors | Zero | Zero | Monitor rows lie between ranks 6 and 7; no Terminal row is in the ranked table; no Excluded row has a bundle |
| Monitor images linking USB, CPU-start or renewal code | Not applicable | Zero | `nm` prefix scan; one fixture injects one forbidden prefix, and a table check matches the prefix list to the families above |
| DDR tested | 8 KiB, rank 3 | 12,224 of 12,288 MiB in three steps | Step table against the image's recorded protected window; on the board, the union of recorded ranges |
| Session time | Not applicable | Self-stop 1.6 s after monitor start; 42 to 100 memory sessions at 1.0 to 2.5 GB/s *estimated*, 494 MHz CPU | Memory qualification's Unicorn budget logic with a modeled counter; on the board, ticks and bytes per record with the rank 6 clock words |
| Maintenance per march element; instructions per 64-byte line | Not applicable | At most 8 MiB of `DC CIVAC` and one `DSB SY`; write ≤ 8, verify ≤ 12, read-verify-write ≤ 16 | Memory qualification's Unicorn counters |
| Queue timeout | Not applicable | Monitor 30 s; Terminal the runner's `capture_seconds` (19 or 59 s) plus 60 s | Computed by the tool; the check confirms 1 to 3,600 s |

No figure above was measured on the board.

## Left out

- **Board contact.** The tool does not submit, claim, run a runner or read
  board state. The coordinator holds the queue token and the board lock.
- **A renewing memory image.** Renewal would cut the chain to the 4 to 7 runs
  of up to 20 s that the memory qualification design plans, but an image that
  hangs while renewing never returns, and the board cannot be reconnected.
- **The bandwidth and latency harness.** Its 256 MiB samples do not fit a
  1.6 s session, and [docs/validation.md](../validation.md) defers physical
  performance runs; march records give a sequential lower bound.
- **CPU startup and multicore bundles.** They belong to the primary agent,
  which adds their rows in the same format and may add a recovery class with
  its first row that needs one.
- **A generated plan table.** Ranks 1 to 9 are ROM queries, `tools/run-*.py`
  runs and frozen ELFs with no preparation module; generating the table would
  need a second row schema.
- **Recovery tracking and retries.** They belong to the queue and the
  coordinator. A failed bundle is prepared again only when an input changes.
