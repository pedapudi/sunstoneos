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
diagnosable. The gate-root files stay as they are: `summary.txt`, `commands.txt`, `timing.txt`, `environment.txt`, `gate-record.json`,
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

Builds run one at a time and finish before any check starts. A keep-going gate
then runs the checks in parallel. `--jobs N` bounds the workers and sets the `-j` of the builds (default: the gate's share of the
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
`jobs_per_slot` divides the usable cores across the slots. Change a measurement there, not the code, when the host or the gate changes.

The steps whose result depends on wall time hold the exclusive `~/.cache/sunstoneos/gate-timing.lock` while they run and run alone in
their gate: the soak, `scheduler-control-linked.py` (which also runs the yield models, whose wake bound is the critical path described above; its perturbation sweep kills a QEMU run that exceeds four times the slowest measured point, 85.7 s, recorded in the same
file; the runs count instructions, so the host load does not change them), `smp-ticket-lock.py`, `tick-footprint-budget.py`, `gpu-conformance.py`, `land.py`, `submit-wait.py` and the `SERIAL_CHECKS`. The two CPU/NPU
checks (`cpu-npu-concurrency.py`, `cpu-npu-return-chain.py`) need no lock: their producer runs the application's threads on a
virtual counter (`checks/cpu-npu-concurrency/producer.c`), so two runs write the same receipt whatever the host load. Other gates' work still runs beside them. The list is `timing_checks` and `timing_steps` in the same file. A check's
pass criteria do not change. `checks/gate-slots.py` runs two gates at once, the exclusive timing lock, a landing that goes ahead of an
older worker and a killed waiter.

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
