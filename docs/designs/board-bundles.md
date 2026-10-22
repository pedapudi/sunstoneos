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

## Rows and recovery classes

The tool holds a fixed table of rows, and each row has one of three classes:

- **Monitor.** The runner is the returning-ROM transport
  `tools/a733-returning-host.py`. The image's EL3 monitor forces a return to
  the boot ROM's recovery mode (FEL) through its secure timer. That return
  holds while three conditions hold: no bus access stalls, no EL1 write
  reaches the protected window that holds the monitor's code, stack and
  state, and the system counter runs. The tool scans each Monitor image with
  `nm` and refuses one that defines a symbol starting with `dcd_`, `hcd_`,
  `tud_`, `tuh_`, `usb0_` (USB stacks and the shared USB0 loop),
  `a733_cpu_start_` or `a733_cpu_secondary_` (secondary-core power-up). It
  also refuses `monitor_sync`, the deadline-renewal handler, unless the image
  defines `monitor_renewal_cap`, the marker of the capped renewal requested
  below. This scan is the tool's only safety check of its own.
- **Terminal.** Every other runner. The supervised runners keep the A733 port
  lock after every outcome until whole-board power removal is confirmed
  (`run_session` in `tools/check-a733-smp-supervised.py`). A Terminal test
  ends the board session, so none runs while the board cannot be reconnected.
- **Excluded.** The test needs an accessory, a voltage or clock change, or an
  intentional hang. The tool prepares no bundle.

| Plan position | Rows | Class |
| --- | --- | --- |
| Waiting for an independent watchdog record; then after rank 7, before rank 8 | Memory qualification: three map steps and one final run | Monitor |
| Waiting for power removal, one per board session, in this order | NPU service in the OS image; GPU service in the OS image; GPU and NPU jobs under the memory and power arbiter; recognition dispatcher | Terminal |
| Waiting for physical recovery | GPU and NPU hang reset; voltage and clock changes; display output | Excluded |

**Monitor gate.** The queue accepts a peripheral test only with the
prerequisite `independent-watchdog-rom-return` or `supervised-ram-recovery`.
No record qualifies for the first ([docs/a733-smp.md](../a733-smp.md)), and
the watchdog-expiry trial that would produce one waits for physical recovery.
The monitor's timer cannot stand in for it: the watchdog record covers
recovery from an interconnect stall, and a stall on the first access to
untested DDR is the risk the monitor leaves open. The memory rows therefore
wait. Running them earlier needs the queue's owner to define a monitor-timer
prerequisite and the user to accept that uncovered stall in writing; the tool
then changes one constant.

**Position.** The plan places known results before new code and narrower
failure modes before wider ones. Rank 7 re-runs a frozen ELF that has passed,
so it comes first. A memory run can lose the connection only through a bus
stall, while rank 8 can also hang a secondary core outside any forced return,
so memory comes before rank 8. Memory submissions require ranks 1 to 7 in the
same power cycle, with rank 3 reporting `dram_mib` equal to 12,288 and rank 6
reporting the boot clock.

NPU comes first among the Terminal rows because GPU firmware writes memory on
its own ([docs/remote-writers.md](../remote-writers.md)), so a GPU failure has
more causes to separate.

## Memory runs

The memory qualification design
([docs/designs/memory-qualification.md](memory-qualification.md)) owns the
tests, loops, sub-slice size, cache maintenance, barriers, instruction budgets
and result record. Each run lasts at most 20 s, renewing the monitor's 2 s
deadline with `SMC` 0x734 every 200 ms. This design fixes only the map limit
of each run and how the runs chain.

**Capped renewal.** Today `monitor_sync` exists only in builds with
`SUNSTONE_USB_RUNTIME`, and it renews without limit: every `SMC` 0x734 sets
`CNTPS_CVAL_EL1` to the current count plus 48,000,000 ticks (2 s at 24 MHz)
([platform/a733/monitor.S](../../platform/a733/monitor.S)). An image that hung
while still renewing would never return. The memory target needs a renewal
handler that sets the deadline to the earlier of the current count plus 2 s
and the entry count plus 22 s. A hang then returns at most 22 s after entry,
inside the host's 30 s return window. Without that handler the tool prepares
no memory bundle.

