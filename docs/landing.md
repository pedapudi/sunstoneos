# Landing a branch

`tools/land.py` lands a worker branch on `bringup` without the coordinator. It
merges `bringup` into the branch, runs the checks that the change earns,
fast-forwards `bringup` and pushes it. The checks scale with risk: a change to
`docs/` runs three quick checks and no gate, and any other change runs the gate at landing, reusing
a passing gate when one exists (below). The full suite runs afterwards on the `bringup` tip, in its
own process, and blocks nothing. `checks/land.py` checks the tool against a
temporary origin.

## Use

```
tools/land.py WORKTREE --reason TEXT [--approved-by coordinator]
```

`WORKTREE` is the worker's own worktree, on its own `work/` branch, with every
change committed. `--reason` is the text passed to the gate. `--approved-by
coordinator` lifts the protected-path refusal and no other refusal. Only the
coordinator passes it.

Further options serve the check: `--cache`, `--bringup-worktree`, `--remote`,
`--wait SECONDS` (wait for the lock instead of refusing at once),
`--gate-command`, `--evidence-command`, `--links-command`, `--registry-command`,
`--affected-command` and `--full-gate-command`. Each command option also reads an
environment variable (`LAND_GATE_COMMAND`, `LAND_EVIDENCE_COMMAND`,
`LAND_LINKS_COMMAND`, `LAND_REGISTRY_COMMAND`, `LAND_AFFECTED_COMMAND`,
`LAND_FULL_GATE_COMMAND`). They exist so that the check
can replace the gate; a real landing uses the defaults.

## Checks by risk

The tool compares the diff against `bringup` with the glob `docs/**`.

- **Only `docs/`, evidence records included.** The tool runs `tools/evidence.py
  validate`, `tools/evidence.py sync --check` and `checks/docs-links.py` in the
  worker's worktree, and no gate. The landing record names the gate `docs-only`.
  A Markdown file outside `docs/` is code for this purpose.
- **Anything else.** The tool starts the gate, `integration-gate.sh TREE LABEL
  --reason TEXT --keep-going` with `--affected BASE..TIP` or, when it reuses a
  gate (below), `--changed PATH...`, and runs the check of the gate declarations
  (`checks/gate-inputs.py`) beside it. The gate selects the checks whose inputs
  the diff touches and reports every failure. If that check finds a check that
  declares no `GATE_RUNS`, `GATE_BOARD` or `GATE_WAIVED`, the tool stops the gate and refuses.
  The gate's output goes to `~/.cache/sunstoneos/build/land-LABEL-gate.log`. The gate passes when it exits 0 and
  `~/.cache/sunstoneos/build/gate-LABEL/summary.txt` has a `PASS selected` line and no `FAIL` line; a refusal quotes the failing
  lines and the last 30 lines of the log, which is all there is when the gate exits at argument validation and writes no summary.

The gate runs in the integration worktree `~/.cache/sunstoneos/land-integration`,
which the tool detaches at the commit to land. Every code landing gates there,
so the build directories persist from one landing to the next and a gate rebuilds
only what changed, and a worker cannot move the tree under the gate.

## The gate runs once

A worker builds and runs the checks it changed while it works. It does not run
the affected gate after each commit: the gate runs at landing. A worker that wants
an early result runs `integration-gate.sh TREE LABEL --affected bringup..HEAD` once
on a clean tree before it asks for a landing.

A passing `--affected` gate on a clean tree writes `gate-record.json` in its
output: the git tree it ran on, the commit its range starts at, and the checks it
selected with their inputs. At landing, the tool picks the recorded gate whose
range starts at a commit already on `bringup` and whose tree differs least from the
merged tip, and applies one rule: **every check affected by any file that differs
from that gate's tree is run on the landed tree.**

- If no check is affected by those files, the gate is the landing's evidence and
  nothing reruns. The landing record names the gate `reused-` and the gate's
  directory; the check of the gate declarations still runs.
- Otherwise the tool runs the gate with `--changed` over the differing files: those
  checks and the build of every image they use.
- With no such gate, the tool runs `--affected BASE..TIP`, every check the branch affects.

## Sequence

1. The tool refuses a worktree that has uncommitted or untracked files, and a
   worktree that is not on a work branch.
2. It takes the lock `~/.cache/sunstoneos/land.lock` and holds it until the push
   completes. A second lander finds the lock held and refuses; no lander waits
   unless `--wait` is given. The lock keeps `bringup` from moving under a gate.
3. It fetches the remote and requires the local `bringup` to contain the remote
   `bringup`, and the `bringup` worktree to be clean and on `bringup`.
