# Landing a branch

`tools/land.py` lands a worker branch on `bringup` without the coordinator. It
performs the steps that the coordinator performed by hand: merge `bringup` into
the branch, run the full gate, validate the evidence records and the progress
view, fast-forward `bringup` and push it. `checks/land.py` checks the tool
against a temporary origin.

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
`--gate-command`, `--evidence-command`, `--progress-command` and
`--registry-command`. The four command options also read `LAND_GATE_COMMAND`,
`LAND_EVIDENCE_COMMAND`, `LAND_PROGRESS_COMMAND` and `LAND_REGISTRY_COMMAND`.
They exist so that the check can replace the gate; a real landing uses the
defaults.

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
6. It compares the diff against `bringup` with the protected paths (below),
   then runs `checks/gate-inputs.py`. That check refuses, in about ten seconds,
   a check that the gate registry (`checks/gate-inputs.json`) neither runs nor
   waives with a reason. The gate would otherwise fail on that check after its
   full run.
7. It runs `~/.cache/sunstoneos/bin/integration-gate.sh WORKTREE LABEL --reason
   TEXT --full` with the label `land-BRANCH-MMDD-HHMMSS`. The gate passes when
   `~/.cache/sunstoneos/build/gate-LABEL/summary.txt` has a `PASS selected`
   line and no `FAIL` line.
8. It runs `tools/evidence.py validate` and `checks/progress.py --output DIR`
   with a new directory under `~/.cache/sunstoneos/build/`.
9. It requires that the worktree is still clean and unchanged. If `bringup`
   moved during the gate, it merges again and gates again, up to three times.
10. It fast-forwards `bringup` in the `bringup` worktree with `git merge
    --ff-only` and pushes `bringup` to the remote. If the push fails after the
    fast-forward, the tool reports it and the push is repeated by hand; the
    tool does not undo the fast-forward.
11. It prints the landed commit and the gate label, and appends one line to
    `~/.cache/sunstoneos/landings.jsonl` with the time, branch, landed tip, gate
    label, reason and approver.

The tool never deletes files, never uses `git stash`, `git reset --hard` or
`git clean`, never forces a push and never pipes the output of `git merge`. It
exits with status 1 and prints `REFUSED:` and the cause for every refusal.

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
It covers a clean landing, a landing when `bringup` moves during the gate, and
the refusals for unmerged paths, a failed gate, an unregistered check,
protected paths, a dirty
worktree, a branch with nothing to land and a held lock. It also covers the
coordinator approval.
