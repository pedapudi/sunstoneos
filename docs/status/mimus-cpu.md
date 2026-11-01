# CPU recognizer qualification

**Status: implemented.**

**Board-qualified scope:** All 32 original corpus references match on the A733, including feature and
log-probability checksums, text, punctuation, capitals and formatting. Total
compute is 7.571656 seconds for 260.504 seconds of audio: aggregate real-time
factor 0.029065, with a maximum individual factor of 0.036796. Every recording
meets the 0.05 limit. The longest, 29.84 seconds, takes 1097.984 ms, below both
the 1492 ms limit and the conditional 1141.869 ms projection. Each final
recording has one physical observation; timing spread is unmeasured.

The qualified image SHA-256 is
`9c667caeaca9d0f4c73c8ba48113108234f1ef94ca83d53ad2fc6f1fa3f3aa49`.
Runs return normally at CPU-L 1794 MHz and 1000 mV, CPU-B 2002 MHz and
1050 mV, and DSU 1274 MHz. Retained thermal coverage is complete for the
first recording in each batch. The thermal policy remains active for later
recordings, whose individual thermal histories are not retained.

## Implementation and validation

The [application source record](../../apps/mimus/source.json) selects mimus
`ae253ad1c` and its declared engine `418ad38c8`. The engine combines parallel
attention, row-first fused linear scaling and private immutable producers.
Channel tiling is removed after its subsampling regression. Independent review
finds no remaining blocking defect. All 32 references also match in QEMU;
205 host engine tests, 196 int16 matrix cases, 35 raw SDOT cases, 450 scaled
output cases and eight callback cases pass.

The OS merge `94886b09` includes shared revision `6ebe687a` and preserves
writable C++ metadata and applied DSU telemetry. Thermal fixes `9c598bb7`
and `f253e59d` preserve the 125 ms coverage limit and critical failure evidence.
All 16 focused thermal cases and the CPU-clock baseline pass. Three baseline
builds and selected integration checks pass; all 45 runnable affected checks
are covered. Another 290 checks require external inputs and remain pending.
The check index contains 791 checks, including 89 pure-host checks.
Long-duration worker race stress remains outside this corpus qualification.

Evidence is under `~/.cache/sunstoneos/build/cpu-perf-1031/`:
`queue-qualified-1101/final-complete.json` and
`queue-qualified-1101/summary.json` record the final
corpus; `merge-validation-6ebe/validation-record.json` records integration
coverage; `thermal-startup-1101/validation-record.json` records the thermal fix.
The application's `docs/sunstoneos_cpu_performance.md` contains all 19 queue
outcomes, per-recording results, stage-scaling tables and paths to every raw
PROFILE record, including rejected
performance hypotheses and the limits of the projection.
