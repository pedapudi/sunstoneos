# GPU operating-point changes in the OS

Status: **implemented** in `drivers/gpu/a733_gpu_performance.c`, `drivers/gpu/pvr_gpu_service.c` and `apps/os/perf.c`, and **host-tested**. Physical live GPU frequency changes, thermal drain and recovery, and heterogeneous performance are **open**; this design does not authorize hardware execution or establish physical DVFS timing.

The GPU joins the OS performance command and the thermal owner. The default point is 1008 MHz at 960 mV, and the optional low point is 600 MHz at 800 mV, both from the vendor `vfdefault` column (`A733_GPU_CORE_HZ`, `A733_GPU_SUPPLY_UV`, `A733_GPU_LOW_HZ`, `A733_GPU_LOW_UV` in `include/a733_gpu_opp.h`). CPU, GPU and NPU keep their own device state and use the shared providers. The GPU stays firmware-driven during a normal change, and a software timeout neither establishes GPU idle nor permits buffer reuse.

## Effective reference and required behavior

The reference kernel is `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Its BXM build defines `SUPPORT_LINUX_DVFS`, and `sysconfig.c:356–369` selects `SetFrequency`, `SetVoltage` and `bIdleReq = IMG_TRUE`. The active [devfreq target](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/modules/gpu/img-bxm/linux/rogue_km/services/server/env/linux/pvr_dvfs_device.c#L97) has SHA-256 `1f32114c38a92aa8c14de9f7e63a83e601911c9938a75f65b803b44bda4fb420` and is cached under `~/.cache/sunstoneos/sources/gpu-performance-2ac08e8c/`, pinned in `sources.lock.json` with the other GPU references. It is MIT OR GPL-2.0-only and runs only as a host reference. The alternate `SUPPORT_PDVFS` implementation is corroboration. The operating points and source selection are in the [performance design](performance-modes.md).

| Effective reference | Required native behavior |
| --- | --- |
| `pvr_dvfs_device.c:97–181`; `power.c:1313–1420` | Keep one power owner across acknowledged idle, voltage and rate change, firmware notification and idle cancellation |
| `rgxpower.c:1519–1670` | Clear the power sync, send FORCE_IDLE, require a fresh acknowledgment and FORCED_IDLE state; CANCEL_FORCED_IDLE also needs a fresh acknowledgment |
| `sunxi_platform.c:405–485`; `ccu-sun60iw2.c:1020–1043` | Above 800 MHz use PLL_GPU0; 600 MHz uses PERI0's 600 MHz output, selector 2 and divider 1; keep PLL_GPU0 while using PERI0 |
| `rgxpower.c:1160–1215`; `rgxtimecorr.c:275–338` | Publish the runtime clock word and a new time-correlation record before CORECLKSPEEDCHANGE and idle cancellation |
| `axp2101-regulator.c:383–408`; `sunxiSetVoltage` | After a 160 mV change wait at least 1200 µs before the next dependent operation |

The native `pvr_rogue_fwif.h` is the command and layout authority: POW is 107 and CORECLKSPEEDCHANGE is 204; the DDK's command IDs differ and supply only the transition order. The runtime configuration places `core_clock_speed` at byte 12. The time-correlation ABI is 256 records of 40 bytes at the start of the utilization allocation followed by the sequence word. Each record holds host nanoseconds, monotonic nanoseconds, the GPU CR timer, the conversion factor `(256000000 << 20) / (rate_hz / 1000)`, the rate and a reserved word. The 48-bit CR timer at GPU `+0x160` is read next to the architectural counter sample, and as in `RGXReadHWTimerReg` bit 31 is compared with bit 63 and reread once on rollover, with an inconsistent second observation failing. The complete record is published before its sequence with the non-cacheable shared-memory ordering. The two programmed rates need no calibration history, and register decoding supports no measured-frequency claim.

## One owner from admission through completion

- Performance-enabled GPU images reserve DCDC4 exclusively under the GPU firmware owner before first power-on, through an explicit DCDC4 mode of the GPU supply consumer; fixed-rate images keep the shared consumer. The exclusive token is acquired after the power-domain owner's initial supply-verification pass for DCDC4 has published success, which needs a shared DCDC2/DCDC4 token, and after that token is released. A failed verification refuses GPU startup.
- The GPU platform is the only DCDC4 writer. It keeps the token through healthy OFF and reopen and through faults. A completed sample stays borrowed until the next serialized sample or write, uncertain provider work is never reaped, and a published voltage changes only after write readback, settling and a fresh snapshot. NPU DCDC2 and CPU DCDC3 and DCDC5 are disjoint. CPU-only performance readback obtains the GPU voltage through this owner once it owns DCDC4 exclusively, because another shared DCDC4 token would deadlock. The PMIC DCDC4 write predicate admits the high-row voltages and the 800 mV low point. The resource manager's initial 960 mV acquisition proof stays, and low-mode reopen first establishes the high startup supply.
- The GPU holds a stable PERI0 clock lease for the performance-enabled lifetime under its identity before it selects the low mode, and verifies the PLL_REF ancestry, the 1200 MHz PERI0 2X output and the 600 MHz branch's software and automatic gates (CCU `+0x1908`, status `+0x1988`, bits 11 and 9). It never reprograms PERI0, which other devices use.
- The thermal task is the performance-policy owner and is bound to a second GPU service client before the scheduler starts (thermal starts before `gpu-linear` in `os_applications`). Its nonblocking actions use the request and reply queues with an immutable action, value, epoch and absolute deadline. The GPU worker alone drives firmware, clock and supply changes. Replies distinguish accepted from completed work, and a late reply cannot turn an expired operation into success.

## Bounded live transition

1. **HOLD.** HOLD closes new Mimus submission with an atomic slot reservation. A producer that already reserved or submitted its slot completes under its deadline, and the worker keeps processing interrupts, events and completions until that job ends and then owns the empty slot. A recognizer that meets the hold waits within its bounded caller operation and does not fall back to the CPU. Previously lent sums stay mapped and unchanged while CPU readout workers finish.
2. **Idle.** With admission closed and no job outstanding, the worker clears the power sync, sends FORCE_IDLE through the kernel CCB and requires fresh sync 1 plus the firmware FORCED_IDLE state. The interrupt handler only masks and wakes and never programs clocks. HOLD completes only after this acknowledgment, and a timeout after publication is a retained failure, not a clean busy refusal. HOLD ownership is recorded when FORCE_IDLE is published, even before HOLD returns DONE, and engine failure accounting preserves it.
3. **Rate and voltage.** A downshift selects PERI0 600 MHz under the register lock, requires the update bit to clear, decodes the live clock, then compares and writes DCDC4 to 800 mV. An upshift compares and writes 960 mV, settles and snapshots, then selects the enabled, verified 1008 MHz PLL_GPU0. Held ownership and clock state are revalidated before lowering voltage, and every PMIC submission, reap, register operation and wait uses its captured deadline.
4. **RELEASE.** RELEASE writes the actual rate into the runtime configuration, publishes a new time-correlation record, submits CORECLKSPEEDCHANGE, then clears the sync and submits CANCEL_FORCED_IDLE. A fresh cancellation acknowledgment establishes resume, as in `RGXCancelForcedIdleRequest`, with no ON-only firmware-state predicate, and ordered CCB completion settles the clock notification. Only then does the Mimus slot reopen. A failed or late resume retains the slot, GPU backing, command identities, leases and the failed service, with no reset, rollback or CPU fallback.

HOLD keeps the shared 1.5 s accelerator bound. Each following action keeps the 50 ms accelerator bound, capped by the board session and never renewed while polling. A GPU transaction also captures an outer bound over HOLD and its voltage, rate and resume actions, so a missing next action cannot leave a silently live hold. The two-second caller bound is not extended, PMIC settling is 1200 µs inside the captured action bound, and timer regression and equality at the deadline fail.

The performance engine waits for asynchronous RELEASE and clears a held bit only on DONE. A FAILED action stops the plan, preserves held ownership and publishes STOPPED before the thermal task fails the board. A clean refusal before a command is published may reopen the reserved slot. A thermal change between successful actions stops further voltage and rate changes. An acknowledged resume at the known safe rate is still required before a healthy CLOSE: a raised rail at the old low rate or a lowered rate at the old high voltage is safe to resume and is reported as mixed. Uncertain clock, rail or firmware state never reaches this cleanup.

## Startup, thermal parking and admission

The boot performance request selects CPU domains. The GPU's point is initialized from the same boot-mode option (HIGH by default, LOW when selected) by its stopped-firmware startup path, so the CPU-ready wait never depends on GPU OPEN. Runtime GPU switches need a healthy READY session. Thermal WARM parks the GPU through the acknowledged CLOSE, and critical or fatal handling retains incomplete work. The recognizer's CLOSE waits for an accepted performance action to resolve under its own bound, and a new performance action cannot race an accepted CLOSE.

Low mode survives healthy thermal OFF and reopen: OFF can gate the current 600 MHz source and then normalize the stopped mux under retained ownership. The high-point power-on sequence may run while firmware is stopped, with 960 mV first. For a selected low mode the sequence switches and settles to 600 MHz and 800 mV before firmware starts, and the clock is serialized into the new firmware state first, so firmware configured for 600 MHz never starts with hardware at 1008 MHz.

The supported compositions are the PERF, CPU_CLOCK, GPU and A76 options with USB operation 8, including the recorded two and eight workers and NPU, with session sizes and the TOP, B1, frozen GPU evidence and cold-boot authority unchanged. Resident GPU images keep fixed-rate support and the PERF refusal pending cold performance admission. The link carries GPU performance capability and point metadata that host preparation and frozen verification compare with the selected features. The exact-image GPU entry requires explicit runtime authority for both 600 and 1008 MHz and the exclusive DCDC4 lease at 800 and 960 mV. Fixed-rate entry evidence alone therefore does not admit live changes, and an old image cannot gain performance authority by changing manifest booleans. The performance tool reports actual readback and terminal refusal.

## Cost

A requested transition adds three firmware commands, one 40-byte correlation record with its sequence and runtime word, one clock-mux update, one PMIC compare and write, a final snapshot (the engine's final GPU readback takes another) and the bounded drain. Normal submission keeps its single slot compare-and-exchange; the HELD return branch adds no atomic on success. Each READY advance adds two `performance_controlling` checks, one no-ticket branch in `performance_step` and one atomic release store to the readiness word. No device access occurs for an idle performance owner, and no workload buffer is copied.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Focused cases: firmware controller, PMIC and platform providers, GPU service, OS bridge | 9, 10, 5, 6 | `gpu-performance-software` |
| Engine, CPU-owner and CPU/NPU-owner cases | 33, 21, 22 | `gpu-performance-software` |
| Host preparation and frozen-entry outcomes | 17 | `gpu-performance-software` |
| Executable growth against the eight-worker CPU/GPU/NPU image without PERF | 28,480 B (GPU-specific 13,168 B); 734,601 B left in the 2 MiB CODE region | `gpu-performance-software` |
| GPU service size | 53,544 to 54,040 B; stack headroom 28,384 of 32,768 B | `gpu-performance-software` |
| Modeled HOLD, RELEASE | 3,506 µs, 4,733 µs (modeled platform callbacks, not hardware latency) | `gpu-performance-software` |

Records: [gpu-performance.jsonl](../evidence/gpu-performance.jsonl). The fixtures are software; the service fixture uses synthetic data for one admitted job and does not replay recognition. Actual PMIC ordering and settling are checked through the real provider state machines.

## Limitations and open work

- Open: measure live GPU 600 and 1008 MHz changes, FORCE_IDLE acknowledgment latency and thermal drain and recovery on a board.
- Open: resident performance admission and lifecycle, pending cold performance qualification.
- The 4 MiB heap, two 2 MiB GPU pools, 8 MiB NPU pool and fixed inference regions keep their bounds, and no task, stack, heap allocation or workload-buffer copy is added.
