# CPU recognizer qualification

**Status: implemented.**

**Board-qualified scope:** Engine `bcaf39ff5`, selected explicitly in the qualified images, preserves all 32 original corpus references on the merged OS at vendor maximum CPU and DSU operating points. Aggregate real-time factor is 0.027888 and maximum is 0.032916. The 29.84-second clip takes 982.227 ms; the reference takes 109.551 ms. Paced entry `1793563703824318733-0c21b2b6` preserves segmented output. Thermal control entry `1793563566905223485-7d25a24b` and the paced entry have complete version 2 reports, with maximum slot lateness 957 and 951 microseconds. Per-core diagnostic entry `1793567533946545503-823190b1` returns normally with exact output and unchanged maximum clocks. Physical long-duration worker stress remains pending.

## Source and validation

The OS includes shared `1a84017e` and the independent snapshot fixes. Thermal capture preserves
absolute 25 ms sensor and 125 ms record phases, independent snapshots and a
complete fresh first row before startup load is released. Version 2 reports
retain their scheduled first slot and unchanged 2001-microsecond lateness budget.
Critical startup and later stale samples remain observable.

The [application source record](../../apps/mimus/source.json) selects mimus
`647aeb636`, whose declared engine is `bcaf39ff5`. The application embeds the
shared USB diagnostic service with its generic entry-point suppression flag.
Writable constructor and GOT metadata remain in image RAM; the storage linker
patch is reviewed against that combined layout. The OS core has no engine dependency.

Seventeen actual-owner thermal cases and nine versioned decoder cases pass.
Three baseline builds pass. The full integration gate builds every core target,
OS variant and application target. Its failed USB target and unexecuted checks
pass after the embedding correction: 99 continuation steps, including 89 indexed
host checks. The index contains 808 checks and 94 pure-host checks. Another
695 checks require external inputs; full physical subsystem coverage is pending.

The recorded image SHA-256 is
`3480a88fa7fa2edd67f9b559bc29a455a56736d1369f69e2a6d1489580bfd653`.
The paced image SHA-256 is
`522874a27101285fb8b3ae966a4b916298489aa3ee1341a163737450a75e97a4`.
Both return normally at CPU-L 1794 MHz and 1000 mV, CPU-B 2002 MHz and
1050 mV, with DSU 1274 MHz.

Evidence is under `~/.cache/sunstoneos/build/cpu-perf-1031/`:
`thermal-merge-1101/landing-validation.json`, `thermal-merge-1101/validation-record.json`
and `full-integration-1101/continued/commands-results.json` bind the merged
validation and physical results. The current corpus and profiles are in
`final-cpu2-1101/board/final-complete.json` and its sibling batch files.
The application performance report still describes the preceding engine.

## Landing work in priority order

1. Confirm the updated source pins on the merged OS with the affected checks,
   three baselines, full integration gate and one default-build board recording.
2. Build a fresh native race image, run its short control, then the four
   predeclared 50-second sessions in `race-test-1101/sessions.json` through
   the queue on port 3-1. The integrated QEMU screen passes; physical stress
   is unqualified. Keep the existing session limit and stop at failure or pause.
3. Complete the application performance report using the final corpus,
   per-core timing and physical stress receipts. The detailed continuation
   record is mimus `docs/status/sunstoneos-cpu.md` in
   `/home/sunil/git/mimus-worktrees/cpu-perf-1031`.

No native race image is built and awaiting its first run. Already-run image
READMEs are `native-producers-1101/README.md` and
`native-kernel-timing-1101/README.md` below the evidence root.
The complete corpus entry list is in
`final-cpu2-1101/board/entries.jsonl`; every entry returns normally and retains
exact outputs. The application status document lists each entry explicitly.

The coordinator owns landing this branch and advancing the mimus shared branch.
No approval is pending for the existing RAM-only queue plan; further board work
awaits coordinator resumption. Storage writes and new operating points require
coordinator approval. Coordinate shared thermal files `apps/os/thermal.c`,
`checks/os-thermal-cadence.c` and `checks/os-thermal-cadence.py` with the
acceptance worker. Coordinate `apps/mimus/source.json` with the integrator.
