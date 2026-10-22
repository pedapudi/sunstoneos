# Board-test bundles for the memory, GPU and NPU paths

Status: the tool and its host check are **implemented**
([measurements](board-bundles-measurements.md)). Bundles run only on the
board, so every path they carry stays implemented and host-modeled, with
physical behavior pending, until a board session records the evidence named in
its row. No row has been prepared from real inputs, and the tool refuses
every row today. The memory profile (`apps/memtest/check.py`) and the capped
renewal do not exist yet. `apps/os/prepare.py` does not yet admit the GPU
service, NPU service or dispatcher builds, and the arbiter is not
implemented. No owning package has named the admission values that identify
its row's build, and no Terminal runner yet reports the per-job completion
and wake-up counts.

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
  holds while three conditions hold: no bus access stalls, no CPU or DMA write
  reaches the monitor's code, vectors, stack or state, and the system counter
  runs. The tool runs `nm` on the bundle's `image.elf`, the file whose
  loadable segments `validate_bundle()` binds to the uploaded bytes. It
  refuses an ELF with no symbol table or without `monitor_entry`, and one that
  defines a symbol starting with `dcd_`, `hcd_`, `tud_`, `tuh_`, `usb0_` (USB
  stacks and the shared USB0 loop), `a733_cpu_start_` or `a733_cpu_secondary_`
  (secondary-core power-up). Memory runs always renew the deadline, so a
  memory image must also define `monitor_renewal_cap`, the marker of the
  capped renewal requested below.
- **Terminal.** Every other runner. The supervised runners keep the A733 port
  lock after every outcome until whole-board power removal is confirmed
  (`run_session` in `tools/check-a733-smp-supervised.py`). A Terminal test
  ends the board session, so none runs while the board cannot be reconnected.
- **Excluded.** The test needs an accessory, a voltage or clock change, or an
  intentional hang. The tool prepares no bundle.

| Plan position | Rows | Class |
| --- | --- | --- |
| Waiting for an independent watchdog record; then after rank 7, before rank 8 | Memory qualification: three map steps | Monitor |
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

**Position.** The plan places known results before new code. Rank 7 re-runs a
frozen ELF that has passed, so it comes first. A memory run can lose the
connection through a bus stall. It can also lose it through a non-linear
address decode that turns a write to tested DDR into a write to the monitor;
memory qualification's alias probe cannot detect such a decode, and its page
signature only bounds the damage. Rank 8 can also hang a secondary core outside any forced return.
Placing memory before rank 8 is a judgement that the decode fault is less
likely than a secondary-core hang under load. Memory submissions require
ranks 1 to 7 in the same power cycle, with rank 3 reporting `dram_mib` equal
to 12,288 and rank 6 reporting the boot clock.

NPU comes first among the Terminal rows because GPU firmware writes memory on
its own ([docs/remote-writers.md](../remote-writers.md)), so a GPU failure has
more causes to separate.

## Memory runs

The memory qualification design
([docs/designs/memory-qualification.md](memory-qualification.md)) owns the
tests, loops, sub-slice size, cache maintenance, barriers, instruction
budgets, result record, the step table below and the 20 s run clamp. Each run
renews the monitor's 2 s deadline with `SMC` 0x734 every 200 ms. The table
describes the plan; memory qualification's `prepare()` derives each run's
step and its `validate_bundle()` enforces it.

**Capped renewal.** Today `monitor_sync` exists only in builds with
`SUNSTONE_USB_RUNTIME`, and it renews without limit: every `SMC` 0x734 sets
`CNTPS_CVAL_EL1` to the current count plus 48,000,000 ticks (2 s at 24 MHz)
([platform/a733/monitor.S](../../platform/a733/monitor.S)). An image that hung
while still renewing would never return. The memory target needs a renewal
handler that sets the deadline to the earlier of the current count plus 2 s
and the entry count plus 22 s. A hang then returns at most 22 s after entry,
inside the host's 30 s return window. Without that handler the tool prepares
no memory bundle.

**Map steps.** The run contract names a map limit. The level-1 entries at or
above it are never valid while `SCTLR_EL1.M` is 1, so speculative and
prefetch accesses stay below it from the moment the MMU turns on. The alias
probe and the address-line test use only partners below the limit; the probe
still runs first in every run.

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
resume address. The last run of step 3 then repeats the page signature over
the tested map with a new seed, as the stability check. If a DMA admission
record exists, it also runs the DMA0 cases: the bit 32 and bit 33 guard
matrix and the two cache maintenance cases, at most 8 transfers of at most
1 MiB.
`prepare(previous_record)` places them there when the recorded resume
address shows the remaining march leaves at least 1.6 s (the 1.5 s signature
plus eight 10 ms DMA0 deadlines). Otherwise it prepares a separate final run
for them. Every run uploads the whole image and its 64-byte contract, so no
run depends on image bytes that an earlier run left in DDR.

