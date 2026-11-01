# Select checks by changed behavior

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

Use `checks/check-index.py --changed PATH...` to help find component coverage.
Its textual search cannot prove that an empty selection is sufficient. The
owner chooses coverage from the changed behavior and shared dependencies;
the gate does not infer it from filenames or launch every matching check.

`--full` explicitly requests all central targets, all configured OS variants of
[the OS image](os-image.md), including the supervised NPU
profile (variant `npu`) and all gate checks. The `npu` variant builds with the
catalogue map `~/.cache/sunstoneos/build/npu-board/session-artifacts.json`;
the OS build verifies every file it names against
`tools/npu-session-models.json`, the gate refuses the variant before any
build when the map is missing, and the summary records its SHA-256.
The optional GPU linear, mSBC, Cortex-A76 and CPU clock options are not
variants of the gate; build them directly with `cfg.sh` when a change
affects them. Reserve it for a shared change or milestone whose missing
integration evidence justifies that scope, and state why in `--reason`.
An integration commit alone does not justify it.

Each invocation uses a new label under `~/.cache/sunstoneos/build/gate-LABEL`.
It records the selection, reason, revision, commands and logs, and stops at the
first failure. The builds use one directory per source checkout,
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
Diagnose a failure and run only the affected work with a new label; reuse the
successful evidence from the earlier invocation. The soak defaults to one
trial; `--trials N` declares a repeatability sample in advance and needs a
reason for that sample count. It does not retry failures.

The host's existing `~/.cache/sunstoneos/bin/integration-gate.sh SOURCE LABEL`
command delegates to this repository tool and accepts the same selection
options. Host dependency configuration remains in `~/.cache/sunstoneos/bin/cfg.sh`.
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
section, and reuse their outputs. Reject empty
dependency inventories when recording build evidence. Routine setup repairs
require owner verification; request another substantive review only when the
repair changes coverage, modeled boundaries or production behavior. Preserve
completed results and resume only failed or affected steps.

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
Python check under `checks/` except the two policy modules. It reads the
checks and documents in the checkout on every call, so the index always matches
the checkout and is never committed; `checks/index.json` is ignored by Git.
Each entry records:

- the first line of the check's docstring;
- the Markdown documents that name the check;
- its required command-line arguments;
- the areas of `~/.cache/sunstoneos` that it names, such as `sources` or
  `evidence`;
- whether it can reach USB devices, and whether it is a helper module that other
  checks load.

A check is *pure-host* when it can run with no input other than a new output
directory: no USB access, no cached evidence or build output, and no other
required argument. Pinned sources and host toolchains under the cache are
allowed. Most checks are not pure-host, because they need an image, a bundle or
earlier evidence; the index names those inputs.

Generating the index takes about four seconds. The tool prints the index or
selects checks for changed paths:

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
when a check does not parse, when the tool's list of checks that cannot be
pure-host (`NOT_PURE`) names a check that no longer exists, or when
`checks/index.json` is tracked again.

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
needs a different name. The scan of the 73 commits in the last 50 first-parent
steps of `bringup`, about 20,000 added lines, takes 0.2 seconds; the full
history of 2,764 commits takes under 4 seconds.

The hook is installed once for every worktree of the clone:

```sh
git config core.hooksPath tools/git-hooks
```

Git stores the setting in the shared repository configuration
(`git rev-parse --git-common-dir`). The relative path resolves against the
root of the worktree that pushes, so a worktree runs the hook from its own
checkout, and a worktree whose branch predates the hook runs no hook.
[checks/pre-push-hook.py](../checks/pre-push-hook.py) pushes each blocked
class and each passing case from a temporary clone and asserts the result.

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

For each batch, record the purpose, command or procedure, artifact identity,
hardware state, result, and raw-evidence location. Keep raw logs outside Git.
Record an untested requirement as untested, even when a related check passes.

## Schedule performance work after functional hardware support

Defer CPU, GPU, and NPU optimization until every required board hardware function
works. During bring-up, measure only what is needed to diagnose a failure,
establish functional deadlines, validate recovery, or satisfy a specific
functional acceptance condition. Do not run recurring benchmark suites or
optimization experiments alongside unfinished hardware support.

After functional coverage, qualify CPU, GPU, and NPU performance separately.
Include application latency, sustained throughput, memory and transfer costs,
multicore scaling, concurrent workloads, and thermal behavior where relevant.
Compare equivalent workloads at recorded clock rates, precision, and correctness
requirements. Existing mimus latency requirements remain in force.

Every measurement batch needs a concrete question, relevant input identities,
and a declared sample count or stopping rule. Reuse valid baseline and candidate
results. Repeat only when changed inputs or unresolved variance require new
evidence; commits, handoffs, and documentation changes do not justify reruns.
