# Applications and development execution

**Status: implemented.**

SunstoneOS supplies the FreeRTOS application kit, an OS-only image, the hello-compute control and optional service profiles. Applications own their algorithms and assets in external repositories.


**Board-qualified scope:** One eight-worker CPU-only mimus recording matches its
90-frame reference, retires seven secondary cores and restores RAM, boot clocks
and CPU rails. A corrected hello-compute control returns normally with seven
secondaries off. These inputs do not establish general recognition or startup
repeatability. [CPU status](cpu.md) identifies the result and failed controls.

## Open work

1. Run the checks that need external inputs (H.264 capture, SAE); the full gate lists nine remaining checks.
2. Qualify physical queue execution, recovery and application GPU recognition.
3. Qualify simultaneous CPU, GPU and NPU operation; the optional builds have host evidence only.
4. Qualify returning accelerator execution; [NPU status](npu.md) lists the finite images that already pass.
5. Add broader application inputs, native audio input, autonomous card boot, fault recovery and concurrent thermal qualification.

## Evidence

Board results are records in `docs/evidence/application.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-application-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/application.jsonl`.
<!-- evidence-table:end -->
