# Applications and development execution

**Status: implemented.**

SunstoneOS supplies the FreeRTOS application kit, an OS-only image, the hello-compute control and optional service profiles. Applications own their algorithms and assets in external repositories.

The returning CPU image ends an application failure, a synchronous exception from an application task at EL0, a panic on a core that can schedule, an unexpected interrupt and a failure before the scheduler starts at the boot ROM with the first-failure record, instead of at a watchdog reset. Device failures, exceptions at EL1 or EL3, SError and an unfinished return still end at the watchdog. An NPU image that fails before its service ran publishes a never-started stop report, so the runner restores DRAM. [The failure-return design](../designs/failure-return.md) lists every failure path and its ending. `checks/os-failure-return.py` executes the linked image in each context on the host, `checks/qemu-os-boot.py` takes a real EL0 data abort on the virtual board, and `checks/board-return-preservation.py` covers the runner outcome. One forced application failure on the board returned through this path without a watchdog reset; the evidence table below records it.

**Board-qualified scope:** One eight-worker CPU-only mimus recording matches its
90-frame reference, retires seven secondary cores and restores RAM, boot clocks
and CPU rails. A corrected hello-compute control returns normally with seven
secondaries off. These inputs do not establish general recognition or startup
repeatability. [CPU status](cpu.md) identifies the result and failed controls.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Application kit, manifest and session ([design](../designs/app-abi.md)) | implemented | 552-byte manifest, 4,096-byte session page | `app-abi-host-layout-checks` in [app-abi.jsonl](../evidence/app-abi.jsonl) |
| OS-only, hello-compute and external-mimus builds | host-tested | 12 OS variants, 84 indexed checks passed | `application-host-gate-selection` in [application.jsonl](../evidence/application.jsonl) |
| Failure return to the boot ROM ([design](../designs/failure-return.md)) | qualified on the board | two forced failures returned without a watchdog reset (run totals 170.8 s and 171.2 s) | the two entry records below |
| Eight-worker CPU-only mimus recording | qualified on the board | 90-frame reference matched, seven secondaries off | [CPU status](cpu.md) |
| GPU compute queue and trusted GPU peer | host-tested | none on the board | [GPU status](gpu.md) |
| Concurrent CPU, GPU and NPU operation | host-tested | none on the board | [NPU status](npu.md) |
| Lifetime from the contract magic in one OS image ([design](../designs/one-os-image.md#lifetime-word-and-sealed-session)) | supervised: qualified on the board; resident: host-tested at EL3 and EL1, unqualified on the board | two supervised runs (ret-npu-load: EL3 stage, total 32.04 s; EL1 stage, total 35.08 s); no resident board run | `1793940886685882479-99c7d548` and `1793943016757588735-a94fc5b2` below; `checks/os-lifetime-admission.py`, `checks/os-sealed-session.py`, `checks/board-lifetime-admission.py` |
| Warm PCM-to-transcription time | open | target 0.05 times the audio duration | `application-recognition-time-target` (not run) |

## Open work

1. Run an NPU image that fails before its NPU service starts, to confirm on the board that the never-started stop report keeps the port usable.
2. Run the checks that need external inputs (H.264 capture, SAE); the full gate lists nine remaining checks.
3. Qualify physical queue execution, recovery and application GPU recognition.
4. Qualify simultaneous CPU, GPU and NPU operation; the optional builds have host evidence only.
5. Qualify returning accelerator execution; [NPU status](npu.md) lists the finite images that already pass.
6. Add broader application inputs, native audio input, autonomous card boot, fault recovery and concurrent thermal qualification.
7. Run the resident lifetime on the board, launched over FEL; no resident run boots from the microSD card. Four pieces are missing:
   - the resident contract in the runner's launcher (`tools/board_images.py`): the resident OS magic, no diagnostic deadline, expiry at armed plus 4 s, WDT0 mode `0x41`, configuration 1 and output 2, no FEL flag or redirect, and a resident session;
   - the static-OFF hold of cores 1 to 7 before entry, a power-state procedure that needs the coordinator's approval: under FEL every secondary inherits policy `0x100`, and the resident start refuses any policy but 0 (`HELD_POLICY` in `platform/a733/cpu_start.c`);
   - a test application that stops renewal when its work ends, because the resident OS renews the watchdog after the application finishes (`apps/os/services.c`);
   - the expected watchdog-reset outcome in the runner: DRAM is lost, the stop record is the RTC crumbs (EL3 stop `0x0111`, value `0x5000` for status 5), and the return to FEL relies on the ROM not booting the inserted card, as the recorded supervised resets did.

## Evidence

Board results are records in `docs/evidence/application.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-application-20261101.md).

<!-- evidence-table:begin -->
7 records in `docs/evidence/application.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `application-host-gate-selection` | 2026-11-01 |  | `` | not-run | passed | os_variants_built 12 variants, core_checks 7 checks, indexed_checks_passed 84 checks, indexed_successes_links_full 74 checks, checks_resumed 9 checks, check_index_entries 790 entries, pure_host_checks 89 checks, remaining_external_input_checks 9 checks |
| `1793579984604886693-7d81d397` | 2026-11-02 | 3-1 | `5bd520a766cf` | failure-return | passed | run_total 170.794 s, secondaries_off 7, rail_return 2.09 s |
| `1793582514152160909-ecc5988b` | 2026-11-02 | 3-1 | `1c4c195b3dce` | failure-return | passed | run_total 171.159 s, secondaries_off 7 |
| `application-recognition-time-target` | 2026-11-02 |  | `` | not-run | inconclusive |  |
| `1793940886685882479-99c7d548` | 2026-11-06 | 3-1 | `ec96397e0cd1` | returned | passed | run_total 32.036 s, launch 14.61 s, npu_trigger_to_irq_p50 2907.9 us, npu_trigger_to_irq_max 2913.1 us, jobs 4, mismatches 0 |
| `1793943016757588735-a94fc5b2` | 2026-11-06 | 3-1 | `6d710e0fbc62` | returned | passed | run_total 35.077 s, launch 17.33 s, npu_trigger_to_irq_p50 2906.6 us, npu_trigger_to_irq_max 2915.5 us, jobs 4, mismatches 0 |
| `1793944293922210812-d6f96434` | 2026-11-06 | 3-1 | `d71fd555fb48` | host-error | passed | run_total 32.203 s, run_total 32.202 s, launch 14.694 s, npu_trigger_to_irq_p50 2906.3 us, npu_trigger_to_irq_max 2914.2 us, jobs 4, mismatches 0 |
<!-- evidence-table:end -->
