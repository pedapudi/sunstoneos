# Mimus application acceptance

**Status: implemented.**

**Software evidence:** host.

**Board-qualified scope:** One recorded 57,440-sample input on port 3-1 returns
normally with seven secondary CPUs off and complete reference parity: 90 frames,
transcript `There was silence.`, compute 135.673 ms against the 179.5 ms limit.
A paced replay of the same input passes six fresh native sessions with full
segmented parity, complete version 2 thermal reports and rail cleanup; median
compute is 92.670 ms. A 20.46 s recording (512 encoder frames) on the eight A76 cores returns normally with the host engine's transcript and checksums: compute 599.4 ms on the CPU engine, 422.8 ms with the encoder's 16 layers on the NPU, 308.6 ms with the subsampling on the NPU as well, no conversion between NPU graphs that clips codes, and the entry and convolution jobs on alternating graph copies (317.7 ms without the copies; two earlier runs of the subsampling missed their timing predictions; [NPU status](npu.md)), and 271.5 ms with the subsampling's mask applied inside its first convolution instead of by a shader multiplication. These runs do not qualify corpus latency, retained-state
replay, sustained multi-utterance capture or thermal and fault recovery.

Mimus runs through the SunstoneOS application kit from its own repository ([apps/mimus/source.json](../../apps/mimus/source.json)).

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Recorded input through the application kit, CPU engine | qualified on the board | 135.673 ms compute against the 179.5 ms limit; 90 frames; exact transcript | `mimus-acceptance-summary` in [mimus.jsonl](../evidence/mimus.jsonl) |
| Paced replay, six fresh native sessions | qualified on the board | Median compute 92.670 ms; full segmented parity | [mimus.jsonl](../evidence/mimus.jsonl) |
| Recorded 512-frame input, CPU engine on eight A76 workers | qualified on the board | 599.4 ms compute; text and checksums equal the host engine's | `1794107014170351336-e7854a09` |
| Eight CPU workers | host-tested for the two-core SMP model and the OS build; run on the board only in the 512-frame record above | The record above | [mimus-eight-workers.md](../designs/mimus-eight-workers.md); [mimus-os-eight-workers.md](../designs/mimus-os-eight-workers.md) |
| Accelerator-assisted recognition | NPU qualified on the board at 512 frames; GPU host-tested | NPU: compute 422.8 ms against 599.4 ms on the CPU, 308.6 ms with the subsampling on the NPU as well, no conversion between NPU graphs that clips codes, and the entry and convolution jobs on alternating graph copies, and 271.5 ms with the subsampling's mask inside its first convolution (`1794512615335201916-41b66297`); GPU: see [GPU status](gpu.md) | [gpu-recognition.md](../designs/gpu-recognition.md) |
| Sustained replay, multi-utterance capture, live input, session expiry, retained-state replay, thermal and fault recovery | open | None | None |

## Open work

1. Qualify sustained replay and multi-utterance capture on the board.
2. Qualify live input and session expiry.
3. Qualify GPU-assisted recognition on the board.
4. Qualify retained-state replay and thermal and fault recovery.
5. Fix the metadata watcher path issue so that reading each result directly from the result file, the current workaround, is no longer needed.

## Evidence

Board results with a queue entry are records in [mimus.jsonl](../evidence/mimus.jsonl), added with `tools/evidence.py add`; the revisions and queue entries of the acceptance runs are in the record `mimus-acceptance-summary`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-mimus-20261101.md).

<!-- evidence-table:begin -->
9 records in `docs/evidence/mimus.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `mimus-acceptance-summary` |  |  | `` | returned | passed | recorded_samples 57440 samples, frames 90 frames, recorded_compute 135.673 ms, compute_limit 179.5 ms, paced_sessions 6 sessions, paced_compute_median 92.67 ms |
| `1793516356151336408-cac6790a` | 2026-11-01 | 3-1 | `3f94df4af6e2` | returned | passed | run_total 69.532 s, compute 135673 µs, total 135715 µs, load_and_pack 92095 µs, frames 90 frames |
| `1793560017211439012-ff8c6091` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 71.663 s, compute 92585 µs, pacing_lateness_max 415 µs, thermal_slot_lateness_max 891 µs |
| `1793560102532814944-02caa432` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 71.563 s, compute 92670 µs, pacing_lateness_max 932 µs, thermal_slot_lateness_max 887 µs |
| `1793560276720214904-91d5e0c3` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 73.642 s, compute 92703 µs, pacing_lateness_max 388 µs, thermal_slot_lateness_max 889 µs |
| `1793560482537824782-7589f457` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 71.887 s, compute 92607 µs, pacing_lateness_max 415 µs, thermal_slot_lateness_max 903 µs |
| `1793560555177262469-0e251bd4` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 71.501 s, compute 92719 µs, pacing_lateness_max 910 µs, thermal_slot_lateness_max 885 µs |
| `1793560636888545789-06c0bafb` | 2026-11-01 | 3-1 | `d789333c6090` | returned | passed | run_total 71.773 s, compute 92610 µs, pacing_lateness_max 901 µs, thermal_slot_lateness_max 901 µs |
| `1794107014170351336-e7854a09` | 2026-11-08 | 3-1 | `6d666ea47bfd` | returned | passed | run_total 78.613 s, frames 512 frames, compute_time 599368 µs, load_time 93800 µs, front_end_time 9257 µs, subsampling_time 161016 µs, attention_time 240826 µs, convolution_module_time 74647 µs, feed_forward_time 96971 µs, layer_norm_time 3205 µs, residual_time 13797 µs, head_time 9368 µs, punctuation_time 1476 µs, arena_peak 4.05778e+06 bytes |
<!-- evidence-table:end -->
