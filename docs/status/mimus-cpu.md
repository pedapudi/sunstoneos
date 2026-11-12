# CPU recognizer qualification

**Status: implemented.**

**Board-qualified scope:** The CPU engine preserves all 32 original corpus
references on bringup at vendor maximum CPU and DSU operating points.
Aggregate real-time factor is 0.027510 and the maximum is 0.032972. The
29.84-second clip takes 983.871 ms; the reference takes 109.551 ms. A default
build confirms the application and engine pins with exact long-clip output in
990.664 ms (0.033199x), complete thermal evidence and normal return. Physical
long-duration worker stress is pending.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Corpus parity at vendor maximum clocks | qualified on the board | 32 of 32 references; aggregate real-time factor 0.027510, maximum 0.032972 (earlier merged OS: 0.027888 and 0.032916) | `mimus-cpu-corpus-bringup-1103` and `mimus-cpu-corpus-summary` in [mimus-cpu.jsonl](../evidence/mimus-cpu.jsonl) |
| Long clip (29.84 s) | qualified on the board | 983.871 ms against a 109.551 ms reference; default build 990.664 ms | `mimus-cpu-corpus-bringup-1103` |
| Paced segmented output with version 2 thermal slots | qualified on the board | Maximum slot lateness 957 and 951 microseconds | `mimus-cpu-corpus-summary` |
| Per-core diagnostic at vendor maximum clocks | qualified on the board | Exact output at unchanged maximum clocks | `mimus-cpu-corpus-summary` |
| Long-duration worker stress | open | None | None |

## Open work

1. Build a fresh native race image, run its short control, then four predeclared 50-second sessions through the queue on port 3-1. The session list is part of the image build and is declared before the first run. Stop at a failure or pause.
2. Complete the application performance report from the final corpus, per-core timing and the stress receipts. The continuation record is `docs/status/sunstoneos-cpu.md` of the mimus repository, which is not yet on its main branch.
3. Coordinate the shared thermal files (`apps/os/thermal.c`, `checks/os-thermal-cadence.*`, `checks/os-cpu-return-thermal.py`) and `apps/mimus/source.json` with their owners.

Writable constructor and GOT metadata stay in image RAM; the storage linker patch is reviewed against that layout. No approval is pending for the existing RAM-only queue plan; storage writes and new operating points need coordinator approval.

## Evidence

Board results are records in [mimus-cpu.jsonl](../evidence/mimus-cpu.jsonl); add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-mimus-cpu-20261101.md).

<!-- evidence-table:begin -->
8 records in `docs/evidence/mimus-cpu.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `mimus-cpu-corpus-bringup-1103` |  |  | `` | returned | passed | aggregate_real_time_factor 0.02751 x, maximum_real_time_factor 0.032972 x, long_clip_time 983.871 ms, corpus_references_preserved 32 references, total_compute 7166.44 ms, aggregate_change_against_baseline -1.36 percent |
| `mimus-cpu-corpus-summary` |  |  | `` | returned | passed | aggregate_real_time_factor 0.027888 x, maximum_real_time_factor 0.032916 x, long_clip_time 982.227 ms, reference_long_clip_time 109.551 ms, corpus_references_preserved 32 references, thermal_slot_lateness_control 957 µs, thermal_slot_lateness_paced 951 µs, default_build_long_clip_time 990.664 ms |
| `1793563566905223485-7d25a24b` | 2026-11-01 | 3-1 | `3480a88fa7fa` | returned | passed | run_total 72.162 s |
| `1793563703824318733-0c21b2b6` | 2026-11-01 | 3-1 | `522874a27101` | returned | passed | run_total 71.388 s |
| `1793567533946545503-823190b1` | 2026-11-01 | 3-1 | `80b3056a23b7` | returned | passed | run_total 72.698 s |
| `1793570213701678324-a3d8ee82` | 2026-11-01 | 3-1 | `f287ae9eb1a5` | returned | passed | run_total 73.812 s, long_clip_time 990.664 ms, real_time_factor 0.033199 x |
| `1793597755723905157-bfbf69e0` | 2026-11-02 | 3-1 | `630613d5027f` | returned | failed | run_total 21.924 s, stream_engine_r1 3.721 ms, stream_engine_r2 3.928 ms, stream_engine_r7 7.986 ms, stream_engine_r14 15.381 ms, stream_engine_r64 28.167 ms, stream_split_r1 3.069 ms, stream_split_r2 3.088 ms, stream_split_r7 3.453 ms, stream_split_r14 5.329 ms, stream_split_r64 20.73 ms, packed_bytes 2.48026e+07 B, output_mismatches 0 words |
| `1793597859719548490-5567c545` | 2026-11-02 | 3-1 | `e3f09bb226fb` | returned | passed | run_total 21.701 s, resident_split_r1 1.566 ms, resident_split_r2 1.591 ms, resident_split_r7 3.161 ms, resident_split_r14 6.5 ms, resident_split_r64 27.97 ms, resident_engine_r1 4.906 ms, stream_split_r1 3.063 ms, stream_split_r7 3.453 ms, stream_split_r14 5.331 ms, output_mismatches 0 words |
<!-- evidence-table:end -->
