# Mimus application acceptance: historical status record 2026-11-01

This log holds the prose evidence of [the current status](../status/mimus.md) as it stood on the date above. It is not updated. Board results that have queue entries are in the records `docs/evidence/mimus.jsonl`.

Mimus runs through the SunstoneOS application kit from its own repository.
The acceptance branch `port/sunstoneos-acceptance-on-split` is based on CPU
engine revision `0cbc96950`. Commits `1a485bad4` and `8f0e0f127` supply paced
replay and CPU-only manifest permissions. The separate build patch selects
paced replay. The recorded result uses SunstoneOS `e124d65f`; the fresh paced
image uses `8008acb1`, including `bringup` `f9375cb7`. The cached C++ layout
includes the CPU worker's fix. No duplicate linker patch is required.

## Recorded input passes physical acceptance

Queue entry `1793516356151336408-cac6790a` on port 3-1 returns normally with
seven secondary CPUs OFF and complete reference parity. The transcript is
`There was silence.` Feature CRC is `ac657a3b`, probability CRC is `a7ade80e`,
and the 57,440-sample recording produces 90 frames.
Recognition reports `compute_us=135673` and `total_us=135715`; loading and
packing take `92095` microseconds before recognition. Packed weights occupy
13,496,816 bytes. The compute duration is below the reference's 179.5 ms limit.
This single observation does not qualify corpus latency or thermal behavior.

The image reports CPU-L/B at 1794/2002 MHz and 1000/1050 mV, with three
completed domain transitions. The printed DSU rate of 494 MHz is the admission
snapshot from `apps/os/cpu.c:mhz`; it does not measure the active DSU clock.

## Paced recognition and thermal capture pass six finite runs

The paced image uses OS build `8008acb1`, including `bringup` `f9375cb7`,
and the same external CPU engine and inputs. Every run on 3-1 returns normally,
passes full segmented parity, retires seven secondary CPUs and completes rail
cleanup. Every thermal report is version 2 and classifies complete.

| Run and queue entry | Outcome | Compute (microseconds) | Maximum pacing lateness (microseconds) | Maximum thermal-slot lateness (microseconds) |
| --- | --- | --- | --- | --- |
| Control, `1793560017211439012-ff8c6091` | returned | 92585 | 415 | 891 |
| Repeat 1, `1793560102532814944-02caa432` | returned | 92670 | 932 | 887 |
| Repeat 2, `1793560276720214904-91d5e0c3` | returned | 92703 | 388 | 889 |
| Repeat 3, `1793560482537824782-7589f457` | returned | 92607 | 415 | 903 |
| Repeat 4, `1793560555177262469-0e251bd4` | returned | 92719 | 910 | 885 |
| Repeat 5, `1793560636888545789-06c0bafb` | returned | 92610 | 901 | 901 |

All runs process 57,440 samples and consume one segment spanning
`[19520, 57440)`. Feature CRC is `efcd0911`, probability CRC is `c78d3801`,
and formatting produces `There was silence` followed by flush `.`.
The five repeats have median compute 92670 microseconds and range 92607–92719.
Replay elapsed time ranges from 3683089 to 3683697 microseconds, and speech-end
latency ranges from 93060 to 93666 microseconds across those repeats.

The producer preserves absolute 25 ms sensor and 125 ms report phases.
Version 2 headers carry the first selected scheduled time through USB and
returning capture. Each sample must match its consecutive slot within 2001
microseconds: one 1 ms wakeup tick, one 1 ms read/publication budget and one
microsecond of timestamp quantization. Longer delays and missing slots fail.
This is an acceptance budget rather than a worst-case execution guarantee.
Version 1 reports retain their earlier decoding behavior.

The six runs qualify this finite input across fresh native sessions. They do
not establish retained-state replay, sustained multi-utterance capture or
thermal/fault recovery. The metadata watcher path issue and its direct-result
workaround are recorded in `cadence-1101/metadata-watch-note.md`.

## Host validation passes

OS-only, hello-compute and recorded application baseline builds pass. The
paced image also passes linked-image and public-interface checks. Actual kit
adapters with eight FreeRTOS workers pass recorded and paced QEMU parity.
The QEMU kit grant models five regions; physical recorded admission checks the
production manifest's six regions, including packed weights.
Eight native cadence cases and nine versioned decoder cases pass, including
USB/returning equivalence, jitter and missed-slot refusal. The linked version 2
image passes capture geometry and immutable-window checks.
Both adapter sanitizer checks and QEMU parity retain their evidence for the
unchanged application sources. Fresh baseline builds, the CPU clock model,
three returning-watchdog cases, the selected integration gate and check index
pass on the updated OS. Independent review finds no blocking rebase or build-patch defect.
Firmware and host sources equal the requested engine revision.

Current images, submission commands, transcripts and per-run parity are in
`~/.cache/sunstoneos/build/app-acceptance-1031/cadence-1101/`;
`paced-series.json` records per-run metrics and `shared-patches/` holds status
integration patches. Recorded and earlier host evidence remain in the sibling
`on-split/` directory. Sustained replay, live input,
session expiry and accelerator-assisted execution remain unqualified.
