# GPU

**Status: implemented; six finite compute-queue workloads pass on port 3-3.**

Native firmware loading, power/interrupt ownership,
compute, rendering and application bindings have software evidence. Finite
queue execution, sharing, restart, injected-timeout recovery and one recorded
recognition input have board evidence. General graphics and thermal recovery
remain unqualified.

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
recovery requires idle checks, ordered reset write/read completion, power-off
and firmware-memory reclaim before queue resumption. Failed isolation retains buffers and owed
records. Final completion quiesces both callers before CPU return or resident
suspension. The primary application owns lifecycle; the second has independent
manifest admission, submission sequence and completion queue.

Host checks cover 204 exact-sum jobs, ten shared open/close cycles, queued-peer
preservation during reset recovery, native reset access policy and final
quiescence. The mimus shader/readout checks preserve exhaustive sum comparison.
These modeled results supplement the finite board cases below. The OS uses the
[application kit](../designs/app-abi.md) and does not depend on mimus.

The texture frame fixture measures native render-service work for three scenes,
with two frames each. Its timed probes compile the actual service scheduling
code with modeled time and task readiness. Burst, CPU-run and submission
exemption mutations fail. Work counters exclude the scheduler itself. Extended
texture and batch fixtures prepare their pools once; native translation checks
reject incorrect final write-back and uncached blocks before GPU construction.
Host evidence is under
`~/.cache/sunstoneos/build/gpu-integrate-1031/texture-prepare-1101/` and
`~/.cache/sunstoneos/build/gpu-integrate-1031/texture-scheduling-1101/`.

GPU capture supports image-bound GPU/thermal receipts and bounded binary
expectations in the shared runner. The following port 3-3 cases return normally.
Each entry is under `~/.cache/sunstoneos/board-queue/entries/`; its
`run/result.json` binds the ELF hash, frozen inputs, native return and receipts.

| Workload | Entry | Observed result |
| --- | --- | --- |
| Firmware without jobs | `1793560443073044909-2795e0f8` | Zero GPU jobs; reference transcript; complete version-2 thermal capture |
| One vector job | `1793558556213820434-b15a959e` | 65 exact elements; checksum 8,840; one completed GPU job |
| Eight vector jobs | `1793558639545392070-81ba2729` | Eight checked outputs; eight completed GPU jobs; 144–178 µs each |
| Ten shared open/close cycles | `1793558732769026225-99c2d8da` | Ten owner jobs, one peer job, ten opens and ten closes |
| Injected timeout recovery | `1793560094069288734-8923c447` | One HANG fault and recovery; queued peer and next owner complete |
| Qualified recorded recognition | `1793560622000131271-53954059` | 186 completed GPU jobs; exact reference CRCs and transcript; complete version-2 thermal capture |

Successful GPU receipts show GPU OFF and a parked worker; CPU return receipts
show all seven secondary CPUs OFF. The recognition ELF compiles
`MIMUS_GPU_QUALIFY_SUMS=1`, comparing every offloaded sum with its CPU reference.
The input produces feature CRC `ac657a3b`, log-probability CRC `a7ade80e`,
90 frames and 57,440 samples. Its text is “there was silence” and its formatted
output is “There was silence.” These results qualify the named finite inputs;
they do not establish sustained load, thermal protection or arbitrary fault recovery.

Additional passing controls are firmware entry `1793558145641638619-38d15f35`
and qualified recognition entry `1793560272415337471-7f3b4f28`. The current
controls include the absolute thermal schedule from `bringup` revision
`f9375cb7`. The thermal merge changes sampling and capture. GPU driver code
and queue job inputs match the earlier passing cases.

Retained failure records distinguish the corrected conditions:

- Entry `1793556885965480682-30338083` reports no accessible new FEL instance
  and no native receipt. The coordinator identified missing udev access;
  the rebuilt firmware control returns after physical recovery.
- Entry `1793558247336076757-d91d1dbf` records application detail `0x412`:
  job 1 receives PARKED before admission. Diagnostics yield and retry for at
  most 500 scheduler ticks while the session remains live.
- Entry `1793558878520344931-0ec4f97b` records a recovery shutdown refusal.
  Entry `1793559451655627386-914e388f` locates its origin at the reset equality
  poll: step 15, timeout, last low word `0xe8000020`. The
  [reset completion contract](../designs/gpu-recovery-reset-readback.md)
  preserves the vendor write and discarded fence read, with mandatory idle
  checks and OFF/reclamation before returning buffer ownership.

Host evidence under `~/.cache/sunstoneos/build/gpu-integrate-1031/` includes
54 vector-add cases and 50 lifecycle cases in `parked-1101/`; 124 shutdown
fault cases, 44 native breadcrumb cases, native backend and vendor comparisons,
five focused QEMU recovery cases and the three baseline builds in
`recovery-1101/`. The merged thermal checks cover eight native cadence cases,
nine decoder cases and the linked return writer. The selected integration gate
and check index pass. These models establish software behavior within their
stated hardware boundaries.

Remaining work includes sustained concurrent workloads, arbitrary stuck-device
recovery, thermal throttling and critical-temperature protection, and graphics.
CPU performance evidence does not establish GPU speedup. Frozen earlier failed
inputs and fault origins remain in [the hardware worklog](../hardware-worklog-20261031.md).
See [backend](../gpu-backend.md), [power](../gpu-power.md),
[rendering](../gpu-render.md) and [compiled programs](../gpu-compiled-programs.md).
