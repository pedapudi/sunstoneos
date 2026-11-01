# GPU

**Status: implemented; current compute queue qualification is incomplete.**

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

The texture frame fixture measures native render-service work for three scenes,
with two frames each. Its timed probes compile the actual service scheduling
code with modeled time and task readiness. Burst, CPU-run and submission
exemption mutations fail. Work counters exclude the scheduler itself. Extended
texture and batch fixtures prepare their pools once; native translation checks
reject incorrect final write-back and uncached blocks before GPU construction.
Host evidence is under
`~/.cache/sunstoneos/build/gpu-integrate-1031/texture-prepare-1101/`.

GPU capture supports image-bound GPU/thermal receipts and bounded binary
expectations in the active shared runner. The current port 3-3 sequence begins
with a rebuilt firmware control, then one and eight jobs, shared reopen cycles,
timeout recovery and recognition with exhaustive sum checks.

The firmware control on OS revision `e124d65f` returns normally with zero GPU
jobs and the reference transcript in queue entry `1793516275891220461-b5cf700f`.
The one-job image in entry `1793516370919560948-dd320841` stops at its first
submission with application detail 4. Its diagnostic retains the public result
and job number. Initial thermal parking remains a hypothesis. These entries
are under `~/.cache/sunstoneos/board-queue/entries/`; they do not qualify the
rebuilt images.

Remaining work includes current queue firmware start/stop, one/eight jobs,
ten physical open/close cycles, queued-peer timeout recovery, complete GPU
recognition, graphics, thermal transitions and representative concurrency.
CPU performance evidence does not establish GPU speedup. Frozen earlier failed
inputs and fault origins remain in [the hardware worklog](../hardware-worklog-20261031.md).
See [backend](../gpu-backend.md), [power](../gpu-power.md),
[rendering](../gpu-render.md) and [compiled programs](../gpu-compiled-programs.md).