**Map steps.** The run contract names a map limit, and the image keeps every
translation at or above it invalid, so speculative and prefetch accesses stay
below it. The alias probe and the address-line test use only partners below
the limit; the probe still runs first in every run.

| Step | Tested range | Map limit | Offset bits probed and paired | New risk in its first run |
| --- | --- | --- | --- | --- |
| 1 | `0x4400_0000`–`0x8000_0000`, 960 MiB | `0x8000_0000` | 6–29 | DDR outside the two checked pages |
| 2 | `0x8000_0000`–`0x1_0000_0000`, 2,048 MiB | `0x1_0000_0000` | 6–31 | Physical bit 31 |
| 3 | `0x1_0000_0000`–`0x3_4000_0000`, 9,216 MiB | `0x3_4000_0000` | 6–33 | First access above 4 GiB |

The steps cover 12,224 of 12,288 MiB; the 64 MiB protected window at
`0x4000_0000` holds boot0 and launcher memory, the image, the monitor and the
result record. The first run of step 3 probes bits 32 and 33 before any other
write above 4 GiB.

**Runs.** The first run of each step writes the page signature over every
tested page below its limit, then marches; step 1's run also carries the
bandwidth and latency harness. Later runs of step 3 march from the recorded
resume address. A final run repeats the page signature over the tested map
with a new seed, as the stability check. If a DMA admission record exists, it
also runs the DMA0 cases: the bit 32 and bit 33 guard matrix and the two cache
maintenance cases, at most 8 transfers of at most 1 MiB. Every run uploads
the whole image and its 64-byte contract, so no run depends on image bytes
that an earlier run left in DDR.

**Chaining.** The tool prepares run k+1 from run k's record, which the
coordinator saved; this is host-only work. Only a record with an
image-initiated exit and every field present chains. A forced return from the
monitor, an abort or an incomplete record stops the chain for review. The
coordinator may stop at any run boundary; coverage is the union of the
recorded ranges. The address-line and page-signature passes of step 3 cover
decoding across the full map, so a high address bit that maps one range onto
another fails there even if every march passes.

**Run count.** From memory qualification's estimate of 0.9 to 1.6 s per
256 MiB sub-slice: step 1 marches 960 MiB in 3.4 to 6.0 s, and step 2
marches 2,048 MiB in 7.2 to 12.8 s, one run each. Step 3 marches 36
sub-slices in 32 to 58 s, which needs 2 to 4 runs at 18.5 s in the first run
(after the 1.5 s signature) and 19 s later. With the final run, the chain is
5 to 7 runs, all *estimated*.

**Requests to the memory qualification package.**

- Take the map limit, time budget and seed from the contract.
- Keep translations at or above the limit invalid, and set `TCR_EL1.IPS` to
  at least 36 bits.
- Limit alias-probe and address-line partners to addresses below the limit.
- Add a Unicorn fixture in which a step 1 contract makes no access at or
  above `0x8000_0000`.
- Provide `validate_bundle()` and `prepare(previous_record)`, which applies
  the chaining rule above.

**Request to the primary agent.** Build the capped renewal handler for the
memory target without `SUNSTONE_USB_RUNTIME`. Record the entry count where
EL1 cannot write it, and define `monitor_renewal_cap`. The level-1 table
words remain the request that memory qualification already makes.

## Data flow and ownership

1. **Inputs.** Each owning package builds its image under
   `~/.cache/sunstoneos/build/<package>/`. The tool builds nothing.
2. **Run directory.** The outer process creates
   `~/.cache/sunstoneos/evidence/board-bundles-<date>-<n>/` exclusively,
   then runs the preparation step once in the sandbox described below. It
   loads no preparation module itself.