4. It merges `bringup` into the branch with `git merge`. Any unmerged path
   stops the tool. The tool never resolves a conflict: it leaves the merge in
   place in the worker's worktree, and the worker resolves it, commits and runs
   the tool again.
5. It requires that `bringup` is an ancestor of the merge result and that the
   branch holds at least one commit beyond `bringup`.
6. It compares the diff against `bringup` with the protected paths (below).
7. It runs the checks of the diff's tier (above).
8. It requires that the worktree is still clean and unchanged. If `bringup`
   moved during the checks, it merges again and checks again, up to three times.
9. It fast-forwards `bringup` in the `bringup` worktree with `git merge
   --ff-only` and pushes `bringup` to the remote. If the push fails after the
   fast-forward, the tool reports it and the push is repeated by hand; the
   tool does not undo the fast-forward.
10. It prints the landed commit and the gate label, and appends one line to
    `~/.cache/sunstoneos/landings.jsonl` with the time, branch, landed tip, gate
    label, reason and approver. After a code landing it starts `tools/full-gate.py`
    in a new session and does not wait for it.

The tool never deletes files, never uses `git stash`, `git reset --hard` or
`git clean`, never forces a push and never pipes the output of `git merge`. It
exits with status 1 and prints `REFUSED:` and the cause for every refusal.

## Evidence logs

`.gitattributes` marks `docs/evidence/*.jsonl` with `merge=union`. Each log is
append-only, so two branches that each add a record merge to a log with both, and
`tools/evidence.py validate` accepts the result. The status tables that
`tools/evidence.py sync` generates in `docs/status/*.md` are not covered: two
branches that add records to one subsystem still conflict in that table, and the
second worker regenerates it with `sync` after merging.

## The gate lock

`tools/integration-gate.py` takes the host-wide lock `~/.cache/sunstoneos/gate.lock`
and waits for it when another gate holds it, so a direct call and a landing never
run gates side by side. The full suite takes `~/.cache/sunstoneos/gate-full.lock`
instead, so that it blocks no landing and no worker's gate.

## Full suite on bringup

`tools/full-gate.py` runs the gate with `--full` in its own worktree,
`~/.cache/sunstoneos/full-gate-tree`, detached at the `bringup` tip. A code landing
starts it; the coordinator can run it by hand. One run at a time: a run that
finds another running exits at once, and the next landing's run covers the newer
tip. It writes `~/.cache/sunstoneos/full-gate.txt` and appends one line to
`~/.cache/sunstoneos/full-gate.jsonl` with the commit, label, duration, verdict,
failing steps and the last passing commit. A failure names the failing steps and
the commit range since the last passing run, `LAST_PASS..TIP`; the first bad
commit lies in that range.

## Protected paths

`tools/land-protected.txt` lists protected globs, one per line, each group with
a comment that states the reason. `*` matches within one path component and
`**` crosses components. The tool reads the list from `bringup`, so a branch
cannot widen its own permission by editing the list. A diff that touches a
protected path is refused unless `--approved-by coordinator` is given.

The list covers:

- the board runner, image preparation, USB access and USB power tools, and the
  host side of the normal-return application;
- the default-kernel scheduler and port patches;
- every source that sets a CPU, DSU, GPU or NPU clock or voltage or a PMIC
  rail: the CPU clock owner, clock leases, the performance-mode service, the
  PMIC driver, rail leases, power domains, the GPU power and clock sources,
  the media clock owner, the NPU start-up, the display PLL and DRAM
  programming;
- the storage write paths: the card write driver, the storage update service,
  and the tools that prepare or update cards;
- `tools/land.py` and `tools/land-protected.txt`.

A new file that sets a clock or voltage, or writes storage, is added to the list
in the same commit that adds the file, and that commit is landed by the
coordinator.

## Check

`checks/land.py --output DIR` builds a bare origin, a `bringup` worktree and
worker worktrees in the new directory `DIR`, and replaces the gate with a stub.
It covers a code landing in affected mode, the three cases of the rule above
(a reused gate, a partial rerun and the full run when no gate exists), the `docs/`-only landing that runs the
evidence and link checks and no gate, the full suite starting after a code
landing, a landing when `bringup` moves during the gate, the refusals for unmerged
paths, a failed gate, a check without gate declarations, protected paths, a dirty
worktree, a branch with nothing to land and a held lock, and the coordinator
approval. It also covers `tools/full-gate.py`, the union merge of two appended
evidence records and the gate lock of `tools/integration-gate.py`.
