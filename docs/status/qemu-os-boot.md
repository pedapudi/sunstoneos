# OS image on a virtual board in QEMU

**Status: implemented.**

The SunstoneOS image starts on a virtual eight-core board in QEMU and runs through startup to the first application task, in the supervised and the resident lifetime. `checks/qemu-os-boot.py` builds and boots it in about eight seconds without a board. The OS CPU clock owner, the NPU and GPU services, the USB host and the radio are not modeled.

**Software evidence:** emulator.

**Board-qualified scope:** None.

No board was used. The virtual board shares the OS, board application and driver sources with the A733 images and replaces the hardware, so it finds software startup faults and cannot qualify the hardware behind a provider.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Supervised and resident OS start to the first application task | host-tested | about 8 s per `checks/qemu-os-boot.py` run | `qemu-os-boot-cases` in [qemu-os-boot.jsonl](../evidence/qemu-os-boot.jsonl) |
| Thermal, PMIC and resident-supervision faults | host-tested | sensor lost ends the session after 5 s | same record |
| Latency CPU control | host-tested | reaches main entry, ends with failure `0xb03` | same record |
| Applications that need the CPU clock owner, DMA, GPU, NPU, USB host or C++ runtime | open | not modeled | `qemu-os-boot-unmodeled-applications` (not run) |

## Open work

1. Model the CPU clock owner so the latency CPU control passes; it reaches its main entry and ends with failure `0xb03` because the clock owner's record lacks the top operating points.
2. Model the DMA controller and the returning lifetime, which latency-with-DMA, `cpu-npu-load` and `cpu-reliability` need.
3. Model the GPU and NPU services for `queue-add`, `queue-gemm`, `gpu-fast` and `gpu-lifecycle`.
4. Model the USB host, Type-C and class services for `usb-input`, `usb-serial` and `usb-camera`.
5. Model the C++ runtime, speech services and CPU clock owner so the external mimus application runs.

## Evidence

Board results with a queue entry are records in `docs/evidence/qemu-os-boot.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-qemu-os-boot-20261101.md).

<!-- evidence-table:begin -->
2 records in `docs/evidence/qemu-os-boot.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `qemu-os-boot-cases` | 2026-11-01 |  | `` | not-run | passed | check_duration 8 s, zero_deadline_failure_code 1879 code, sensor_late_answer 900 ms, sensor_lost_timeout 5 s, service_fault_report_bound 1 s, resident_watchdog 4 s, transport_stall_after_ready 100 ms, usb_ready_delay 30 ms, stall_status 5 |
| `qemu-os-boot-unmodeled-applications` | 2026-11-01 |  | `` | not-run | inconclusive |  |
<!-- evidence-table:end -->
