# CPU recognizer qualification

**Status: implemented.**

**Board-qualified scope:** Engine `bcaf39ff5` preserves all 32 original corpus
references on the merged OS at vendor maximum CPU and DSU operating points.
Aggregate real-time factor is 0.027888 and the maximum is 0.032916. The
29.84-second clip takes 982.227 ms; the reference takes 109.551 ms. Default-build
entry `1793570213701678324-a3d8ee82` confirms the pins on merged OS `4c48bce0`:
exact long-clip output in 990.664 ms (0.033199x), complete thermal evidence and
normal return. Physical long-duration worker stress is pending.

## Open work

1. Build a fresh native race image, run its short control, then the four predeclared 50-second sessions in `race-test-1101/sessions.json` through the queue on port 3-1. Stop at a failure or pause.
2. Complete the application performance report from the final corpus, per-core timing and the stress receipts. The continuation record is mimus `docs/status/sunstoneos-cpu.md` in `/home/sunil/git/mimus-worktrees/cpu-perf-1031`.
3. Coordinate the shared thermal files (`apps/os/thermal.c`, `checks/os-thermal-cadence.*`, `checks/os-cpu-return-thermal.py`) and `apps/mimus/source.json` with their owners.

Writable constructor and GOT metadata stay in image RAM; the storage linker patch is reviewed against that layout. No approval is pending for the existing RAM-only queue plan; storage writes and new operating points need coordinator approval.

## Evidence

Board results are records in `docs/evidence/mimus-cpu.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-mimus-cpu-20261101.md).

<!-- evidence-table:begin -->
4 records in `docs/evidence/mimus-cpu.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793563566905223485-7d25a24b` | 2026-11-01 | 3-1 | `3480a88fa7fa` | returned | passed | run_total 72.162 s |
| `1793563703824318733-0c21b2b6` | 2026-11-01 | 3-1 | `522874a27101` | returned | passed | run_total 71.388 s |
| `1793567533946545503-823190b1` | 2026-11-01 | 3-1 | `80b3056a23b7` | returned | passed | run_total 72.698 s |
| `1793570213701678324-a3d8ee82` | 2026-11-01 | 3-1 | `f287ae9eb1a5` | returned | passed | run_total 73.812 s, long_clip_time 990.664 ms, real_time_factor 0.033199 x |
<!-- evidence-table:end -->
