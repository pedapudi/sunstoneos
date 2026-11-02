# Applications and development execution

**Status: implemented.**

SunstoneOS supplies the FreeRTOS application kit, an OS-only image, the hello-compute control and optional service profiles. Applications own their algorithms and assets in external repositories.

The returning CPU image ends an application failure, a synchronous exception from an application task at EL0, a panic on a core that can schedule, an unexpected interrupt and a failure before the scheduler starts at the boot ROM with the first-failure record, instead of at a watchdog reset. Device failures, exceptions at EL1 or EL3, SError and an unfinished return still end at the watchdog. An NPU image that fails before its service ran publishes a never-started stop report, so the runner restores DRAM. [The failure-return design](../designs/failure-return.md) lists every failure path and its ending. `checks/os-failure-return.py` executes the linked image in each context on the host, `checks/qemu-os-boot.py` takes a real EL0 data abort on the virtual board, and `checks/board-return-preservation.py` covers the runner outcome. One forced application failure on the board returned through this path without a watchdog reset; the evidence table below records it.


**Board-qualified scope:** One eight-worker CPU-only mimus recording matches its
90-frame reference, retires seven secondary cores and restores RAM, boot clocks
and CPU rails. A corrected hello-compute control returns normally with seven
secondaries off. These inputs do not establish general recognition or startup
repeatability. [CPU status](cpu.md) identifies the result and failed controls.

## Open work

1. Run the forced EL0 data abort and an NPU image that fails before its service starts on the board; the EL0 return has host and virtual-board evidence only.
2. Run the checks that need external inputs (H.264 capture, SAE); the full gate lists nine remaining checks.
3. Qualify physical queue execution, recovery and application GPU recognition.
4. Qualify simultaneous CPU, GPU and NPU operation; the optional builds have host evidence only.
5. Qualify returning accelerator execution; [NPU status](npu.md) lists the finite images that already pass.
6. Add broader application inputs, native audio input, autonomous card boot, fault recovery and concurrent thermal qualification.

## Evidence

Board results are records in `docs/evidence/application.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-application-20261101.md).

<!-- evidence-table:begin -->
1 records in `docs/evidence/application.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793579984604886693-7d81d397` | 2026-11-02 | 3-1 | `5bd520a766cf` | failure-return | passed | run_total 170.794 s, secondaries_off 7, rail_return 2.09 s |
<!-- evidence-table:end -->
