# Complete a retained GPU return claim and admit stopped thermal state

Status: **host-tested**. The compound continuation with a GPU return is implemented in the mimus checkout (`sunstoneos/apps/os/cpu-return-followup.py`, functions `authority` and `gpu_returned`); the OS supplies the GPU retirement decoder `apps/os/gpu-return.py` and `a733_thermal_stop`. The frozen GPU returning ELF has SHA-256 `6d28a8340c08ba4a3d07eb46a6cd578cb8b8a72f9102c43052e31690f8c89c66` and is unchanged. No physical recognition or return is claimed.

## Binding the GPU return to the compound continuation

The compound host accepts only a completed retained CPU return, its exact successor ROM and one preapproved claim. It decodes CPU retirement and, for a reviewed GPU profile, GPU retirement independently. The continuation supplies transcript parity, cache and survey helpers, strict CPU rail-helper capture and the final proof under that claim. It retires the helper's private 18 MiB instead of claiming those bytes were restored.

A compound review gains one explicit `gpu_return` member. It binds the linked GPU configuration, the frozen normal-return review hash and the fixed future capture path `gpu-retirement.bin`. The linked configuration carries the 64-byte report address and the write-back and uncached pool bounds. The GPU-aware CPU host validator must accept the frozen bundle and its GPU retirement review before the continuation accepts the opt-in. CPU-only reviews keep their admission and receipt checks, and a CPU-only profile rejects `gpu_return`.

Before any transport request the continuation reads the copied GPU record, checks its SHA-256 against the retained receipt, and calls the GPU decoder with the exact epoch and generation. The decoded GPU result must match the receipt's GPU summary alongside the decoded CPU report. A valid record follows acknowledged firmware OFF, retired queues, reset and isolation, GPU resource-token release, completed PMIC sample retirement and a suspended GPU worker in the reviewed native producer. Common-context cleanup command 106 is not part of that producer. Backing stays owned through OFF and the returning host's RAM restoration.

The local write-back and uncached pools must match the linked configuration and lie within the restored reservation `0x41000000` through `0x42200000`, and the original return's backup extents must cover them. The CPU rail helper then overwrites that private reservation under its unchanged strict return and supply and readback checks. A successful continuation records the GPU proof. It grants no unrelated device and does not claim that DCDC4 was restored from 960 mV.

## Stopped thermal state at the next entry

`a733_thermal_stop` (`drivers/thermal/a733_thermal.c`) disables all five sensors and the local alarm and critical interrupt enables, reads them back, and leaves the GPADC clock and THS gate and reset enabled. The CPU return path masks INTID 71 and observes the STOPPED result before it publishes completion, and its terminal restores the saved GIC fields. Analog conversion may finish after sensor disable, but no DMA path exists and local interrupts cannot write memory.

The next session uses the enabled-clock mode. The cold-clock mode (`clocks(p, 0)`) requires the GPADC enable bit clear when `configure_adc_clock=1`, so it does not apply. With that session field zero, the driver admits an already enabled 24 MHz GPADC source and divider. It checks that THS sensor, shutdown and interrupt enables are all zero, programs calibration and settings, clears stale pending status and enables the sensors last. Its masked updates do not pulse reset when the gate and reset bits are set. `apps/os/thermal.c:construct` already acquires shared stable GPADC ownership and exclusive THS ownership for this mode, so the next GPU image needs no rebuild.

The CPU rail helper (`apps/board-bundles-power/cpu-opp-step.c:thermal_start`) writes the thermal block between the CPU return and the next entry. It reads the GPADC enable bit and selects enabled-clock mode when set, acquires its own THS and GPADC lease, runs the reviewed CPU rail plan and calls `thermal_stop` before COMPLETE. Its alarm and critical masks are zero, and acquisition, period, filter and SID calibration match the OS driver. The last stopped predecessor is therefore the helper. A new owner reconstructs its own driver and lease ledger and inherits register values only.

The stopped-predecessor packet accompanies the thermal entry evidence. A root preparing the next entry fills and binds:

- the original CPU-return receipt and the helper's strict kernel receipt, with hashes, board and port, original and returned ROM identities and the retained claim (RAM-continuity validation uses these receipts, not the compound summary);
- the exact helper image, binary, manifest and source closure, its COMPLETE, boot clock, DCDC3 and DCDC5 readbacks and its final thermal STOPPED call, with the private reservation retired and no preserved-byte claim;
- the source closure of every executable between the helper's STOP and the next entry, including ROM and SRAM helpers, excluding any other THS, GPADC, clock or interrupt writer;
- the next frozen GPU image, session identities, thermal calibration and configuration, `configure_adc_clock=0` and stable enabled GPADC ownership, where no inherited service means no surviving active owner with retirement evidence;
- the observed elapsed time from the helper's validated return to the next sensor enable, if relied upon. Existing helper receipts record no monotonic stop timestamp.

The driver rechecks the oscillator, reference, APB and GPADC source and divider, requires all sensor, data, critical and alarm enables zero before programming, and verifies enabled THS clocks after startup. Its masked gate and reset updates perform no CCU writes on the reviewed retained state, but it can enable a missing THS gate or reset bit during startup, so the packet must not describe a pre-start refusal of every altered THS clock word.

Stopping does not prove analog drain: a conversion may finish after disable, and old pending flags or filter history may remain. `startup` recalibrates from the same SID words, rewrites the identical acquisition, period and filter settings and clears all four pending registers (write 1 to clear) before enabling sensors. The native entry guard requires valid, fresh NORMAL observations before CPU performance or GPU recognition. Conversions carry no generation tag, and physical warm adoption stays unqualified. The manual's timings are in the record `gpu-compound-followup-thermal-timing`.

The source packet is saved as `stopped-predecessor.json` beside the focused check results. Its source hashes and manual sections are concrete, and its physical receipt, successor and elapsed-time bindings are absent, so it cannot serve as a runnable entry authority.

## Cost

There is no native code change, new scheduler task, reset or bus transaction. GPU continuation adds one 64-byte record read, hash and decode per readiness check and bounded comparisons against the loaded geometry and review. Backup coverage checks use the small extent list and allocate no device memory.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Focused host cases with the actual GPU-aware bundle validator and native producer records | 20 pass | `gpu-compound-followup-software` |
| Unsafe thermal predecessor variants refused before writes | 9 | `gpu-compound-followup-software` |

Records: [gpu-compound-followup.jsonl](../evidence/gpu-compound-followup.jsonl).

## Open work

1. Run the compound continuation on a board with a retained GPU return, a receipt and a real clock authority; the cases here use nominal modeled clock authority.
2. Measure the stop-to-start elapsed time and any analog drain or filter flush after `a733_thermal_stop`, and fill the packet's receipt, successor and elapsed-time bindings.