**Chaining.** The coordinator saves each run's record, and the tool prepares
the next run from it; this is host-only work. A record chains only if it
shows an image-initiated exit, every field present, an alias-probe pass, an
address-line pass, a page-signature match, no external abort and no expired
DMA0 deadline. `prepare()` embeds the previous record's hash in the contract,
and `validate_bundle()` checks it. Any other record retains the board: no
further upload of any kind, including rank 8 and the Terminal rows, until the
user reviews the record, and after a DMA0 expiry not until a FEL read also
shows DMA0 quiet. The memory rows' `stop_condition` and the plan state this
rule. The coordinator may stop at any run boundary; coverage is the union of
the recorded ranges. The address-line and page-signature passes of step 3
cover decoding across the full map, so a high address bit that maps one range
onto another fails there even if every march passes.

**Run count.** From memory qualification's estimate of 0.9 to 1.6 s per
256 MiB sub-slice: step 1 marches 960 MiB in 3.4 to 6.0 s, and step 2
marches 2,048 MiB in 7.2 to 12.8 s, one run each. Step 3 marches 36
sub-slices in 32 to 58 s, which needs 2 to 4 runs at 18.5 s in the first run
(after the 1.5 s signature) and 19 s later. Its last run has 1.0 to 15.7 s
spare, so the separate final run is needed only near 1.0 or 1.5 s per
sub-slice. The chain is 4 to 6 runs, all *estimated*.

**Requests to the memory qualification package.**

- Own the step table and the 20 s clamp: `prepare(previous_record)` derives
  the step and applies the chaining rule above, and `validate_bundle()`
  rejects a contract whose map limit, protected window or time budget is not
  in the table, or whose previous-record hash does not match.
- Take the map limit, time budget and seed from the contract, and limit
  alias-probe and address-line partners to addresses below the limit.
- Make the mapping check's translation count and stride, and the host's
  golden-word comparison, use the step's limit.
- Add Unicorn fixtures in which a step 1 contract makes no access at or above
  `0x8000_0000`, and in which `prepare()` refuses each retaining record.

**Request to the primary agent.** Build the capped renewal handler for the
memory target without `SUNSTONE_USB_RUNTIME`, record the entry count where
EL1 cannot write it, and define `monitor_renewal_cap`. In the memory target,
`platform_init` reads the map limit from the contract before it writes the
level-1 table, and makes valid only the entries of memory qualification's
request that lie below the limit, before it enables the MMU. A Unicorn
fixture asserts that no valid descriptor at or above the limit exists when the
MMU is first enabled. `TCR_EL1.IPS` needs no change: `platform.c` already
sets it to 2, which is 40 bits.

## Data flow and ownership

1. **Inputs.** Each owning package builds its image under
   `~/.cache/sunstoneos/build/<package>/`. The tool builds nothing. The
   coordinator passes one JSON file that maps each row name to the keyword
   arguments of that row's `prepare()`, without `output`; the tool turns
   every string into a path and adds `output`. A row without inputs is
   recorded as such and skipped.
2. **Run directory.** The outer process creates
   `~/.cache/sunstoneos/evidence/board-bundles-<date>-<n>/` exclusively,
   then runs the preparation step once in the sandbox described below. It
   loads no preparation module itself.
3. **Preparation and admission.** For each row, the step calls the module's
   `prepare()` into `<row>/bundle/`. Every `prepare()` ends by returning
   `validate_bundle(output)`, the path's admission rule and hash check; the
   step takes that value as the admission result. Each row names, in
   `admission`, the fields of that value that identify its build, such as
   the workload or service flag `apps/os/prepare.py` records for the NPU
   service build, or the arbiter's schema. The step refuses the row when a
   field differs, so an image admitted for another path, such as a mimus OS
   build offered as the NPU service row, cannot carry that row's expected
   result. A row whose `admission` is `None` is refused; every row is `None`
   until its owning package names those values. The bundle layout stays
   what the runner's `--bundle` expects; the OS-image bundle keeps its files
   under `board/` and `assets/`. A refusal at any stage refuses only its row;
   `run.json` records each row's outcome, and the tool exits with status 1
   when any row was refused.
4. **Freezing.** The step removes write permission from every entry under
   `bundle/`, which stays the only copy of each file.
5. **Artifacts.** The step walks `bundle/` recursively and hashes each
   regular file once, which gives the submission's artifact list for any
   layout. It writes that list to `<row>/artifacts.json`.
