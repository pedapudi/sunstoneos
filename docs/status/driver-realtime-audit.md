# Driver completion, CPU cost and idle wakeups

**Status: implemented.**

This audit covers the I/O paths under `drivers/`, `platform/`, `runtime/` and `apps/os/`. Its findings identify loops, service hops, critical regions and copied buffers that need workload-specific review. Source occurrences establish neither execution frequency nor a worst-case bound.

**Software evidence:** host.

**Board-qualified scope:** None.

Physical driver timing and idle power are unmeasured. Host models cover the selected ready-step, notification and deadline changes.

## Open work

1. Deliver USB0 device IRQs to the owner and notify application-reply publication. A precise deadline covers protocol and health timers.
2. Add a bounded EL0 privileged serial submission path and more than one outstanding TX per endpoint.
3. Route SMHC command, data and IDMA completion to the SD owner, wake requests directly and batch ready state transitions. Storage writes need separate coordinator review.
4. Admit DMA profiles once, submit from caller context and deliver completion fences, keeping the 25 microsecond masked-region and 250 microsecond DMA delivery targets. Coordinate with the latency work.
5. Notify the audio owner on mailbox publication and DMA period or reply completion, and use a precise stop deadline.
6. Replace the tick-paced display replay and per-MMIO validation checks.
7. Notify G2D readout by task-end IRQ and batch its copy and guard scans; a 720p frame needs 113 copy wakes.
8. Carry authenticated lengths into native CE cache synchronization and use the CE completion IRQ; a 16-byte AES task maintains 7,872 bytes of cache ranges.
9. Integrate the reviewed xHCI close and drain commit `13897638` and the JPEG INTID 81 wakeup commit `3d33b607` after the empty-port runs.
10. Replace the remaining tick polling in the USB decoder frame lifetime, I2C, SPI, UART, PCIe/NVMe, network, GPU owner, NPU owner, thermal owner and camera paths; the history lists each location.
11. Measure baseline and candidate latency, throughput, CPU cost and idle current on admitted device paths, which need the USB, video and display prerequisites in [USB status](usb.md), [video status](video.md) and [display status](display-g2d.md).

## Evidence

Board results with a queue entry are records in `docs/evidence/driver-realtime-audit.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-driver-realtime-audit-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/driver-realtime-audit.jsonl`.
<!-- evidence-table:end -->
