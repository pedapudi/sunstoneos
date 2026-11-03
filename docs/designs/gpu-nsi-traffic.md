# Experimental GPU traffic measurement through NSI

Status: **host-tested**. The optional diagnostic (`drivers/gpu/a733_gpu_nsi_pmu.c`, `include/a733_gpu_nsi_pmu.h`, `apps/os/gpu-timing.c`) is implemented behind `SUNSTONE_GPU_NSI_PMU`. Board calibration is **open**: no physical traffic measurement is claimed, and the counters' update semantics must be established before their values are reported as per-job traffic.

The diagnostic reads hardware traffic counters for the GPU's master port in the A733 Network System Interconnect (NSI). The reader knows memory-mapped I/O and GPU job queues. The counter boundary is the GPU master port after the GPU caches. It includes firmware, translation and command traffic that uses that port, does not distinguish shader loads from those accesses, and does not measure DRAM pin transactions.

## Register identities and units

The pinned A733 device tree assigns GPU master ID 6, NSI base `0x02020000` and 16 bytes per input-agent data-counter increment, and its `master_clks` property assigns GPU master 6 to the NSI clock. The vendor bandwidth reader multiplies the read and write data counts by that unit. These facts come from the A733 configuration, not from another member of the SoC family.

| GPU master register | Offset from NSI base | Used access |
| --- | ---: | --- |
| Counter enable | `0x0cc0` | 32-bit read and write, bit 0 |
| Counter clear | `0x0cc4` | 32-bit read and write, bit 0 |
| Measurement period | `0x0cc8` | 32-bit read and write |
| Read data count | `0x0cd4` | 32-bit read |
| Write data count | `0x0cd8` | 32-bit read |

The identities follow `sunxi-nsi.h` lines 377–385 evaluated at master 6, and the mapping and units are in `sun60iw2p1.dtsi` lines 1470–1517. The vendor period helper programs cycles as microseconds times the selected master's clock rate divided by one million, then disables, clears and enables the counters and waits 10 µs. That delay is the vendor's sequence and does not establish a documented clear-completion latency. Pinned URLs, SHA-256 values and licenses are in [the source record](../vendor-firmware/gpu-nsi-sources.json).

## Ownership and bounded access

`SUNSTONE_GPU_NSI_PMU=ON` requires `SUNSTONE_GPU_TIMING=ON`, both are off by default, and the PMU source and callbacks are omitted when its option is off. Normal submission code is unchanged.

- The diagnostic runs on the GPU worker while that worker owns the active GPU service. It accesses only master 6's counter controls and reads clock configuration, using 32-bit MMIO on a checked Device mapping with `DSB SY` before and after. It writes no clock, reset, priority, bandwidth-limit or other master's register.
- An enabled inherited counter causes a refusal with no writes. For a disabled counter the diagnostic saves enable, period and data values and takes exclusive software ownership of the bank until cleanup. This assumes that the image contains no second NSI PMU writer. Initial data values are reported because clearing cannot preserve them.
- The period is `UINT32_MAX` cycles. Clock decoding requires a stable 24 MHz PLL reference and an enabled, non-reset NSI clock with no dynamic scaling or pending update. Integer SYS24, PERI0 and DEPLL3X sources are supported; DDRPLL and spread-spectrum DE configurations produce an unsupported-clock record without counter writes. The configured rate must be at most 700 MHz, which keeps the maximum period above six seconds. The fields follow A733 manual sections 4.1.6.61 and 4.1.6.74 and the clock driver's NSI and DEPLL definitions.
- The pinned boot0 selects NSI mux 3 (PERI0_600M) with divider zero, and its DRAM clock routine selects the same parent and keeps that divider; the DRAM-rate branch changes an MBUS divider before rejoining the common NSI write ([binary facts](../vendor-firmware/data/gpu-nsi-clock.json)). This supports the parent decoder without assuming an NSI rate from the DRAM rate, and live clock readback controls admission.
- The measurement must finish before the programmed period expires. End-of-job validation uses the decoded clock rate and the architectural counter. Decreasing raw counts are classified as nonmonotonic, and the diagnostic assumes no rollover width.
- Cleanup disables counting and restores the saved disabled enable word and period with readback. A cleanup error invalidates the diagnostic and blocks later counter writes in that boot without failing the GPU job, so optional telemetry never interferes with GPU retirement or ROM return.

## Observations

Each job reports inherited counts, counts after the initial 10 µs idle interval, after publication, at observed completion, immediately after disabling, and after a further disabled 10 µs interval. The record layout is `struct a733_gpu_nsi_record` in [a733_gpu_nsi_pmu.h](../../include/a733_gpu_nsi_pmu.h), carried in the timing record of `include/gpu_queue_timing.h`. Status `A733_NSI_SAMPLED` means the software sequence completed and does not certify the hardware's counting semantics. Saved clock words permit independent rate decoding, and the parent chain, NSI gate and clock word must agree at both ends of the interval. A clear word that still has bit 0 set after the vendor delay keeps status `A733_NSI_CLEAR_PENDING` while all six phases are collected; the vendor does not poll that bit, so it is not treated as an acknowledgment.

If live counting is established, an eligible interval's traffic is 16 times its read and write counter difference, in 64-bit arithmetic, reported with the sampling boundary, the idle-control counts and the measurement overhead. A decrease, expired period, changing clock, enabled inherited owner or failed readback makes the interval ineligible. Raw observations remain evidence even when the hypotheses fail.

The host fixture models immediate clear and live counting as explicit hypotheses and verifies the software sequence, counter refusal, restoration, configured-rate decoding and invalid-sample handling only.

## GPU firmware performance counters

The firmware interface exposes HWPerf commands and shared-memory addresses, and the native initializer leaves performance collection off. The selected command IDs are 201, 203, 217, 218 and 220; the cached proprietary DDK uses 300–304 for related operations, so its numbers cannot be copied into the native command stream. The pinned HWPerf headers describe programmable groups and the block table describes which register blocks can exist, but they identify no proven BXM-4-64 DRAM-byte or cache-miss event selector. The generic register headers name SLC read and write stall counters, which cannot establish transferred bytes. A missing event definition and native collection path is the gap; it is not evidence that the GPU lacks counters.

## Open work

1. Run one board calibration that pairs an idle control with known GPU work under a period that cannot expire during either interval. It tests five hypotheses, recorded as the prediction `gpu-nsi-traffic-calibration` in [gpu-nsi-traffic.jsonl](../evidence/gpu-nsi-traffic.jsonl):
   - the read and write data registers expose useful partial-period values before the period expires;
   - clearing and the vendor's settle interval give a reproducible start;
   - counts increase with known device traffic and exceed the idle control;
   - disabling either preserves the live count or exposes a consistent partial count, and the two disabled observations agree after settling;
   - enable and period readbacks restore the inherited disabled configuration, and the configured clocks stay stable.
2. Find a BXM-4-64 event selector for DRAM bytes or cache misses and a native HWPerf collection path.
