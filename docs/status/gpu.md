# GPU

**Status: implemented.**

Native firmware loading, power/interrupt ownership,
compute, rendering and application bindings have software evidence.
Complete recognition and general recovery remain unqualified.

**Board-qualified scope:** Standalone firmware start, health, stop and restart,
eight compute jobs and two production exact-sum shapes pass at 1008 MHz/960 mV.
The shapes use K=512 and K=516, each with 33 rows and 17 channels. All 1,122
sums, tags and guards match; context cleanup, firmware OFF, isolation, provider
parking and normal ROM return pass for those frozen inputs.

Evidence paths are relative to `~/.cache/sunstoneos/evidence/`:

- `board-session-20261025-20/port-3-1/` and
  `board-session-20261025-22/port-3-1/` record firmware stop/restart.
- `board-session-20261025-29/port-3-1/` records eight jobs.
- `sol-resume-20261028/port-3-1/gpu-exact-sum/` binds the exact-sum inputs
  and healthy retirement.
- `sol-resume-20261028/port-3-1/gpu-cache-permission/` tests the CPU-only
  read-only-output cache helper; it does not establish general GPU visibility.

The reference firmware image at `fed5813f` starts and stops firmware with zero
jobs and returns the reference transcript on 3-1. Its evidence is in
`dev-runs-1031/3-1-gpu-return-210941/`, with RETURN_ROM and watchdog mode 0.
This result does not qualify the later combined queue image.

The generic compute queue is implemented with one retained program and memory
map, up to four buffer leases and two trusted application slots. One job executes
at a time. Completion uses interrupts with a 4 ms backup poll. Shared mutable
mappings bypass the GPU system cache; data pages are at most 256 KiB.
Startup uses executed-command health, vendor secure writes, initial timer
correlation and MMU-cache completion before READY.

Healthy close powers off while retaining memory for reopen. Timeout/reset
recovery requires idle checks, reset readback, power-off and firmware-memory
reclaim before queue resumption. Failed isolation retains buffers and owed
records. Final completion quiesces both callers before CPU return or resident
suspension. The primary application owns lifecycle; the second has independent
manifest admission, submission sequence and completion queue.

Host checks cover 204 exact-sum jobs, ten shared open/close cycles, queued-peer
preservation during reset recovery, native reset access policy and final
quiescence. The mimus shader/readout checks preserve exhaustive sum comparison.
These modeled results add no physical queue qualification. The OS uses the
[application kit](../designs/app-abi.md) and does not depend on mimus.

The texture fairness fixture `checks/gpu-texture-frame.c` references the removed
`PVR_GPU_SERVICE_BURST_STEPS` scheduler limit. The GPU lifecycle maintainer owns
its repair: exercise the service's actual 200 microsecond timed bursts and
1 millisecond CPU scheduling interval, including the submitting-caller exemption.
The fixture's compile failure leaves that fairness check unresolved.

GPU capture supports image-bound GPU/thermal receipts and bounded binary
expectations. The shared daemon needs the reviewed capture changes from
`2e679bc9`, `2110837c`, `a00b8341` and `9d7aa701`. Capture, malformed-result,
CPU-adapter and binary-field checks pass under
`~/.cache/sunstoneos/build/gpu-integrate-1031/qualified/`.
Frozen lifecycle proposals are under `images-lifecycle/` in that build root.
Their CPU clock-close defect requires rebuild with the merged correction before
qualification. The [lifecycle guide](../../apps/gpu-lifecycle/README.md) and
[queue design](../designs/gpu-compute-queue.md) define the bounded operations.

Remaining work includes current queue firmware start/stop, one/eight jobs,
ten physical open/close cycles, queued-peer timeout recovery, complete GPU
recognition, graphics, thermal transitions and representative concurrency.
CPU performance evidence does not establish GPU speedup. Frozen earlier failed
inputs and fault origins remain in [the hardware worklog](../hardware-worklog-20261031.md).
See [backend](../gpu-backend.md), [power](../gpu-power.md),
[rendering](../gpu-render.md) and [compiled programs](../gpu-compiled-programs.md).