3. **Preparation.** For each row, the step calls the module's `prepare()`
   into `<row>/bundle/`. The bundle layout stays what the runner's `--bundle`
   expects; the OS-image bundle keeps its files under `board/` and `assets/`.
4. **Freezing.** The step removes write permission from every entry under
   `bundle/`, which stays the only copy of each file.
5. **Admission.** The step calls `validate_bundle()` once. It is the path's
   admission rule and hash check, and the tool adds none.
6. **Artifacts.** The step walks `bundle/` recursively and hashes each
   regular file once, which gives the submission's artifact list for any
   layout.
7. **Snapshot.** `tools/evidence-snapshot.py` writes `<row>/snapshot/` with
   the path's sources. The tool refuses the row if a listed source appears in
   `worktree_status_before_copy`, and takes the source revision from the
   manifest.
8. **Submission.** The kind is `peripheral`. Terminal prerequisites are the
   runner's `PREREQUISITES` set, copied from the runner. Monitor prerequisites
   are `independent-watchdog-rom-return`, plus `memory-run-previous` naming
   the previous record for every chained run. The tool validates the result
   with `submission()` from `tools/board-test-queue.py`, loaded by path.

At claim time, each Terminal runner's `claimed()` re-hashes the bundle and
checks its prerequisite bindings; the Monitor transport re-runs
`validate_bundle()` in `ready()` before launch. The tool adds no later check.

Terminal procedures require ranks 1 to 5 and rank 9 earlier in the same power
cycle. Terminal expected results include, per job, completions observed by
interrupt, completions observed by the one-tick fallback in
`pvr_gpu_service.c` or `npu_service.c`, and client wake-ups. Zero fallback
completions over a session is the evidence that removes that tick wait under
section 1.3 of [docs/performance-architecture.md](../performance-architecture.md).
The NPU evidence is one admitted model whose output matches its host
reference.

## Guard against board contact

Preparation modules load code that also drives USB: `apps/npu-finite/host.py`
and `apps/gpu-finite/prepare.py` load `tools/check-a733-smp*.py` and
`tools/usb-access.py`. The preparation step therefore runs under
`/usr/bin/bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /sys --tmpfs /tmp
--tmpfs /run --bind <run directory> <run directory> --unshare-all
--die-with-parent --clearenv` with `PATH`, `HOME`, `LANG` and
`PYTHONDONTWRITEBYTECODE=1` set explicitly. Inside it, `/dev` holds only core,
fd, full, null, ptmx, pts, random, shm, stderr, stdin, stdout, tty, urandom
and zero; `/sys`, `/tmp` and `/run` are empty, so no session bus socket is
reachable; the queue databases are read-only; and there is no network.
Child processes, including the `git` that `evidence-snapshot.py` runs, see
the same view. If `bwrap` is missing or fails to start, the tool stops.

## Interrupts, memory and synchronization

The tool polls nothing; the outer process blocks in one wait on the sandbox.
Bundles are small (the eight-core launch bundle is 548 KiB), and hashing
streams 1 MiB blocks. On the board, interrupts, placement and cache policy
belong to each path. For memory runs they are those of the memory
qualification design; the tool checks only the map limit and protected window
against the step table and the contract's 20 s clamp. Terminal rows return
evidence over the shared USB0 loop, which polls once per tick because USB0
interrupt delivery is not qualified, a wait that section 1.3 permits.

Memory runs use one Cortex-A55 at the 494 MHz boot clock, as memory
qualification's Left out section requires until the primary agent qualifies a
returning multicore base. With eight per-core slices, and if DRAM sustains
their sum, the march would take 6 to 10 s and the chain 4 runs instead of 5
to 7, saving up to about 8 minutes, mostly USB churn wait (*estimated*). On
the host, exclusive creation of the run directory keeps two runs apart, and
the snapshot refuses a source or revision that changes during its copy.

## Budgets and measurement

