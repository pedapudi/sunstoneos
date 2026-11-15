# Select checks by changed behavior

Driver and fixture authors follow [completion and timing validation](driver-validation.md):
wait for documented completion before verification, preserve vendor barriers and
inter-write delays, and model delayed completion with stale readback.

Board runs are recorded as [evidence records](evidence.md), not prose.

Before a test batch, state the changed behavior, credible failure risk, and
smallest check that resolves that risk. Run focused checks during iteration.
Broaden testing when a shared dependency changes, a failure reveals wider impact,
or a milestone lacks integration evidence.

## Select the integration gate explicitly

[tools/integration-gate.py](../tools/integration-gate.py) runs the selected
central build targets, OS variants and regression checks. The coordinator names
the changed behavior and its risk with `--reason`. A call without a selection
is refused before any build or check runs. For example, a PMIC source-ownership
change selects its two source checks:

```sh
python3 tools/integration-gate.py "$PWD" pmic-ownership \
  --reason 'PMIC adapter ownership changed; check the provider boundaries' \
  --check pmic-tree --check power-domains-tree
```

Add `--target TARGET` for each affected OS-core central build, `--variant NAME`
for an affected OS configuration, and `--check NAME` for each needed regression
check. The kernel and eight-core soak checks automatically build their required
images. Selected external application checks build their declared targets.
OS-core builds and every OS variant clear external application selectors.
Each application checkout supplies its name, target and command list in
`sunstoneos/gate.json`; its builds and checks use a separate directory.
Select checkouts with repeatable `--application CHECKOUT` arguments or the
semicolon-separated `SUNSTONE_EXTERNAL_APPLICATIONS` environment variable.
A source declaration with `application_directory` may also provide a
compatibility selector, such as `MIMUS_DIR` for the mimus example.

A source-only check configures no build. Duplicate selections run once.
`--help` lists the supported OS variants; external check names come from the
selected declarations. Other component checks run directly with their
required inputs.

A path that a change deletes or renames also selects `checks/docs-links.py`, which runs in under a second, because a document may link to it.

Use the affected-check selector for a worker's Git range:

```sh
~/.cache/sunstoneos/venv/bin/python3 -B checks/affected.py bringup..HEAD
~/.cache/sunstoneos/venv/bin/python3 -B tools/integration-gate.py "$PWD" affected \
  --affected bringup..HEAD --build-root "$HOME/.cache/sunstoneos/build/my-task" \
  --reason 'Run the checks whose inputs changed and one OS baseline'
```

The selector compares both range endpoints and the checkout's generated check
index. It includes removed dependencies and both sides of renames. Every plan
includes one supervised OS baseline with external applications cleared.
`--changed PATH...` selects paths directly for uncommitted iteration.
The gate runs selected pure-host checks once. It records checks requiring images,
cached evidence, USB or other arguments as PENDING and exits 2 when these remain.
Supply those inputs to the component checks before claiming complete coverage.

Selection uses declared and inferred repository inputs. Textual inference misses
paths assembled dynamically; an empty selection cannot establish complete
coverage. Add a literal `CHECK_INPUTS = ['drivers/example', 'include/example.h']`
to a check when inference misses its inputs. Review the changed behavior and
shared dependencies before accepting the resulting plan.

