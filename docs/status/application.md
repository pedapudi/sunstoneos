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
| Typed session for an image that grants a device (`tools/session.py`) | host-tested | the GPU mimus image and the GPU lifecycle image are admitted with device bit `0x10`; the NPU and control images grant no device | `checks/session-build.py`; [runner document](../board-runner.md) |
| Concurrent CPU, GPU and NPU operation | host-tested | none on the board | [NPU status](npu.md) |
| Lifetime from the contract magic in one OS image ([design](../designs/one-os-image.md#lifetime-word-and-sealed-session)) | returning: the lifetime of every image except the USB class images, qualified on the board; supervised: host-qualified on its EL3 stop paths, last run on the board before the returning lifetime got its own magic and lifetime word value; resident: one FEL run reaches EL1 supervision and watchdog return; successful completion remains unqualified | four returning runs after that change (ret-npu-load, the NPU encoder, ret-latency, ret-radio-scan), each ending at RETURN_ROM value 0; two supervised runs before it (ret-npu-load: EL3 stage, total 32.04 s; EL1 stage, total 35.08 s); one resident diagnostic retains a USB0 stop failure | `1793948483319321125-4d7fb7af` below and the [NPU](npu.md), [latency](latency.md) and [radio](radio.md) records of 2026-11-06; `1793940886685882479-99c7d548` and `1793943016757588735-a94fc5b2` below; `checks/el3-stop-trace.py` verifies unchanged supervised and returning effects and the resident RTC stop and fault records; `checks/os-lifetime-admission.py`, `checks/os-sealed-session.py`, `checks/board-lifetime-admission.py`; resident run `1794468954726206112-8d88da24` |
| Warm PCM-to-transcription time | open | target 0.05 times the audio duration | `application-recognition-time-target` (not run) |

## Open work

1. Run an NPU image that fails before its NPU service starts, to confirm on the board that the never-started stop report keeps the port usable.
2. Run the checks that need external inputs (H.264 capture, SAE); the full gate lists nine remaining checks.
3. Qualify physical queue execution, recovery and application GPU recognition.
4. Qualify simultaneous CPU, GPU and NPU operation; the optional builds have host evidence only.
5. Qualify returning accelerator execution; [NPU status](npu.md) lists the finite images that already pass.
6. Add broader application inputs, native audio input, autonomous card boot, fault recovery and concurrent thermal qualification.
7. Qualify successful resident completion over FEL after identifying the USB0 provider refusal. The retained record cannot distinguish a PMIC result error from a snapshot mismatch; thirteen linked host cases cover that ambiguity and the resident timing guards. Fresh USB rail settings and FEL controller handoff admission remain unresolved ([failure audit](../evidence/resident-usb0-stop-audit.md), [entry contract](../designs/resident-fel.md), [physical evidence](../evidence/application.jsonl)).
8. Fix the retained failure record of `ss_fail`, then make the gate run `checks/returning-watchdog.py --image`. The check's former call-budget failure was a model defect: `ss_fail` ends in `for (;;) vTaskSuspend(NULL)` and the instruction model has no scheduler to switch away, so the check now stops at the first `vTaskSuspend` and asserts it suspends the calling task. The mode then passes 16 of its cases and fails `application-failure-site`: after `ss_fail` records the application detail, `board_fail` records its own board note, which `board_fail_record_wanted` (`apps/board/fail_record.h`) takes for a device failure after an application failure and the vector stores in place of the application's record, so the retained words name the board note and lose the detail. Once the record keeps the application's detail, register the mode with `GATE_RUNS = [['--out', '{output}', '--image', '{elf:ret-gpu}']]` in the check.
9. Raise the USB host from 24 device records to the 127 that USB addresses allow. The records live in the USB parent lease, which `A733_USB_MEMORY_PARENT_FOR(devices)` in [include/a733_usb_memory.h](../../include/a733_usb_memory.h) computes from the device and endpoint counts. The linked arena ends at `A733_USB_MEMORY_END` (`0x42a00000`) and holds 24 records. 127 records need a lease of `0x904000` bytes and a 14 MiB arena, `0x42200000` to `0x43000000`. The arena is placed by the platform link scripts, `apps/os/usb-memory-link.patch` and `A733_USB_MEMORY_END`, which the memory-layout work owns. Until it reserves that range, a larger `SUNSTONE_USB_HOST_DEVICES` fails a static assertion that names `A733_USB_MEMORY_ARENA_FOR`.
   Moving the records into the unified ledger, sized from the declaration, needs two pieces outside the device owners. The controller reads and writes the records, so they need non-cacheable memory, and the OS image's ledger has no `convert_chunk` hook (`umem_boot_hooks` in `apps/os/large.c` installs only the lock and unlock), so `os_owner_memory` refuses `OS_MEMORY_DEVICE`; adding the hook needs the table builders to remap a converted chunk. Applications also reach the arena through a fixed alias at `0x7a000000`, built by `a733_cached_usb_el1_l3` in `platform/a733/cpu_coherence.S`. Both belong to the table-walk work.
10. Size the USB serial slots (4 per port, `SS_USB_SERIAL_SLOTS` in `drivers/usb/host/serial.c`) from the application's manifest. The slots hold loans of the USB transport's buffers (`acquire` in `drivers/usb/host/serial-native.c`), which are Normal non-cacheable controller buffers in the USB arena, so they move with the device records of item 9. The input event queue is done: its depth is the manifest's `usb.input_events` (a power of two; 1,024 when an application with a keyboard or mouse omits it) and admission takes its storage from the ledger (`os_usb_service_memory` in `apps/os/usb-service.c`).

## Evidence

Board results are records in `docs/evidence/application.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-application-20261101.md).

<!-- evidence-table:begin -->
9 records in `docs/evidence/application.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `application-host-gate-selection` | 2026-11-01 |  | `` | not-run | passed | os_variants_built 12 variants, core_checks 7 checks, indexed_checks_passed 84 checks, indexed_successes_links_full 74 checks, checks_resumed 9 checks, check_index_entries 790 entries, pure_host_checks 89 checks, remaining_external_input_checks 9 checks |
| `1793579984604886693-7d81d397` | 2026-11-02 | 3-1 | `5bd520a766cf` | failure-return | passed | run_total 170.794 s, secondaries_off 7, rail_return 2.09 s |
| `1793582514152160909-ecc5988b` | 2026-11-02 | 3-1 | `1c4c195b3dce` | failure-return | passed | run_total 171.159 s, secondaries_off 7 |
| `application-recognition-time-target` | 2026-11-02 |  | `` | not-run | inconclusive |  |
| `1793940886685882479-99c7d548` | 2026-11-06 | 3-1 | `ec96397e0cd1` | returned | passed | run_total 32.036 s, launch 14.61 s, npu_trigger_to_irq_p50 2907.9 us, npu_trigger_to_irq_max 2913.1 us, jobs 4, mismatches 0 |
| `1793943016757588735-a94fc5b2` | 2026-11-06 | 3-1 | `6d710e0fbc62` | returned | passed | run_total 35.077 s, launch 17.33 s, npu_trigger_to_irq_p50 2906.6 us, npu_trigger_to_irq_max 2915.5 us, jobs 4, mismatches 0 |
| `1793944293922210812-d6f96434` | 2026-11-06 | 3-1 | `d71fd555fb48` | host-error | passed | run_total 32.203 s, run_total 32.202 s, launch 14.694 s, npu_trigger_to_irq_p50 2906.3 us, npu_trigger_to_irq_max 2914.2 us, jobs 4, mismatches 0 |
| `1793948483319321125-4d7fb7af` | 2026-11-06 | 3-1 | `e44044de5574` | returned | passed | run_total 32.4 s, run_total 32.4 s, launch 14.78 s, npu_trigger_to_irq_p50 2906.5 us, npu_trigger_to_irq_max 2913.5 us, jobs 4, mismatches 0 |
| `1794468954726206112-8d88da24` | 2026-11-12 | 3-3 | `28155e9b4b9d` | watchdog-reset | failed | run_total 24.846 s, launch_stage 9.62776 s, rtc_stage 273, rtc_stop_value 24575, rtc_core 0, board_failure_core 1, wdt_mode_before 65, wdt_mode_after 0 |
<!-- evidence-table:end -->
