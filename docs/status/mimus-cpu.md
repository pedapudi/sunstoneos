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
6 records in `docs/evidence/mimus-cpu.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793563566905223485-7d25a24b` | 2026-11-01 | 3-1 | `3480a88fa7fa` | returned | passed | run_total 72.162 s |
| `1793563703824318733-0c21b2b6` | 2026-11-01 | 3-1 | `522874a27101` | returned | passed | run_total 71.388 s |
| `1793567533946545503-823190b1` | 2026-11-01 | 3-1 | `80b3056a23b7` | returned | passed | run_total 72.698 s |
| `1793570213701678324-a3d8ee82` | 2026-11-01 | 3-1 | `f287ae9eb1a5` | returned | passed | run_total 73.812 s, long_clip_time 990.664 ms, real_time_factor 0.033199 x |
| `1793597755723905157-bfbf69e0` | 2026-11-02 | 3-1 | `630613d5027f` | returned | failed | run_total 21.924 s, stream_engine_r1 3.721 ms, stream_engine_r2 3.928 ms, stream_engine_r7 7.986 ms, stream_engine_r14 15.381 ms, stream_engine_r64 28.167 ms, stream_split_r1 3.069 ms, stream_split_r2 3.088 ms, stream_split_r7 3.453 ms, stream_split_r14 5.329 ms, stream_split_r64 20.73 ms, packed_bytes 2.48026e+07 B, output_mismatches 0 words |
| `1793597859719548490-5567c545` | 2026-11-02 | 3-1 | `e3f09bb226fb` | returned | passed | run_total 21.701 s, resident_split_r1 1.566 ms, resident_split_r2 1.591 ms, resident_split_r7 3.161 ms, resident_split_r14 6.5 ms, resident_split_r64 27.97 ms, resident_engine_r1 4.906 ms, stream_split_r1 3.063 ms, stream_split_r7 3.453 ms, stream_split_r14 5.331 ms, output_mismatches 0 words |
<!-- evidence-table:end -->