`--full` explicitly requests all central targets, all configured OS variants of
[the OS image](os-image.md), including the supervised NPU
profile (variant `npu`), the two-model EL0 submission ring (variant `npu-queue`),
all gate checks and every indexed pure-host check once.
The hook fixture remains conditional. Use `--full --affected BASE..HEAD` or
`--full --changed PATH...` to name its input scope; ordinary full checks still
run. Without an explicit scope, the gate uses the latest first-parent commit
change plus staged, unstaged and nonignored untracked paths. A root commit uses
all its tracked paths. `hook-scope.json` records the inputs and decision.
The gate lists checks needing external inputs without claiming they ran. The `npu`
variant builds with the
catalogue map `~/.cache/sunstoneos/build/npu-board/session-artifacts.json`;
the OS build verifies every file it names against
`tools/npu-fixtures.json`, the gate refuses the variant before any
build when the map is missing, and the summary records its SHA-256.
The optional GPU linear, mSBC, Cortex-A76 and CPU clock options are not
variants of the gate; build them directly with `cfg.sh` when a change
affects them. Run the full set once for integration acceptance. A worker builds and runs the checks it changed while it
works and does not run the affected gate after each commit; the gate runs at landing ([landing](landing.md#the-gate-runs-once)).
A worker may run `--affected bringup..HEAD` once on a clean tree before it asks for a landing, and the landing reuses that pass. Run all three baseline images, the integration
gate and the check index once before the final integration report; reuse their
results while relevant inputs remain unchanged.

Each invocation uses a new label under `~/.cache/sunstoneos/build/gate-LABEL`.
It records the selection, reason, revision, commands and logs. The builds use one directory per source checkout,
`gate-central-NAME-HASH`, `gate-APPLICATION-NAME-HASH` and
`gate-os-VARIANT-NAME-HASH`, where NAME is the
checkout's directory name and HASH is the first ten hexadecimal digits of the
SHA-256 of its resolved path. A gate on another worktree therefore never meets
a CMake cache configured for a different source. A lock file,
`gate-NAME-HASH.lock`, refuses a second gate on the same checkout while one
runs. The gate refuses a source with uncommitted or untracked changes unless
`--allow-dirty` is given. It records `git status --porcelain` in `status.txt`,
and in the summary the SHA-256 of `git diff --binary HEAD` and a SHA-256 over
the name and content digest of every untracked file that Git does not ignore,
so evidence from a dirty tree names the changes it built. The revision in the
summary comes from the same reading. The gate reads the revision, status,
diff and untracked files again after taking the lock and after its last step,
and writes a FAIL line and fails if any of them changed or could not be read,
for example an unreadable untracked file. There are no automatic retries, including for emulator timeouts.
Diagnose the failures and run only the affected work with a new label; reuse the
successful evidence from the earlier invocation. The soak defaults to one
trial; `--trials N` declares a repeatability sample in advance and needs a
reason for that sample count. It does not retry failures.

### Checks that need a built image

A check that executes an integrated OS image needs the image. The gate does not leave such a check pending: for each affected
check that is not pure-host, the check's own file says what the gate does, in a literal near the top that
[checks/check-index.py](../checks/check-index.py) reads.

- **Supplied.** `GATE_RUNS = [[ARGUMENT, ...], ...]` lists one argument list for each run. `{output}` is the run's new evidence
  directory and `{cache}/...` a path under the cache. The check takes the images it names. `{elf:NAME}` and `{build:NAME}` name the
  ELF and the build directory of a gate variant (`sup`, `res`, `headset` and the others of `VARIANTS`) or of a
  recipe in [tools/gate_images.py](../tools/gate_images.py) (the CPU-return NPU image, the CPU-return GPU image, the latency image). The gate builds each image once
  per tree state, keyed by the revision, the tracked diff and the untracked files, and passes it to the check.
  `{fixture:NAME}` names a fixture of `FIXTURES` in the same file: host data that a cache cleanup may delete. The gate runs the
  fixture's build command, which skips whatever is current, before the checks that name it. The `npu-graphs` fixture is the compiled
  test graphs under `build/npu-graph-variants` ([tools/npu-graph-build.py](../tools/npu-graph-build.py)).
- **Board.** A check that needs a board or USB access is reported with a `BOARD` line and does not refuse. One that needs a
  board without a USB or `--fel` argument declares `GATE_BOARD = 'reason'`.
- **Waived.** A check whose inputs the gate cannot supply declares `GATE_WAIVED = 'category | reason'`. A module that only defines helpers (no command line) is a library and declares nothing; a tool that is not a check belongs in `tools/`. The gate counts waived checks and writes their reasons to
  `waived.txt` in its output. A waiver states what input is missing and what would supply it.
- **Unsupplied.** An affected check in none of these groups makes the gate refuse before building anything (exit 2), naming the
  check, in `--full` and in `--affected` or `--changed` alike.

`checks/gate-inputs.py` checks the declarations: no pure-host check declares one, no check declares two, every non-pure-host check
has an answer, and each image-executing check builds its emulated machine with `harness.machine()`, the one machine with the maximal core
that the images' atomics need. Declare the answer in the same file that adds an image-needing check.

### The gate yields the disk to board runs

The gate and the landing run at nice +10 and the idle I/O class, and image-copying checks copy sparsely
([board runner](board-runner.md#board-runs-keep-their-timing-while-the-gate-runs)). A check that copies a disk image calls
`harness.copy_image`, not `shutil.copyfile`; `checks/host-priority.py` checks the priorities and the sparse copy.

### A passing step keeps its records and drops its build products

At its end, after it records its peak disk use, a gate reduces the evidence directory of every step that passed (PASS or WARN) to its
records. Step `image-NAME` writes `evidence/images/NAME`, `indexed-NAME` writes `evidence/indexed/NAME` and `check-NAME` writes
`evidence/NAME`. A record is a text file of a record suffix (`.json`, `.jsonl`, `.log`, `.txt`, `.md`, `.csv` and a few others, or no
suffix) up to 8 MiB that is no ELF file or archive. The step keeps the records at the top of its directory as plain files and
compresses any of them over 1 MiB in place (`NAME.gz`); it packs the records of its subdirectories into one `records.tar.gz`. Everything
else goes: object files, linked executables and images, generated sources, firmware and tensor copies, CMake build trees, fixture Git
repositories and the fixture `build`, `build-*` and `inputs` directories. `retention.json` in the step directory records the count and
bytes removed and the size and SHA-256 of every linked ELF removed. The gate also writes `images.json`: the path, size and SHA-256 of each
image it built, because the per-checkout build directories that hold the images are rebuilt by the next gate on that checkout.

A step that failed or was skipped keeps everything, and so does an evidence directory that no step names, so a failure stays
diagnosable. That includes the build products it ran on: the build directories are named after the checkout, not the gate, and every
landing gate runs in `~/.cache/sunstoneos/land-integration`, so the next landing rebuilds into the same `gate-img-*` directories (on
2026-11-14 a failing landing step's image was overwritten by the next landing before anyone re-ran it). At its end, while it still holds
its checkout, a gate copies every build directory that a failing step's command names into `evidence/kept-builds/` (for a bundle, only the
`out-` directory of its tree state), and `kept-builds/kept.json` records each failing step's command as run and pointed at the copies.
Over the 130 gates of the week to 2026-11-14, 27 had a failing step, and the directories those steps named came to 39 MiB per failed
gate at the median, 286 MiB at most and 1.7 GB in all. When a later gate of the same landing label passes, its slimming of the earlier
gates removes the build products of the copies too. The gate-root files stay as they are: `summary.txt`, `commands.txt`, `timing.txt`, `environment.txt`, `gate-record.json`,
`disk-peak.json` and the step logs. These are the only files that anything reads from a gate directory after the gate: `tools/land.py`
reads `summary.txt` and `gate-record.json`, `tools/host_space.py` reads `disk-peak.json`, the evidence records under `docs/evidence/` cite
`summary.txt` and `commands.txt`, and the board runner copies each image into its own queue entry. A passing gate also slims every step
of the earlier gates of its landing label, which were kept for diagnosis. The deletion is `slim_gate` in `tools/clean-generated.py` and
obeys its refusals: no evidence records, sources or build directory that an evidence record or queue entry names. On a landing gate of
88 steps this keeps 7 MB of 340 MB and takes under one second. The per-checkout build directories (`gate-os-*`, `gate-img-*`,
`gate-central-*`) are the incremental build caches and stay; `clean-generated.py stale-builds DAYS` prunes them.
`clean-generated.py` never deletes a build directory that a check or tool names as `build/NAME` in its source (for example
`npu-graph-variants`); the set is derived from the sources. `clean-generated.py slim-gates [--apply]` applies the same rule to the gate
directories already on disk, reading each step's verdict from `summary.txt`.

### The gate keeps its temporary files off /tmp

/tmp is a memory-backed volume with a per-user inode and size quota; scratch there stops builds with "Disk quota exceeded".
[host_space.py](../tools/host_space.py) sets `TMPDIR`, `TMP` and `TEMP` to `tmp` under the build root (the landing: under the cache's
`build` directory; the board runner: under the cache) at the start of `main()` in `tools/integration-gate.py`, `tools/land.py` and
`tools/board.py`, so the compilers, CMake, the checks and the helpers all write on the main disk. A base under `/tmp`, `/var/tmp` or
`/dev/shm` is refused. The gate and the landing also refuse to start, naming the volume, when the build root has less than twice the largest
disk use (bytes and inodes) that one of the last ten gates recorded in its `disk-peak.json`, or less than 5 GiB
and 100,000 inodes when none is recorded, or `/tmp` has less than 2 GiB or 20,000 inodes free. Fixtures that run the gate or the landing against a scratch
sandbox set `SUNSTONE_HOST_SPACE=off`; nothing else does. `checks/host-space.py` runs the gate's environment setup and checks all of this.

### Failure handling, parallelism and prose checks

`--full` keeps going. It runs every selected step, writes one `PASS`, `WARN`,
`FAIL` or `SKIP` line per step to `summary.txt`, and exits with status 1 when any
step failed or was skipped. A step is skipped, and counts as a failure, when a
build it needs did not pass: the kernel and soak checks need `central-build`,
and an application check needs its application build. A failed configure step
skips only its own build. The last lines of the summary count the steps and
name every failure; the line `PASS selected integration checks` appears only
when none failed. `--fail-fast` runs the steps one at a time in their fixed
order, stops at the first failure and writes `Stopped at first failure`. Every gate keeps going unless `--fail-fast` is given,
targeted selections included: the same selection takes about a third of the time in parallel, and a gate holds a slot while it
runs (below).

Builds finish before any check starts. A keep-going gate builds its images three at a time, each with the slot's build cores
divided by three and rounded up (`-j 3` in a slot of `-j 7`), and starts another build only while `MemAvailable` stays above
`--min-available-gb` plus the measured peak of one build; `--fail-fast` builds them one at a time in a fixed order. The count and the
peak are `image_builds` in [tools/gate-slots.json](../tools/gate-slots.json), measured on the 21 image builds of an affected gate:
one at a time with `-j 7` they took 168 s, and three at a time with `-j 3` took 72 s; a rebuild after one kernel source changed took
135 s and 48 s. Three builds peak at 1.0 GB in all, inside the measured peak of a gate, so the slot count does not change. A keep-going gate
then runs the checks in parallel, longest first: the order follows the host seconds that the latest 20 earlier gates recorded in their `timing.txt`, the most recent record of each step winning, and a step with no record starts first. The order changes no check's inputs or verdict; it lets the longest check start when the builds end instead of after the shorter ones. `--jobs N` bounds the workers and sets the `-j` of the builds (default: the gate's share of the
usable cores, below; 8 and `-j 12` for `--full`). The
gate starts no new check while `MemAvailable` is below `--min-available-gb`
(default 32); a check that cannot start within `--memory-wait` seconds
(default 3600) fails as not started. `--check-timeout` bounds each check
(default 3600 seconds). A check that runs QEMU or times a result runs alone,
after the parallel group finishes: the kernel and soak checks, every external
application check, the indexed checks named in `SERIAL_CHECKS` in
[checks/check-index.py](../checks/check-index.py) and the wall-time checks of the timing lock (below). The index reports the
choice as the `serial` field of each entry.

### Scheduler QEMU models: wake latency is a critical path

The yield models (`scheduler-yield-qemu` and `scheduler-yield-storm-qemu`, run by `checks/scheduler-control-linked.py`) used to assert that
no ordinary wake took more than half of the 400 microsecond wake period, measured as the counter ticks from the wake interrupt to the
served task. The runs use one TCG thread and `-icount shift=0,sleep=off`, where the eight vCPUs take turns and guest time is the sum of
the instructions that all of them execute. A core that spins while it waits, here the priority-1 busy task of the victim core, therefore
adds its instructions to the elapsed time of the wake, which on hardware, where the cores run in parallel, it does not. How many of
them it adds depends on the order in which the vCPUs happen to interleave, and under host load that order differed in about one run
in twenty: the worst wake was 11,207 ticks (90 percent of the 12,500-tick bound) in 23 of 24 runs and 16,016 ticks in the other. QEMU 10.2.1's own host-timed paths are not the cause: the round-robin kick timer runs on the virtual clock, and no RCU
force-kick occurred in 72 loaded runs. The cause of the different interleavings is not identified; QEMU pinned to one or two otherwise idle
cores differed in 22 to 25 percent of runs, so it is not starvation of the QEMU thread.

The bound is now on the critical path of each wake, in instructions: the instructions of the owner (core 0) plus the instructions of the
victim core itself, both outside the spin function, between the wake interrupt and the served task. `tools/qemu-critical-path.c` is a TCG
plugin that counts the instructions each vCPU executes (the quantity the PMU instruction counter of each core reports under icount) and
snapshots the counts at the empty marker functions `ymark_isr_N` and `ymark_srv_N` that `checks/scheduler-yield-qemu.c` calls on core N; the
check subtracts the two snapshots. The bound stays 200 microseconds: at the model's one nanosecond per instruction (1,000 instructions per
microsecond, 16 per tick of the 62.5 MHz counter) it is 200,000 instructions, the value of the old 12,500 ticks. The models no longer
assert the tick latency; they still count raised and served wakes, the pending switch requests and the fast-path exits.

Over 132 runs under 24 and 48 busy host processes the worst critical path was 41,924 instructions in every run of the yield model (21
percent of the bound) and 17,022 in every run of the storm model (8.5 percent); it did not vary with the interleaving. An earlier version of
the plugin counted 36,855 to 84,814 because it took the executing core from the callback's `vcpu` argument, which in QEMU 10.2.1
(`accel/tcg/plugin-gen.c`, `gen_cpu_index`) is the index of the vCPU that translated the block, baked in as a constant when the run is not
`CF_PARALLEL`, as `-accel tcg,thread=single` is; translated blocks are shared by all vCPUs, so vCPU 0 was credited with the instructions
of every core (118 million of the blocks of one run), and the inline per-vCPU counters share the defect. The plugin now reads TPIDR_EL1,
which `platform/qemu-smp/start.S` sets to the core index before any C, in every block (MPIDR_EL1 reads 0 through the plugin interface), and
the check requires every marker to have run on the core that its name gives (4,242 of 4,242 do). The case `yield-wake-path-regression`
runs the model with 6,000 extra loop turns of work on the wake path (about 10 instructions each) and requires the critical path to cross the
bound (217,260 measured, 8.6 percent above it, the same in every run). The bound is not the only detector there, and cannot be: the
models' own end-of-run accounting fails above about 3,000 turns, where the critical path is 65,000 instructions, and a run that fails
ends early, so the path of the larger delays does not rise monotonically (8,000 turns: 119,619).

### QEMU guest time in the radio checks

A QEMU guest that runs without `-icount` counts a tick, a timeout and a clock reading in the host's time, so a verdict that reads them changes
with the speed and the load of the host. The four radio checks that start a guest (`a733-radio-session`, `aic8800-dispatch`, `radio-network`
and `radio-network-dhcp-dns`, 29 guest runs in all) did not use it. With 24 copies of each run at once beside 24 busy host processes
(48 runs of each of the 29, 1,392 in all), 83 runs failed: 76 of 336 in `a733-radio-session` (six of its seven cases), 3 of 240 in
`aic8800-dispatch` (the deadline case), 1 of 240 in `radio-network-dhcp-dns` (the DHCP case) and 3 of 48 in the `late` case of
`radio-network`. The console output of the DHCP, static-address and SNTP cases differed in every run, because it prints times.

They now pass `harness.QEMU_DETERMINISTIC` (`-accel tcg,thread=single -icount shift=0,sleep=off`), the flags of the yield models and the
latency-control models, so that guest time follows the instructions executed. Under the same load all 1,392 runs pass and each case writes
one console output, byte for byte, in all 48 runs; alone the verdict and the output are the same as before. The slowest run beside the
busy processes was 3.8 s (`a733-radio-session`), 1.4 s (`aic8800-dispatch`), 1.8 s (`radio-network`) and 7.0 s (the DHCP case); the host
kill limit of each check is 4 times the slowest run, recorded per check in `qemu_checks` of [tools/gate-slots.json](../tools/gate-slots.json)
(re-measured at a higher load in "A run the host stops"), and only ends a run that hangs: a run it kills is run once more. `checks/qemu-icount-audit.py` fails a check that starts a guest without `-icount` unless it is listed with the
reason its verdict does not read guest time: `checks/smp-ticket-lock.py` and `tools/check-smp.py` run multi-threaded TCG on purpose, to
exercise the ticket lock's atomics on cores that run in parallel, and their guests assert no time. The checks `os-npu-graphs`,
`os-npu-service` and `os-npu-fault` run on `checks/host/host_rtos.c`, whose clock is virtual and whose tasks switch only at their own
calls, and `session-lifetime` and `a733-radio-platform` start no guest; none reads host time.

### The cause ping

The latency cause model (`latency-partitioned-pmu-model`) ends its QEMU run with `FAIL src/apps/latency/model.c:0x34` since the
tick-footprint step moved to one TCG thread with a fixed icount: the cacheline ping of `apps/latency/cause-ping.c` between CPU0 and CPU1
timed out after 27 of 256 exchanges (status 4 of the pair 0 row; the local reference completed 256, and the CPU0-CPU6 pair was aborted
before it began). The step passed anyway, because it judges only the busy-tick trace.

It is an artefact of the round-robin icount, not a defect of the ping or the scheduler. The ping is a handshake between two cores: each
exchange waits for the peer's turn, and on one TCG thread a turn comes after the other seven vCPUs have had theirs, so an exchange costs
about 9 ms of guest time against the ping's 250 ms deadline (a quarter of the application's one-second progress window; a longer deadline
runs into the controller's own progress check, `workload.c:0x117`). Run on QEMU's multi-threaded TCG, where the cores run in parallel as on
the board, the same image completes every exchange: 64 of 64 runs passed, 10 alone and 54 beside 24 busy processes with up to eight
copies at once, the slowest in 5.8 s. The images at `caa8d7c67` (before the step moved), `f7eb88ad1` (the move) and `97d2317c3` (the ping's
kit task creation) all pass multi-threaded and fail on one thread, so no landing since 09:30 (tcb-lines, fastpath-cycles, outbox)
changed it. The failing single-thread run is also not repeatable: alone it ended at `model.c:0x34`, and six copies beside busy processes
ended at `workload.c:0x117`.

Nothing required the model's `PASS latency cause model` line: `checks/tick-footprint-budget.py` ran the image to trace it and its comment
left the model's functional assertions to "its own checks", of which there were none. Two changes:

- `checks/tick-footprint-budget.py` stops the guest (SIGTERM, which lets the plugin write the trace) once the plugin holds the last of its
  240 windows, so it runs what the step needs and shows no verdict it ignores.
- `checks/latency-cause-model.py` runs the image on the multi-threaded guest and requires the PASS line, then rebuilds the image with the
  peer reporting a sequence error after the fourth exchange (the changed copy of `cause-ping.c` compiled with the image's own flags and
  linked with its own link command) and requires the model to fail at its ping assertion. It runs on real threads, so it holds the timing
  lock (`timing_checks` in `tools/gate-slots.json`), and its kill limit is 4 times the slowest loaded run, 9.3 s (see "A run the host stops").

The model failed on 15 to 25 of 96 runs beside eight busy processes on the multi-threaded guest, in four ways, all from the host's
scheduling of the vCPUs and none from the ping or the scheduler. The check now passes 96 of 96 runs under the same load, and passed four
whole runs of itself, pass and mutant halves, beside eight busy processes.

- `model.c` asserted that every busy wake lies inside its phase window. The tick hook released a wake for a tick that came due before the
  phase began whenever the interrupt reached CPU0 late (a host stall of up to 78 ms was measured); the recorder places a sample in the phase
  that holds its whole interval, so such a wake counted toward the phase without being in its window. The hook now releases only ticks that
  came due inside the phase. A wake that runs after the end of its phase still fails (a mutant that adds 2^40 counts to the running stamp
  fails the model).
- The wake-segment model asserted that all 128 busy wakes of a core link to an interrupt chain. A chain a later interrupt replaces before the
  woken task finishes its bookkeeping is counted unlinked, by design (`ss_wake_seg_running`), and a stalled core does that to about one wake
  in a hundred. It now requires every wake to be linked or unlinked and more than half to be linked, so a recorder that links none fails.
- It asserted that a core CPU0 releases arms no timer. CPU1 and CPU6, the cores of the cause ping, arm timers for the ping's waits while the
  recorder is still armed, three to seven times on a loaded host; the other cores must still arm none.
- It asserted two complete busy ticks. A tick is complete only when no core still holds its previous wake, and a loaded host left one in
  two of 96 runs. It now requires one, and a second exactly when more than one was filed.

The broken-ping half expected `model.c:0x34` to be the first FAIL line. On a host loaded enough that the guest's own run budget (12 s of guest time)
ran out first, the run ended at `workload.c`'s budget failure and the check reported that the broken ping did not fail the model. The peer
that meets the planted sequence error now reports it when it happens (`ss_latency_cause_ping_failed`, a weak no-op outside the model), and
the check accepts a non-zero exit without the PASS line and either that report or the ping assertion. A run that ends at the budget failure
without either is repeated, up to three times, in both halves.

### One tree, one build id

An image's build id hashes its inputs in list order, and the lists come from globs, which sort by absolute path. Two checkouts of one tree
at different depths or names put the same headers in different places beside the files of other roots, so one tree gave different build ids
at different checkout paths (found in the stage-1 decoder image check), and `build-id-inputs.txt` showed a block of
`kernel/scheduler/*.h` moved. The identities now sort their inputs by portable path (`src/...`, `build/...`, `cache/...`:
`sunstone_sort_inputs` in `cmake/aarch64-clang.cmake`, applied before every digest loop and inside `sunstone_identity_digests`), and the
gpu-application and cached-devices identities pass the text of their compile options, definitions and link options through
`sunstone_portable_text`, which the other identities already did. `checks/build-id-checkout-path.py` copies the tree to two paths of
different depth and name, configures both, and requires the same eleven `build-id.txt` files with the same ids; on bringup before the change
eight of the eleven differed. A copy with one source byte changed must change the OS image id.

The board record's `git_revision` is the revision of the source the image was built from, read from the image. Every image carries the
48-byte object `a733_source_revision`: `SRV1`, the 40 hex digits of HEAD, a flag that the revision was known and a flag for uncommitted
changes to tracked files. `cmake/source-revision-write.cmake` writes it at every build (a custom target, not at configure, so a commit between
configure and build cannot leave a stale one), `cmake/sunstone-source-revision.cmake` adds it to every executable that carries a build-id
object (deferred until the tree is configured, and listed in `source-revision-targets.txt` of the build directory), and the revision is not
part of the build id. The runner reads it with `board_images.embedded_source_revision`. A worker can commit after building or build from a
dirty tree, so the record names the host tree's HEAD at run time separately, as `host_tree_git_revision`, and
`git_revision_host_tree_mismatch` is true when it differs from the image's revision (null without a host tree or an embedded revision).
`git_revision_source` is `image`, or `host-tree` / `daemon-checkout` for an image built before images carried the object or outside a git
checkout, and `git_revision_dirty` is the image's flag. The daemon's own revision is `daemon_git_revision`. The stage-1 decoder record said
`4f4d44e73`, the daemon's HEAD, for an image built from `6fef74d18`. `checks/image-source-revision.py` builds the object as the images do,
commits to the tree afterwards and checks the record; `checks/board-runner.py` covers the fallback fields.

The image's own hash now changes with every commit, even when its sources do not: the embedded revision is part of its bytes. The build
id stays source-only. Anything that keys reuse on the image hash instead of the build id treats a rebuilt image as new, so the paths that
name an image were checked for what they mean:

- **Bytes, and right to be.** A session, an admission and a queue job bind the exact bytes that run on the board:
  `tools/board_images.py` (descriptor, DMA and session admissions: `image_sha256 == sha(image)`), `tools/session.py`,
  `tools/sunstone-image-link.py`, `tools/a733-crumbs.py` (`bundle_image`), the `tools/check-a733-*-supervised.py` admissions, the frozen and
  submitted copies of `tools/board_image_store.py` and the job and result hashes of `tools/board.py`. After a rebuild at a new commit the
  session of the old image names the old bytes, so it is prepared again; its build id, which the image checks on the board, is unchanged.
  `tools/host_admission.py` caches the host admission harness by image digest: a queued copy of an image has only its bytes to find the
  harness that was built at submission, and the cost of the change is one more compile per rebuilt image.
- **Build id, and right to be.** `tools/host_admission.py` (`find_build`) accepts an image only when its `a733_smp_build_id` equals the
  build's recorded `build-id.c`. The gate reuses a passing gate by the git tree and the inputs of the selected checks
  (`tools/land.py`, `gate-record.json`), and builds images once per tree state (`tools/gate_images.py`): neither reads an image hash.
- **Not affected.** `tools/check-smp.py` and `tools/check-kernel.py` skip a passed case by an identity that holds the image digest, for QEMU
  models and the kernel check, which have no build-id object and so carry no revision. The pinned hashes (the rail helper probes in
  `tools/board.py`, `BUNDLE2_ELF` in `checks/board-bundles-apps.py`, `PIN` in `tools/card-fel-image.py`) name frozen files that are not
  rebuilt. `checks/harness.py` `Steps` skips a passed step by its command text.
- **One wrong path, changed.** `checks/check-index.py` followed `include()` and `add_subdirectory()` only, so a change to
  `cmake/source-revision-write.cmake`, which every build runs with `cmake -P`, selected four checks and not the checks that build the images
  it writes into. It now follows `cmake -P` scripts (`CMAKE_SCRIPT`); `checks/image-source-revision.py` requires the selection.

### A run the host stops

A guest run or a threaded host model can end without saying anything about the claim of the check that started it, because of the host and
not the code: three cases in the landing gates of 2026-11-14, all under a load average of 32 to 35 on 24 cores.

- **Boot.** The guests of `platform/qemu-smp` and `platform/qemu-os` give each boot wait (the secondary cores coming online, the primary
  core's initialisation flag, a register clearing) one second of their own counter. On a loaded host the secondaries take longer, and the
  guest ended with `FAIL src/platform/qemu-smp/platform.c:0x93` before its application started; the broken-ping half of the latency cause
  model reported that as "the broken ping did not fail the model". The waits now print `FAIL boot-timeout <wait>` (`register-clear`,
  `secondary-cpus-online`, `primary-init-flag`) when their second has passed; the bound is the same.
- **Kill.** `radio-network` was killed by its wall-clock bound after 7.2 s, and a case of `scheduler-control-stop` by its 20 s `timeout`
  (status 124). With a fixed icount the guest's time follows the instructions it executes, so the bound only guards against a hang; it is
  not a result.
- **Hang guard of a threaded model.** `scheduler-owner` and `scheduler-control-stop` wait for other threads with wall-clock deadlines
  (ten seconds, two seconds). A model run beside a loaded host can have a thread unscheduled for that long.

`checks/harness.py` runs the guest or binary through `run_qemu` (or any launch through `until_booted`), which runs it again after a boot-stage
failure (three attempts in all), after a kill, and after the model's own hang guard (a model stops on it with status 75,
`INCONCLUSIVE_EXIT`). A kill or a hang guard that happens twice raises `KillInconclusive`, and a boot failure in every attempt raises
`BootInconclusive`; both are `Inconclusive`, an `AssertionError`, and a check that does not catch it ends with status 75 and the line
`INCONCLUSIVE: ...`. `tools/integration-gate.py` reports status 75 as `INCONCLUSIVE <step>`, counts it apart from the failed steps and
ends with `INCONCLUSIVE selected integration checks: ...; run them again`, which is not a pass. A guest that hangs is killed twice and is
inconclusive, never a pass, and a failure of the application or the model is returned at the first attempt. Every check and tool that
runs a QEMU guest with more than one core uses it, and so does every user of a measured kill bound; `checks/qemu-boot-inconclusive.py`
lists them, feeds the harness a boot-stage log, a kill, a hang guard and a model failure, and fails when a launch site without the harness
appears. `checks/device-memory-capacity.py` is outside it: its images boot from the ROM, which has no boot wait.

The kill bound is `kill_multiplier` (4) times the slowest run of any case of the check, measured by `tools/measure-kill-limits.py` with
every command of the check's result file run 24 at once beside 36 busy processes (a load average of 67 to 121, twice the gate's), and
recorded with the load reached and the exit statuses in `qemu_checks` and `host_checks` of `tools/gate-slots.json`. The figures changed
from 3.8, 1.4, 1.8, 7.0 and 5.8 s (24 busy processes) to 7.3 (`a733-radio-session`), 5.3 (`aic8800-dispatch`), 5.5 (`radio-network`), 8.4
(the DHCP case) and 9.3 s (`latency-cause-model`, 8 copies); the host checks measured 22.0 s (`scheduler-owner`, an ASan binary) and 27.9 s
(`scheduler-control-stop`).

A limit that is part of the claim is not a hang guard, and the threaded models state it in their own progress. `scheduler-owner` no longer
fails a slot that is not inspected in ten seconds. A drain pass that begins after a hint's publication and finds nothing to inspect has
inspected every hint published before it began, so a slot still not inspected after two idle passes counted by the owner thread (the pass
running at the read may have begun before the publication) is a lost hint; no pass count would do, because a strict priority order lets
other sources' hints overtake a waiting one for as long as they keep arriving (a bound of two sweeps of passes failed the unmodified model).
The ten seconds remain as a guard that stops the run inconclusive when the owner thread ran no pass at all, and the check builds the model
with the owner losing one slot's hint in one round and requires it to fail with the idle-pass message in seconds. The same rule applies to
the shared-owner composition model of the concurrent-accelerator branch, whose deadline "the GPU retirement did not start during the NPU
job" read the GPU thread's active flag: on a loaded host the GPU thread ran the whole retirement between two looks of the NPU thread, the
flag was clear again, and the wait ran to its model-time limit (2,009,498 µs against 2,009,497). The retirement has started once it is
active or done. The latency cause model's ping timed out in 6 of 24 runs at a load average of 84 with eight copies at once beside 36 busy
processes (host time again: 250 ms for a core the host did not schedule). The check runs as a timing step, and there the image's own
bounds hold: 193 runs made four at once under the timing lock, at load averages up to 75, all passed, with the ping at most 84.9 ms of
its 250 ms and the run at most 2.2 s of its 12 s budget. The check runs each image once; the model prints its ping statuses before its
assertion, and a planted sequence error is reported by its peer as status 5.

### Gate slots and the timing lock

The host runs N gates at once, not one. `tools/gate_slots.py` holds the slots as the files `~/.cache/sunstoneos/gate.lock.0` to
`gate.lock.N-1`; a gate takes the first free one it may use and keeps the flock until it exits. A gate that finds none waits on a
ticket in `~/.cache/sunstoneos/gate-queue/`; a waiter that is killed leaves no ticket that holds the queue. Landing gates
(`tools/land.py` sets `SUNSTONE_GATE_PRIORITY=landing`; `--priority landing` does the same) go ahead of waiting worker gates and may
take slot 0, which workers never use, so a landing does not wait behind workers. The full suite takes `gate-full.lock` and no slot.

N and the share of cores a slot gives its builds and checks come from [tools/gate-slots.json](../tools/gate-slots.json), not from
constants in the code: the host's cores and memory, the cores that other work on the host keeps busy, the memory kept free, and the
mean cores and peak resident memory of one gate, all measured with a sampler on the process tree of gates that held the lock.
`slot_count` takes the fewer of what the cores (usable cores divided by the mean cores of a gate) and the memory allow;
`jobs_per_slot` divides the usable cores across the slots. A worker gate that passes `--jobs` above that share is refused before it queues
(`integration-gate.py: error: --jobs 12 is above the share of a worker gate, ...`): the slots run side by side, so more jobs than the share
oversubscribe the host for every other gate (a load average of 64 to 70 on 24 cores on 2026-11-14, from worker gates run with `--jobs 12` and
`--jobs 6` beside a landing gate, behind the QEMU and model timeouts of that day). A landing gate and `--full` keep their own rules. Change a measurement there, not the code, when the host or the gate changes.

The steps whose result depends on wall time hold the exclusive `~/.cache/sunstoneos/gate-timing.lock` while they run and run alone in
their gate: the soak, `smp-ticket-lock.py`, `tick-footprint-budget.py`, `gpu-conformance.py`, `land.py`, `submit-wait.py` and the `SERIAL_CHECKS`. The two CPU/NPU
checks (`cpu-npu-concurrency.py`, `cpu-npu-return-chain.py`) need no lock: their producer runs the application's threads on a
virtual counter (`checks/cpu-npu-concurrency/producer.c`), so two runs write the same receipt whatever the host load.
`scheduler-control-linked.py` needs no lock either: the wake bound of its yield models is the critical path described above, counted in
instructions, and its worst values (41,924 and 17,022 instructions) were the same in a run on a host at a load of about 20 and in a run
beside 24 busy processes on 2026-11-13. Its other models compare guest ticks only with liveness bounds. Those tick counts are not
always the same from run to run, because QEMU's round-robin of the vCPUs on one thread usually repeats exactly but not always under
host load (in one full run of the check beside 24 busy processes, 11 of 71 QEMU logs differed from a quieter run). The local-wake phase of
`scheduler-partitioned-qemu` takes 905 ticks in nearly every run, 1,600 in about one run in fifty under host load and 1,891 under the
heaviest perturbation, against a bound of 7,600; each progress wait of `scheduler-objects-qemu` has 4,000 ticks, and how close a run
comes to that is not recorded.
The perturbed partitioned models passed at the gate's delays (300, 1,000 and 3,000 turns) and failed at others, on bringup as much as on
the branch that reported it, and every run of one delay gave the same verdict (20 of 20 runs, one output hash). Three causes, each found
from a trace of one failing delay, are fixed in the fixture and the storm platform, not in the kernel:
the controller polls the progress of a lower-priority task (the shielded-event task, the movers) with `vTaskDelay(1)`. With 3,500 turns at
`owner-drain` the stretch between the controller's block and the switch that follows it took 11 ticks of the shared virtual clock, the shield
was selected on the same tick that the controller's one-tick delay expired, and it was preempted before it executed an instruction, for
7,000 polls in a row. Every poll in `checks/scheduler-partitioned-qemu.c` now sleeps `limit(1)` ticks with a fixed count, so the window
a lower-priority task gets grows with the injected delay and is one tick at none. The `published on SGI 3` assertion of the band phase read
the SGI count at the moment the band task ran; at some `owner-publish` delays the owner's own tick takes the hint before the doorbell, so
the count rises one tick later. The check now waits, within the scaled bound, for the count to rise; `band-publication-sgi0`, which drops
the SGI, still fails it. The storm model's interrupt fires every 33,000 instructions and each interrupt exit runs owner work; once an exit
costs as much as that period (about 2,800 turns at `owner-drain`) the core runs only exits and the model prints no verdict. The
storm period (`platform/qemu-smp/platform.c`) grows with the injected turns, and the reply and notification timeouts of the workers
scale with `limit()`, because a saturated core 0 can leave a worker ready for longer than 2,000 ticks (at 2,500 turns of `owner-drain` in
the storm model worker 0 stayed ready for 2,000 ticks while core 0 spent most of its samples in the delay loop). The storm model still
prints no verdict within 15 minutes at 5,000 turns of `owner-drain`, where it finishes in 7 s at 4,750; the sweep's kill limit reports that
as a timeout.
The perturbation sweep kills a QEMU run that exceeds eight times the slowest measured point,
85.7 s, recorded in the same file; the slowest run took 192 s beside 24 busy processes. Other gates' work still runs beside them. A gate that wants the lock writes a ticket in `~/.cache/sunstoneos/gate-timing-queue/` and takes the lock only when its ticket is the first live one, landings before workers and then oldest first, as for the slots; a free lock is not taken past an earlier waiter, and the gate that holds it keeps it until its step ends. A landing therefore waits only for the step that runs, not for the workers queued before it (measured on 2026-11-13: landing gates waited up to 475 s for this lock). The list is `timing_checks` and `timing_steps` in the same file. A check's
pass criteria do not change. `checks/gate-slots.py` runs two gates at once, the exclusive timing lock, a landing that goes ahead of an
older worker, a landing that takes the timing lock ahead of an older waiting worker, and a killed waiter.

Every check runs from the source directory with standard input closed and one
environment: the process environment, `PYTHONDONTWRITEBYTECODE=1`,
`SUNSTONEOS_DIR` set to the source, and the cached LLVM 21 `bin` directory first
on `PATH`. The gate records it in `environment.txt`. A check does not source
`cfg.sh`; only the configure steps use it. `checks/check-index.py --run` uses the
same variables, so a check that passes in one place passes in the other.
`timing.txt` lists the steps by duration.

A check whose failure can come from documentation prose alone reports a `WARN`
line and does not fail the gate. The index entry `prose` marks them:
`docs-links.py` in full, and `progress.py` for the two test cases that read the
repository's status records and hardware inventory (`PROSE_CHECKS`). A failure
in any other case of `progress.py`, a crash, or a timeout is a `FAIL`. Code,
build, test and runner checks always fail the gate. `--strict-docs` turns every
`WARN` into a `FAIL`; the integration milestone runs with it.

A check that matches the device-access text scan but opens no device is listed
in `HOST_ONLY` in the index and runs in the gate. These are `board-runner.py`
(a fake FEL and board under `--out`), `usb-disconnect.py` and
`usb-access-discover.py` (fixture sysfs trees). The entry `output_option` names
the option that receives each check's new output directory. Every other check
that reaches a device keeps its USB classification and is reported as PENDING;
a name in `HOST_ONLY` that is not pure-host fails `check-index.py --verify`.

Run the gate as `python3 tools/integration-gate.py SOURCE LABEL`; it takes a gate slot itself. `tools/configure.py` holds the host dependency
configuration.
`--build-root` selects a separate directory for new builds and gate evidence.
The gate builds and checks software only; board runs remain separately queued.

## Match coverage to the change

| Change | Initial verification | Broaden when |
| --- | --- | --- |
| Documentation | Review changed statements and links | Commands or build instructions changed |
| Build or packaging | Build the affected target and inspect the artifact | Toolchain, boot layout, or shared configuration changed |
| Local driver behavior | Affected transfer, error, or recovery case | Shared clocks, DMA, interrupts, or power behavior changed |
| Scheduler, memory, DMA | Relevant concurrency and ownership checks | Integration exposes shared-resource effects |
| mimus platform adapter | Affected fixture and host parity | Model loading, scheduling, or arithmetic changed |
| mimus arithmetic | Kernel fixtures and deterministic reference comparison | Arithmetic intentionally changes, requiring accuracy evaluation |
| Hardware repeatability | Predeclared repeated trial | Failure or variance changes the hypothesis |

Do not add tests that merely repeat implementation details. Do not run accuracy
studies for non-numerical changes. Preserve mimus restrictions on held-out data
and training. Its substantive parity and concurrency requirements remain valid.

## Resolve fixture setup before running cases

Construct one valid baseline fixture with the production preparation functions
where available. Check identities, sizes, hashes and configuration together;
each failure case should change only the field or behavior it exercises.
Declare modeled hardware boundaries explicitly. A modeled authority must not
be accepted as physical evidence by production tools.

Compile independent changed harness targets before their runtime cases and
report setup errors together. Before instruction emulation, inspect the final
ELF for required public entry points and layouts. Account for inlining and the
actual calling convention, including the width of integer return values.
Verify emulator privilege and interrupt-controller settings before execution.
Fixtures that start midway through device initialization must establish the
prerequisite ownership, clocks, generation and deadlines through actual setup
calls. Audit these prerequisites together before running the remaining cases;
assigning a later state does not establish the effects of earlier states.

Keep these checks under `checks/` with the shared policy described in the next
section. Reject empty dependency inventories when recording build evidence.
Resume only failed or affected steps.

Collect an executable's dependency inventory from its actual linked objects.
A compilation database can also contain unrelated targets. Report entries outside
the selected executable separately; their exclusion is not a source change.

## Apply the shared check policy

Host checks are standalone Python scripts. Run them with the host virtual
environment, `~/.cache/sunstoneos/venv/bin/python`, which is installed from
[checks/host-requirements.txt](../checks/host-requirements.txt). The NPU
reference checks and fixture tools are the exception: they run inside the pinned
NPU compiler image described in [npu-compiler.md](npu-compiler.md).

[checks/harness.py](../checks/harness.py) holds the policy that every check must
follow:

- **Assertions stay enabled.** Checks decide pass or fail with `assert`, which
  `python3 -O` and `PYTHONOPTIMIZE` remove. Importing the module under either
  setting stops the check. Checks that do not import it carry the same one-line
  `__debug__` guard.
- **Sanitizers stop at the first report.** Every host build that enables
  `-fsanitize=...undefined` also passes `-fno-sanitize-recover=all`. Without it,
  Clang and GCC print an undefined-behavior report and the program still exits
  with status 0. `SANITIZE` is the complete flag list for new checks.
  `environment()` adds `UBSAN_OPTIONS=halt_on_error=1` for child processes as a
  second guard.
- **Negative cases name their refusal.** `refused(action, reason, kind)` passes
  only when `action()` raises `kind` (by default `ValueError`) with `reason` in
  its message. An accepted input fails the case, and any other exception
  propagates. A fixture error, such as a `KeyError` while building the input,
  or a refusal from an unrelated earlier check therefore cannot count as the
  refusal that the case exercises. Pass the exception type that the validator
  declares, or a tuple when an injected failure raises another type, such as
  `OSError`.
- **Host tools resolve in one place.** `CACHE`, `CLANG` and `LLD` come from
  `SUNSTONE_CACHE`, `SUNSTONE_HOST_CLANG` and `SUNSTONE_HOST_LLD` when set.
  Their defaults are `~/.cache/sunstoneos`, `/usr/bin/clang-21` and the
  `ld.lld-21` link under the cache's `host/usr/bin`. New checks use these
  values instead of literal paths.
- **Reruns keep earlier attempts.** `Steps` records labelled commands in
  `steps.json` for checks that resume with `--run-only`. A step that passed with
  the same command is skipped. A retried step appends its command to the existing
  command log and writes `<label>-attempt<N>.log` beside the earlier log. Its
  earlier record moves into the step's `history`.
- **One reader supplies ELF layouts.** `Image(path, layout_names)` returns an
  image's loadable segments, symbol addresses, DWARF function addresses and
  structure member offsets. Checks and the returning-image builders use it;
  the builders copy `checks/harness.py` beside each image as
  `layout-helper.py`. `checks/storage-read-native.py` re-exports it for the
  checks that load that module.
- **Emulation hooks only the addresses a model handles.** A Unicorn code hook
  without an address range calls into Python before every emulated
  instruction. `hook_addresses(uc, addresses, callback)` registers one ranged
  hook per address. `instruction_sites(parts, match)` finds instruction
  addresses in loaded image segments, and `system_register_access(word)`
  matches MRS, MSR, SYS and SYSL. A model that acts only at symbols, a
  sentinel and system-register accesses hooks just those. A model that
  advances state on every instruction keeps the unranged hook.

The SRAM launchers that admit a prepared session share one AArch32 model,
[checks/sram_launcher.py](../checks/sram_launcher.py). Its `execute` runs the
actual launcher instructions with modeled system registers, counter and
reset registers, and its `verify` requires the published deadlines on
admission and an unchanged contract with restored registers on refusal. The
per-device `*-launcher.py` checks supply the session and contract formats,
the supervision deadline and the mutation cases.

## Find the checks that cover a change

[checks/check-index.py](../checks/check-index.py) builds an index of every
Python check under `checks/` except the policy and selector modules. It reads the
checks and documents in the checkout on every call, so the index always matches
the checkout and is never committed; `checks/index.json` is ignored by Git.
Each entry records:

- the first line of the check's docstring;
- the Markdown documents that name the check;
- its required command-line arguments;
- the areas of `~/.cache/sunstoneos` that it names, such as `sources` or
  `evidence`;
- whether it can reach USB devices, and whether it is a helper module that other
  checks load;
- its repository file and directory inputs, including literal `CHECK_INPUTS`
  declarations, inferred source paths, included headers and transitive fixtures.

A check is *pure-host* when it can run with no input other than a new output
directory: no USB access, no cached evidence or build output, and no other
required argument. Pinned sources and host toolchains under the cache are
allowed. Most checks are not pure-host, because they need an image, a bundle or
earlier evidence; the index names those inputs.

The tool regenerates the index before selection. It prints the index or selects
checks for changed paths:

```sh
python3 checks/check-index.py --json      # print the index
python3 checks/check-index.py --verify    # fail if a check cannot be classified
python3 checks/check-index.py --changed drivers/rtc include/a733_rtc.h
~/.cache/sunstoneos/venv/bin/python checks/check-index.py --changed drivers/rtc \
  --run --output /absolute/new/directory
```

`--changed` prints each selected check with the inputs it needs. Adding `--run`
runs only the selected pure-host checks, each with its own log under the output
directory, and writes `results.json`. The selection is textual. A check covers
a path when the check, or a `checks/` file that it names, names that path or a
directory above it, or reaches it through `#include`. A path assembled from
separate string segments is not found. An empty selection therefore does not
show that no check covers the change.

`--verify`, which the integration gate runs as its `check-index` check, fails
when a check does not parse, when `NOT_PURE`, `HOST_ONLY`, `PROSE_CHECKS` or
`SERIAL_CHECKS` names a check that no longer exists, when a `HOST_ONLY` check is
not pure-host, or when `checks/index.json` is tracked again.

## Scan outgoing commits before a push

[tools/git-hooks/pre-push](../tools/git-hooks/pre-push) runs before every
`git push` and refuses the push when an outgoing commit adds any of:

- a private key block (`-----BEGIN ... PRIVATE KEY-----`);
- a GitHub, AWS, Slack, OpenAI, Anthropic or Google API token in its published
  format, such as `ghp_` followed by 36 characters or `AKIA` followed by 16;
- a credential file by name: `.env` and `.env.*` other than
  `.env.example`, `.env.sample` and `.env.template`, an SSH private key such
  as `id_rsa`, a `.pem`, `.p12` or `.pfx` file, `credentials.json`, `.netrc`;
- a path into an assistant session's scratch or memory directory
  (`/tmp/claude-` or `.claude/projects/`); <!-- pre-push: allow -->
- in a commit message, an assistant co-author line, an assistant
  "Generated with" credit or an assistant session link.

The token rules also apply to commit messages. The outgoing commits of a ref
are those that the remote does not have. A new branch is compared with its
merge base with `origin/bringup`, and the hook leaves out commits that any of
the remote's tracking branches already hold. Only added lines are read, so
deleting a secret never blocks a push, and a merge contributes only the lines
that its conflict resolution wrote. The rules match formats, so a variable
named `token` or `key`, Python's `secrets` module and SHA-256 digests pass.
Each finding names the file and line, or the commit message line, and the rule.

A line containing `pre-push: allow` is skipped; use it on test fixtures that
must contain a sample. A credential file has no such marker, so a fixture
needs a different name.

The hook is installed once for every worktree of the clone:

```sh
git config core.hooksPath tools/git-hooks
```

Git stores the setting in the shared repository configuration
(`git rev-parse --git-common-dir`). The relative path resolves against the
root of the worktree that pushes, so a worktree runs the hook from its own
checkout, and a worktree whose branch predates the hook runs no hook.
The hook uses the affected-input selector to run its fixtures only when an
outgoing commit changes the hook, its fixture or a file in the fixture's
complete declared input inventory. The index uses that same inventory for this
process check; native checks retain inferred and declared endpoint dependencies. Driver and documentation pushes still receive the complete secret scan;
they do not repeat hook fixtures or build images. The input comparison includes
intermediate commits, so reverting a hook change does not skip fixture coverage.

[checks/pre-push-hook.py](../checks/pre-push-hook.py) pushes each blocked
class and each passing case from retained fixture clones and asserts the result.
Run it explicitly only when its inputs change:

```sh
~/.cache/sunstoneos/venv/bin/python3 -B checks/pre-push-hook.py \
  --output "$HOME/.cache/sunstoneos/build/my-task/hook-check"
```

The gate's fixture likewise requires `checks/integration-gate.py --output DIR`.
Each output directory must be new; fixture repositories remain available for
failure diagnosis. The hook places its fixture runs under `SUNSTONE_CHECK_OUTPUT`
or the default local `build/pre-push-hook` directory.

## Reuse valid evidence

A successful result remains valid when its relevant inputs are unchanged:

- Source and dependency contents used by the test.
- Toolchain, build configuration, and generated image.
- Test implementation, fixtures, and model contents.
- Hardware identity, peripheral configuration, and relevant device state.

A commit, conflict-free rebase, handoff, or final response is not an input change.
Use existing build dependencies and a small evidence record before introducing
new caching infrastructure. Missing dependency information prevents reuse only
for checks affected by that uncertainty.

Do not rebuild solely to synchronize comment-only source changes with a passing
image. Record the changed source hashes and why the edit does not affect generated
code; retain the image's original inputs.

A runner may skip a preserved passing case only when its artifact identity
matches. Reusing a result across different images requires an explicit comparison
of the affected inputs and behavior; the case name alone is insufficient.

Preserve successful executables, logs and relevant inputs before another build
can overwrite them. Existing evidence runners may already do this. Otherwise,
use [tools/evidence-snapshot.py](../tools/evidence-snapshot.py) immediately after
the check, while that worktree and build output are idle:

```sh
python3 tools/evidence-snapshot.py --output /absolute/new/evidence-directory \
  --source path/to/runtime.c --source path/to/header.h \
  --source path/to/check.c --source path/to/build-configuration \
  --artifact /absolute/path/to/executed-image \
  --artifact /absolute/path/to/result.log \
  --note 'Describe the check, result, and remaining limitations.'
```

Repeat source and artifact arguments for all relevant dependencies, fixtures,
generated sources and results. Optional `--tool` arguments preserve tool
executables without running them. The tool copies selected bytes and hashes,
records the repository revision and dirty state, and refuses an existing output
directory. A failed snapshot can leave an incomplete directory without a manifest.
Select a fresh output after resolving the failure. Snapshotting does not prove
execution or complete dependency coverage, and does not justify repeating a test.

Toolchains and shared libraries repeat across snapshots; one LLVM library is
138 MB. `--reuse <earlier snapshot>`, repeatable, hard-links a file instead of
copying it when an earlier snapshot recorded the same SHA-256 and that file's
current bytes still match. The manifest row then records `linked_from`. A file
on another file system, or one whose earlier copy no longer matches its record,
is copied as before. Linked files share their bytes, so never edit a snapshot
file in place. `checks/audit-build-harness-snapshot.py --output <new dir>`
checks this behavior on a temporary repository.

Build once per changed target. Flash only when the required running image differs.
Run hardware cases in one controlled session when practical. Do not rebuild or
reflash between cases that use the same artifact.

## Integration and repeatability

At a milestone, run acceptance cases that lack valid evidence on the final
candidate. Preserve earlier evidence that still applies. Lower-level checks do
not substitute for an integration case that has never run.

Repeated trials need a purpose and a sample count or stopping rule. Examples
include boot reliability, timing variance, race reproduction, and reconnect
behavior. A failed attempt is not permission to retry indefinitely.

Keep raw logs outside Git. Record an untested requirement as untested, even
when a related check passes.

## Host validation rules

A host check is worth its cost when it tests our own logic, reaches a path that the board cannot reach on demand (faults, races, refusals, recovery), guards a board run, or links the real code on both sides of an interface. A hand-written model of hardware, firmware or a vendor service is worth its cost only where it is anchored to a measurement or a reference.

1. A behavioral model of hardware, firmware or a vendor service models only behaviors that cite a board measurement (a queue entry or a fact of `docs/hardware/a733-facts.json`) or a reference source (Linux, TF-A, a vendor driver, an Arm or Allwinner specification section). The citation sits beside the modeled behavior.
2. Where both sides of an interface are our code, the check links the real code instead of a stand-in.
3. A mock refuses any operation or register access that it does not model. Use the strict register model and the host link described below, and the GIC model of `tools/hw_facts.py` for interrupt priorities.
4. Every bug fix adds a case that fails on the pre-fix code, and the commit message names the case.
5. The board is ground truth. When a board run fails where a host check passed, the model behind that check is wrong. The fix corrects the model and adds a case built from the board record, in the same change as the code fix.
6. A modeled behavior that the board can read back has a conformance case against a recorded board reading, as `checks/hw-facts.py` does for the GIC model. Where neither a measurement nor a reference exists, the check does not model the behavior; a board run tests it.

A strict model refuses accesses it does not cover, and a citation makes a modeling error traceable, but neither makes a modeled behavior correct. Only agreement with board readings does.

A Non-secure view of the GIC differs from the stored one: a priority stored as 0xd8 reads 0xb0 at Non-secure EL1. Compare priorities only through `include/a733_gic_priority.h` or `GicModel`; QEMU runs with one security state and cannot show a view error.

### Strict register model and host link

`checks/host/strict_mmio.h` defines a register table in which each register names its source and its write hook. The model refuses an offset that is not defined, an access of another width, a value that the write hook does not define, and a definition without a source. `checks/host_link.py` builds a driver module from its unchanged source with `checks/host/mmio/a733_mmio.h` ahead of `include/a733_mmio.h`, so the accessors reach the strict model and nothing else is replaced; it verifies that the host header defines every accessor of the real header. `checks/watchdog-ownership.py` uses it: the real `drivers/watchdog/a733_watchdog.c` against `checks/watchdog.c`, which cites the register map, the write key and the reload word and refuses the rest.

### Checks pending re-qualification

The checks below run by hand, but the full gate lists them as pending (`REQUALIFICATION` in `checks/check-index.py`) because their models include behaviors that cite neither a measurement nor a reference. They return to the gate when each uncited behavior is replaced by a measured or cited one.

| Check | Uncited behaviors |
| --- | --- |
| `a733-pcie-trace`, `pcie-qemu` | ATU, PHY lock, link-up and speed-change latencies; NVMe ready delay and controller model; PCI power-management D3 behavior; link loss after PERST. No PCIe board run exists. |
| `io-native-ordering` | TWI status sequence, reset values and the 30 microsecond flag delay; initial WDT0 values; posted-store completion at a DSB. |
| `memtest-machine` | SCTLR_EL1 reset, translation walk and PAR_EL1 format, DC ZVA, counter step and cache geometry. |
| `power-write-order` | PMIC bus event latency and the status after START (the model returns 0x18 where a START gives 0x08), USB reset minimums, initial register values. |
| `provider-clock-reference` | Bit-level healthy values of the USB2, radio, CE and TWI clock and pin fixtures. |

The protection for rails and clocks is coordinator approval and the runner, not a model of guessed behavior.

### Uncited layers in checks that stay in the gate

These checks link real code but rely on an uncited model of a device, card, register layer or service. A change to one adds citations or links the real counterpart.

| Check | Uncited layer |
| --- | --- |
| `rtc-alarm-ownership` | RTC alarm and callback register model |
| `sd-return-close`, `storage-read-retain`, `a733-mmc-adaptive-voltage` | MMC card model and PMIC register words |
| `usb-generic-host` | USB peripheral traffic, except the pinned RPLIDAR facts |
| `usb-host-ready-work` | xHCI controller timing and events |
| `os-gpu-thermal` | GPU service replies and thermal input |
| `os-thermal-cadence` | delayed sensor reads, clock completion and interrupt controller |
| `qemu-os-boot` | thermal sensors, PMIC, clock providers, USB transport and NPU and GPU services of the virtual board |
| `reset-latency` | WDT0 model |
| `board-probe-abort` | GPIO and crumb register behavior |
| `dma-adopted-return` | DMA and CCU register offsets and busy behavior |
| `gpu-texture-frame` | device progress and ring model |
| `jpeg-advance` | completion delay values |
| `npu-queue`, `npu-board-capture`, `npu-idle` (class e, stays) | NPU device model, USB device stub, idle gating registers |
