# Driver completion, CPU cost and idle wakeups

**Status: implemented.**

This audit covers the I/O paths under `drivers/`, `platform/`, `runtime/` and `apps/os/`. Its findings identify loops, service hops, critical regions and copied buffers that need workload-specific review. Source occurrences establish neither execution frequency nor a worst-case bound.

**Software evidence:** host.

**Board-qualified scope:** None.

Physical driver timing and idle power are unmeasured. Host models cover the selected ready-step, notification and deadline changes.

## Capabilities

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| JPEG ready programming in one advancing poll | host-tested | 659 writes in one call | `driver-realtime-audit-ranked-findings` in [driver-realtime-audit.jsonl](../evidence/driver-realtime-audit.jsonl) |
| USB serial and UVC ready-step waits | host-tested | at most 32 ready passes before a precise wait | same record, `driver-realtime-audit-host-checks` |
| Display validation once per prepare, request and replay | host-tested | walks per prepare, start and stop: 13 to 3 | same records |
| Open findings (USB0, SD, DMA, audio, G2D, CE, other families) | open | counted costs, no board measurement | `driver-realtime-audit-ranked-findings`, `driver-realtime-audit-other-families` |
| Latency, CPU cost and idle current on admitted paths | open | none measured | `driver-realtime-audit-board-comparisons` (not run) |

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
10. Replace the remaining tick polling in the USB decoder frame lifetime, I2C, SPI, UART, PCIe/NVMe, network, GPU owner, thermal owner and camera paths. Qualify the [NPU owner's notification and precise-deadline waits](../designs/npu-owner-events.md); the history lists the other locations.
11. Measure baseline and candidate latency, throughput, CPU cost and idle current on admitted device paths, which need the USB, video and display prerequisites in [USB status](usb.md), [video status](video.md) and [display status](display-g2d.md).

## Evidence

Board results with a queue entry are records in `docs/evidence/driver-realtime-audit.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-driver-realtime-audit-20261101.md).

<!-- evidence-table:begin -->
4 records in `docs/evidence/driver-realtime-audit.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `driver-realtime-audit-board-comparisons` | 2026-11-01 |  | `` | not-run | inconclusive |  |
| `driver-realtime-audit-host-checks` | 2026-11-01 |  | `` | not-run | passed | jpeg_completion_delay_a 1 µs, jpeg_completion_delay_b 17 µs, jpeg_completion_delay_c 2000 µs, usb_reset_stale 3 ms, usb_run_stale 2 ms, usb_command_record_delay 4.5 ms, usb_transfer_record_delay 3.5 ms, usb_build_peak_rss 143548 KiB, jpeg_check_peak_rss 112532 KiB, host_available_memory 114 GiB, host_memory_reserve 32 GiB, integration_gate_duration 20.89 s, integration_gate_peak_rss 142584 KiB |
| `driver-realtime-audit-other-families` | 2026-11-01 |  | `` | not-run | inconclusive |  |
| `driver-realtime-audit-ranked-findings` | 2026-11-01 |  | `` | not-run | passed | jpeg_ready_programming_writes 659 writes, jpeg_one_tick_sleep_estimate 659 ms, usb_startup_sleeps_control 19 sleeps, usb_ready_step_sleeps_control 14 sleeps, usb_ready_passes_before_wait 32 passes, usb_retry 50 µs, usb0_active_poll 200 µs, sd_pio_ceiling 512000 bytes/s, display_table_words_per_walk 8192 words, display_cpu_pages_checked 1810 pages, display_translations 3620 translations, display_walks_before 13 walks, display_walks_after 3 walks, g2d_copy_per_tick 32 KiB, g2d_guard_scan_per_chunk 48 KiB, g2d_720p_copy_wakes 113 wakes, g2d_720p_guard_reads 5.3 MiB, ce_aes16_cache_range 7872 bytes, ce_aes16_ratio 492 x, ce_random32_cache_range 2560 bytes, dma_masked_region_target 25 µs, dma_delivery_target 250 µs |
<!-- evidence-table:end -->