`checks/board-bundles.py` runs the tool on fixture rows, including a nested
OS-image layout. It names the tool, whose name matches the device-access
pattern in `checks/check-index.py`, so the index lists it as device-capable
and runs it only when named. No fixture names a USB path or lock file, so a
broken sandbox fails the check instead of reaching a device.

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| Device nodes and entries in `/sys`, `/tmp` and `/run` visible to preparation and its children | Full host view | The 14 `/dev` entries above; the other three empty; only the four set environment variables | A fixture lists them from the tool and from a child process |
| Sandbox start time | 31 ms for `bwrap` running `sh`, on this host on 2026-10-22 | 100 ms, one sandbox per run | Timed in the check |
| Bytes the tool reads from `bundle/` outside `validate_bundle()` | Not applicable | The bundle size, one pass | `rchar` in `/proc/self/io` around the artifact pass |
| Writable entries in `bundle/` when `validate_bundle()` runs | Not applicable | Zero | `stat` modes |
| Copies of each bundle file | One, written by `prepare()` | One | The snapshot holds sources only |
| Submissions refused by `submission()` | Not applicable | Zero | The tool's own calls |
| Monitor images with a USB or CPU-start prefix, or `monitor_sync` without the cap | Not applicable | Zero | `nm` scan; fixtures inject each case |
| DDR tested | 8 KiB, rank 3 | 12,224 of 12,288 MiB | Step table against the recorded protected window; on the board, the union of recorded ranges |
| Memory runs per chain | Not applicable | 5 to 7 *estimated* | Memory qualification's planner with the step limits; board records replace the estimate |
| Time from entry to forced return | 2 s, uncapped by renewal in USB builds | 22 s | Primary agent's monitor model; board record ticks |
| Fixed cost per run: upload, launch, re-enumeration, record read | 2.0 s *estimated*: the 2026-10-15 forced-stall run took 4.0 s from its first read to its result file, of which 2 s was the deadline | 3 s | Evidence file times; board journals replace the estimate |
| USB re-enumerations per chain | Not applicable | 10 to 14 (two per run) | Count of `guard.reenumeration` reservations |
| Chain wall time | Not applicable | 12 min: about 3 min for six runs, about 10 min for seven because the seventh waits for the 12-per-10-minute cap (*estimated*) | Replay of the run schedule through `wait_needed` in `~/git/mimus/tools/usb_guard.py` |
| Queue timeout | Not applicable | Monitor 1,200 s; Terminal the runner's `capture_seconds` (19 or 59 s) plus 60 s | Computed by the tool; the check confirms 1 to 3,600 s and at least the runner's internal maximum |

The Monitor timeout covers the runner's worst case in one attempt: 615 s for
the churn guard to admit two re-enumerations (the 600 s window plus 15 s
spacing), 435 s of transaction and return bounds (120 s each for the upload,
contract write and record read, 15 s each for three short commands, and the
30 s return window), and 150 s of margin. A stop during the guard wait comes
before launch and leaves the board in FEL. No figure above was measured on
the board.

## Left out

- **Board contact.** The tool does not submit, claim, run a runner or read
  board state. The coordinator holds the queue token and the board lock.
- **Short runs without renewal.** Runs of 1.6 s would need 42 to 100
  sessions, 84 to 200 re-enumerations of a board that cannot be reconnected
  and at least 70 minutes of churn wait. Their 64 MiB sub-slices would also
  weaken memory qualification's bound on cache lines surviving a march
  element from below e^-32 to about e^-8. If capped renewal is not built, the
  memory rows stay unprepared.
- **CPU startup and multicore bundles.** They belong to the primary agent,
  which adds their rows in the same format and may add a recovery class with
  its first row that needs one.
- **A generated plan table.** Ranks 1 to 9 are ROM queries, `tools/run-*.py`
  runs and frozen ELFs with no preparation module; generating the table would
  need a second row schema.
- **Recovery tracking and retries.** They belong to the queue and the
  coordinator. A failed bundle is prepared again only when an input changes.
