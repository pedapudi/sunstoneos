# CPU recognizer qualification

The OS includes GPU and NPU integration through `29629d68`, merged in
`438c878c`. Writable C++ constructor and GOT metadata remain in image RAM,
and the clock record publishes the applied DSU frequency. The external
application record selects mimus `8458509c8` and CPU engine `433086425`.

Three baseline builds pass: OS services without an application, hello-compute
without mimus, and the default external mimus application. All eight selected
integration checks pass, including the check index with 773 checks and
86 pure-host checks. The single integration gate stopped at a stale application
engine pin; only failed or unexecuted steps resumed after its correction.
Focused GPU client, application ABI and eight table-walk cases also pass.

Independent review finds no remaining blocking defect in parallel attention,
scaled packed matrix tiles or immutable row producers. The final engine
preserves all 32 original references in QEMU. All 205 host engine tests,
196 int16 matrix cases, 35 raw packed-sum cases, 450 scaled-output cases and
eight output callback cases pass. Physical timing and worker race qualification
of these additional kernels remain pending.

The earlier physical control preserves reference output for 24 recordings at
CPU-L 1794 MHz and 1000 mV, CPU-B 2002 MHz and 1050 mV, and DSU 1274 MHz.
Its aggregate compute time is 0.051790 times audio duration; five recordings
exceed the 0.05 target. The 3.59-second reference has a median of 135.474 ms
across three runs, with a 135.394–135.506 ms range. Eight control recordings
remain unverified: batch 7 has no retained completion after interruption,
and batch 8 is unsubmitted.

Board work waits for USB recovery and the queue daemon after the host reboot.
Completed image and hypothesis pairs must not repeat. No new physical timing
is available for the additional kernels.

Evidence is under `~/.cache/sunstoneos/build/cpu-perf-1031/`:
`merge-validation-29629d68/validation-record.json` records the merged software
checks, `final-parity32/summary.json` records final QEMU parity, and
`corpus-landing/result-before-gains.json` records the physical control.
The validation record names application `9eca2315e`; its engine and application
code are identical to the selected documentation-only revision `8458509c8`.