6. **Snapshot.** `tools/evidence-snapshot.py` writes `<row>/snapshot/` with
   the path's sources and `artifacts.json` as its single `--artifact`. The
   tool refuses the row if a listed source appears in
   `worktree_status_before_copy`, and takes the source revision from the
   manifest.
7. **Submission.** The kind is `peripheral`. Terminal prerequisites are the
   runner's `PREREQUISITES` set, copied from the runner. The Monitor
   prerequisite is `independent-watchdog-rom-return`. The queue timeout is
   1,200 s for a Monitor row and the attempt bound below for a Terminal row.
   The tool validates the result with `submission()` from
   `tools/board-test-queue.py`, loaded by path.

At claim time, each Terminal runner's `claimed()` re-hashes the bundle and
checks its prerequisite bindings; the Monitor transport re-runs
`validate_bundle()` in `ready()` before launch. The tool adds no later check.

Terminal procedures require ranks 1 to 5 and rank 9 earlier in the same power
cycle. A Terminal test passes when its jobs complete and each output matches
its host reference; for the NPU row that is one admitted model.

**Completion counts.** Section 1.3 of
[docs/performance-architecture.md](../performance-architecture.md) permits the
one-tick fallback waits in `pvr_gpu_service.c` and `npu_service.c` only until
board evidence shows interrupt delivery. That evidence is three counts per
job: completions observed by interrupt, completions observed by the fallback,
and client wake-ups. Zero fallback completions over a session would justify
removing the tick wait. Nothing records these counts today.
`pvr_gpu_service.c` counts no completions. `npu_service.c` counts
`irq_completions` in `irq_progress()`, which also runs for transitions that
the fallback observes, so that counter does not separate the two paths. No
runner or host result reports any of the three. The tool therefore refuses a
Terminal row unless its runner declares the module-level literal
`COMPLETION_COUNTS = ('interrupt_completions', 'fallback_completions',
'client_wakeups')`, read with `ast` in the same way as `PREREQUISITES`. The
expected result and procedure record the counts as measurements, not as pass
conditions.

**Requests to the GPU service, NPU service and OS and accelerator image
owners.**

- In `pvr_gpu_service.c` and `npu_service.c`, count separately the
  completions that the interrupt path observes, the completions that the
  one-tick fallback observes, and the client wake-ups, per job.
- Carry the three counts to the host in each job's result, and have
  `tools/check-a733-os-supervised.py` and
  `tools/check-a733-accelerators-supervised.py` write them to the session
  record under the three names above and declare `COMPLETION_COUNTS`.
- Name the admission values in the value `prepare()` returns that identify
  the NPU service, GPU service, dispatcher and arbiter builds, so that each
  row's `admission` can name them.

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
the same view. The sandbox also changes into the run directory with `--chdir`;
`bwrap` then always sets `PWD`, so the step removes it before it loads any
module. Inputs must lie outside `/tmp`, `/run` and `/sys`, which the sandbox
replaces with empty directories. If `bwrap` is missing or fails to start, the
tool stops.

## Interrupts, memory and synchronization

The tool polls nothing; the outer process blocks in one wait on the sandbox.
Bundles are small (the eight-core launch bundle is 548 KiB), and hashing
streams 1 MiB blocks. On the board, interrupts, placement and cache policy
belong to each path; for memory runs they are those of the memory
qualification design. Terminal rows return evidence over the shared USB0
loop, which polls once per tick because USB0 interrupt delivery is not
qualified, a wait that section 1.3 permits.

Memory runs use one Cortex-A55 at the 494 MHz boot clock, as memory
qualification's Left out section requires. At that clock the march is limited
by the CPU, at 1.0 to 1.6 GB/s. Two changes would shorten the chain, both
*estimated*. A qualified A55 clock raise to 1.8 GHz would cut march time by
about three times and give 3 to 4 runs; it needs the user's approval under
memory qualification's clock rule and the CPU clock evidence, and it carries
no secondary-core startup risk. A returning multicore base with eight
per-core slices, if DRAM sustains their sum, would give 3 runs, but it waits
for the primary agent to qualify secondary-core startup. Both are left out
until that approval or qualification exists. On the host, exclusive creation
of the run directory keeps two runs apart, and the snapshot refuses a source
or revision that changes during its copy.

## Budgets and measurement

