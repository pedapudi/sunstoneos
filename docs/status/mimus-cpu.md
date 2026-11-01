# CPU recognizer qualification

**Status: implemented.**

**Board-qualified scope:** Engine `418ad38c8` preserves all 32 original corpus references at vendor maximum CPU and DSU operating points. Its aggregate factor is 0.029065 and maximum is 0.036796, with one observation per recording. On the merged OS, entry `1793563566905223485-7d25a24b` returns the 29.84-second clip in 1097.939 ms with exact output. Paced entry `1793563703824318733-0c21b2b6` preserves segmented output and retires seven secondary CPUs. Both retained thermal reports are complete version 2 captures, with maximum slot lateness 957 and 951 microseconds. Retained thermal history covers only the first recording in each batch; policy remains active afterward. Long-duration worker stress remains pending.

## Source and validation

The OS merges shared `802ffef9` in `3470254e`. Thermal capture preserves
absolute 25 ms sensor and 125 ms record phases, independent snapshots and a
complete fresh first row before startup load is released. Version 2 reports
retain their scheduled first slot and unchanged 2001-microsecond lateness budget.
Critical startup and later stale samples remain observable.

The [application source record](../../apps/mimus/source.json) selects mimus
`10db8e721`, whose declared engine is `418ad38c8`. The application embeds the
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
validation and physical results. The earlier complete corpus and its profiles
remain in `queue-qualified-1101/final-complete.json` and the application
report `docs/sunstoneos_cpu_performance.md`.
