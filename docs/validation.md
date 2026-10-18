# Select checks by changed behavior

Before a test batch, state the changed behavior, credible failure risk, and
smallest check that resolves that risk. Run focused checks during iteration.
Broaden testing when a shared dependency changes, a failure reveals wider impact,
or a milestone lacks integration evidence.

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

Keep these checks in the existing harness and reuse their outputs. Reject empty
dependency inventories when recording build evidence. Routine setup repairs
require owner verification; request another substantive review only when the
repair changes coverage, modeled boundaries or production behavior. Preserve
completed results and resume only failed or affected steps.

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