`checks/board-bundles.py` runs the tool on fixture rows, including a nested
OS-image layout, a stripped memory ELF and one linked with `--discard-locals`.
It names the tool, whose name matches the device-access pattern in
`checks/check-index.py`, so the index lists it as device-capable and runs it
only when named. No fixture names a USB path or lock file, so a broken
sandbox fails the check instead of reaching a device.

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| Device nodes and entries in `/sys`, `/tmp` and `/run` visible to preparation and its children | Full host view | The 14 `/dev` entries above; the other three empty; only the four set environment variables | A fixture lists them from the tool and from a child process |
| Sandbox start time | 31 ms for `bwrap` running `sh`, on this host on 2026-10-22 | 100 ms, one sandbox per run | Timed in the check |
| Bytes the tool reads from `bundle/` outside `prepare()` | Not applicable | The bundle size, one pass | `rchar` in `/proc/self/io` around the artifact pass |
| Writable entries in `bundle/` during the artifact pass | Not applicable | Zero | `stat` modes |
| Copies of each bundle file | One, written by `prepare()` | One; the snapshot holds sources and `artifacts.json` | Snapshot manifest |
| Submissions refused by `submission()` | Not applicable | Zero | The tool's own calls |
| Monitor images accepted with no symbol table, no `monitor_entry`, a USB or CPU-start prefix, or (memory) no `monitor_renewal_cap` | Not applicable | Zero | `nm` scan; fixtures inject each case |
| DDR tested | 8 KiB, rank 3 | 12,224 of 12,288 MiB | Memory qualification's `validate_bundle()`; on the board, the union of recorded ranges |
| Memory runs per chain | Not applicable | 4 to 6 *estimated* | Memory qualification's planner with the step limits; board records replace the estimate |
| Time from entry to forced return | 2 s, uncapped by renewal in USB builds | 22 s | Primary agent's monitor model; board record ticks |
| Fixed cost per run: upload, launch, re-enumeration, record read | 2.0 s *estimated*: the 2026-10-15 forced-stall run took 4.0 s from its first read to its result file, of which 2 s was the deadline | 3 s | Evidence file times; board journals replace the estimate |
| USB re-enumerations per chain | Not applicable | 8 to 12 (two per run) | Count of `guard.reenumeration` reservations |
| Chain wall time | Not applicable | 10 min: 2.3 to 3.5 min for 4 to 6 runs alone; 8.3 to 9.5 min after ranks 1 to 7 (*estimated*) | Replay through `wait_needed` in `~/git/mimus/tools/usb_guard.py` from the guard state ranks 1 to 7 leave |
| Queue timeout | Not applicable | Monitor 1,200 s; Terminal the attempt bound below, at most 3,600 s | Computed by the tool from the runner, the transport and the bundle's file sizes; the check recomputes the bound with the transport's own `transfer_timeout()` and `REGIONS` and confirms the timeout is at least that bound |

The wall-time replay assumes 22 s per run plus 3 s of fixed cost, and at
least 60 s from rank 7's last re-enumeration to the first memory run. It
counts two re-enumerations each for ranks 4 to 7 and none for ranks 1 to 3,
whose code returns to the ROM without resetting its USB stack. Those eight leave
room for two memory runs under the guard's cap of 12 per 600 s, so the third
waits for the oldest to age out.

The Monitor timeout covers the runner's worst case in one attempt: 615 s for
the churn guard to admit two re-enumerations (the 600 s window plus 15 s
spacing), 435 s of transaction and return bounds (120 s each for the upload,
contract write and record read, 15 s each for three short commands, and the
30 s return window), and 150 s of margin. A stop during the guard wait comes
before launch and leaves the board in FEL.

The Terminal timeout covers the worst case of one supervised attempt, in the
order `run_session` in `tools/check-a733-smp-supervised.py` performs it. Each
FEL request is bounded by `transfer_timeout()` in `tools/check-a733-smp.py`:
15 s plus one second per 250,000 bytes moved, rounded up. The tool reads
those constants and the SRAM backup regions from that file without running
it. The terms are:

- 615 s for the churn guard to admit one re-enumeration;
- 45 s for the version request, the watchdog read before launch and the
  `exe` request;
- 80 s for the five SRAM backup reads;
- for each bundle file, a 15 s watchdog read plus one write and readback of
  the file; every upload is the bytes of one bundle file, so counting every
  file covers every upload;
- the runner's capture window, 19 s or 59 s;
- 111 s for the refusal report read, a watchdog read and the five SRAM
  restores that follow a capture window with no native device;
- 60 s of margin.

The tool refuses a row whose bound exceeds the queue's 3,600 s limit. Without
uploads the bound is 970 s for the OS-image rows and 930 s for the arbiter
row. An existing OS-image bundle of 42 files and 22.9 MB gives 2,451 s, of
which 1,481 s are uploads. No figure above was measured on the board.

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
