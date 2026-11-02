# Mimus application acceptance

**Status: implemented.**

Mimus runs through the SunstoneOS application kit from its own repository. The acceptance branch `port/sunstoneos-acceptance-on-split` is based on CPU engine revision `0cbc96950`; the recorded result uses SunstoneOS `e124d65f` and the paced image uses `8008acb1`.

**Software evidence:** host.

**Board-qualified scope:** One recorded 57,440-sample input on port 3-1 returns
normally with seven secondary CPUs off and complete reference parity: 90 frames,
transcript `There was silence.`, compute 135.673 ms against the 179.5 ms limit.
A paced replay of the same input passes six fresh native sessions with full
segmented parity, complete version 2 thermal reports and rail cleanup; median
compute is 92.670 ms. These runs do not qualify corpus latency, retained-state
replay, sustained multi-utterance capture or thermal and fault recovery.

## Open work

1. Qualify sustained replay and multi-utterance capture on the board.
2. Qualify live input and session expiry.
3. Qualify accelerator-assisted execution; the qualified runs use the CPU engine only.
4. Qualify retained-state replay and thermal and fault recovery.
5. Fix the metadata watcher path issue so that the direct-result workaround in `cadence-1101/metadata-watch-note.md` is no longer needed.

## Evidence

Board results with a queue entry are records in `docs/evidence/mimus.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-mimus-20261101.md).

<!-- evidence-table:begin -->
7 records in `docs/evidence/mimus.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793516356151336408-cac6790a` | 2026-11-01 | 3-1 | `3f94df4af6e2` | returned | passed | run_total 69.532 s, compute 135673 µs, total 135715 µs, load_and_pack 92095 µs, frames 90 frames |
| `1793560017211439012-ff8c6091` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 71.663 s, compute 92585 µs, pacing_lateness_max 415 µs, thermal_slot_lateness_max 891 µs |
| `1793560102532814944-02caa432` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 71.563 s, compute 92670 µs, pacing_lateness_max 932 µs, thermal_slot_lateness_max 887 µs |
| `1793560276720214904-91d5e0c3` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 73.642 s, compute 92703 µs, pacing_lateness_max 388 µs, thermal_slot_lateness_max 889 µs |
| `1793560482537824782-7589f457` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 71.887 s, compute 92607 µs, pacing_lateness_max 415 µs, thermal_slot_lateness_max 903 µs |
| `1793560555177262469-0e251bd4` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 71.501 s, compute 92719 µs, pacing_lateness_max 910 µs, thermal_slot_lateness_max 885 µs |
| `1793560636888545789-06c0bafb` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 71.773 s, compute 92610 µs, pacing_lateness_max 901 µs, thermal_slot_lateness_max 901 µs |
<!-- evidence-table:end -->
