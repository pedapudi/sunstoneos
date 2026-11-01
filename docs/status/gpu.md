# GPU

**Status: implemented.**

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

The generic compute queue retains one program and memory map, up to four buffer
leases and two trusted application slots. The native job path submits in the
caller, retains four jobs and collects interrupt-published fences through direct
task notifications. The service owns lifecycle and recovery. Its one-tick safety
watchdog reads CPU fence state and never polls a timeline for successful
completion. Diagnostic configurations without native IRQ admission retain the
one-slot service path and its 4 ms backup poll. Shared mutable
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

## Caller submission and interrupt fences

**Board-qualified scope:** The zero-element vector shader completes 20,064 jobs
per measured input on port 3-3, including 10,000 serial samples and 10,000 jobs
at depth four. The admitted-ring image returns normally with GPU OFF, a parked
worker and seven secondary CPUs OFF. This finite workload does not qualify
the worst-case timing targets, recovery under load or arbitrary programs.

The [design](../designs/gpu-fast-path.md) defines retained payloads, context-bound
fences, four-job batch admission, priorities and the guarded caller-task EL1
gateway. Each batch uses one kernel entry and one firmware KICK per job.
Mappings remain fixed until context retirement. Write-back payloads remain the
default; uncached payloads have no demonstrated tail advantage.

| Entry on port 3-3 | Condition | RTT p50 / p99 / maximum, µs | Submit wall p50 / p99 / maximum, µs | Pipeline jobs/s |
| --- | --- | ---: | ---: | ---: |
| `1793563967717509980-ac170b9a` | Guarded ring, write-back | 104.458 / 121.083 / 175.958 | 13.458 / 24.000 / 75.500 | 9,506.375 |
| `1793564171325300569-9135052d` | Guarded ring, uncached | 104.417 / 120.792 / 335.583 | 13.250 / 22.708 / 76.500 | 9,516.749 |
| `1793567368102223375-02dbc4e4` | Admitted ring, write-back | 103.792 / 115.833 / 247.958 | 3.833 / 11.250 / 51.875 | 9,539.784 |

IRQ C-entry to notification return in the admitted-ring run is
2.292 / 8.167 / 118.000 µs, with all 10,000 samples present. IRQ C-entry to
collection return is 3.417 / 11.333 / 119.208 µs. The first interval excludes
the vector/dispatcher prefix; submission wall time includes preemption and
measurement overhead. These distributions establish neither a five-microsecond
submission CPU bound nor a twenty-microsecond maximum wake bound.

Zero-job controls `1793563824693939001-7c9def1d` and
`1793567279428783460-552e5684` return normally. The guarded-ring image is built
from `16729f62`, SHA-256
`ffa80959528c62ffd25d8167b226cc7e6a64aa88425c8484c17ad4d708a2c229`.
The admitted-ring image is built from `74f8ec3c`, SHA-256
`9cb215e5134ae94348249c6c1b48840281f1a73c3e87ecc843116396b35b4e80`.
Raw evidence lives in each entry's `run/` under
`~/.cache/sunstoneos/board-queue/entries/`. The recorded runner Git revision
identifies `bringup`; the image source revisions above identify this branch.

The eight-job application entry `1793567482542195083-8703e806` returns normally
with eight GPU jobs and healthy retirement, but its runner outcome is
`result-mismatch`. Its 48-byte payload passes `apps/queue-add/host.py`: eight
jobs, 65 elements, checksum 8,840, sequence 8 and times
130, 96, 98, 103, 101, 104, 100 and 91 µs. The frozen expectation contains a
literal JSON byte string because the submission uses the legacy `mode/words`
format. It does not contain the intended bounded binary expectation.
`queue-add-capture-validation.json` in the build directory records the independent
decode. No later GPU run is submitted after this failure.

The recognition baseline on `work/gpu-mimus-1031` uses external mimus revision
`99c700db0`. Entry `1793560651607761780-3cb221f5` on port 3-1 measures 444.437 ms
compute and 339.188 ms aggregate job time across 154 jobs. Timed entry
`1793561022387122671-06be5388` assigns 94.6% of aggregate elapsed job time to
device scheduling and shader execution. Every completion interrupts the blocked
worker before its backup poll expires. These data do not support attributing
the recognition penalty to tick polling. The caller-submission recognition
image has no board result yet.

Host evidence under `~/.cache/sunstoneos/build/gpu-fastpath-1101/` includes
delayed firmware acceptance, execution and IRQ models, direct EL0 calls with
the service suspended, coalesced fences, two clients, ownership conflicts,
out-of-order collection, missing interrupts, retained faults, raw-ring wrap and
backpressure, close/reopen and forced-idle deadline expiry. The native source
and compiled-image guard rejects message hops, delays and mapping updates in
selected hot functions. Timing-disabled builds omit all timestamp fields and
IRQ counter reads. Independent review covers the gateway and queue changes.
The three baseline images, kernel check, eight-core soak, external mimus model
and check index pass. The repaired gate record is
`gate-repaired/gate-gpu-fastpath-repaired/summary.txt`; it pins the external engine
to `99c700db0`. The first gate's OS-only baseline remains valid.

## Next actions and integration boundaries

1. Correct the queue-add submission expectation to schema
   `sunstone-board-expect-u32-v1`, using its actual 12 output words and bounds.
   Check it without USB, then request one new queued entry with the corrected
   expectation. Preserve the failed entry; do not relabel it as runner success.
2. Run the built recognition image with the same model, input and placement as
   the 154-job baseline after queue-add passes. Use the pinned mimus checkout
   in the build directory. Validate reference
   CRCs/text and healthy retirement before comparing recognition time.
3. Explain the roughly 104 µs dispatch interval and the 118 µs notification tail
   before changing timing budgets. Inspect firmware scheduling and measure
   interference separately. The latency worker records long shared kernel-lock
   waits, but those observations do not attribute these GPU tail samples.

Built images and their image READMEs are beneath
`~/.cache/sunstoneos/build/gpu-fastpath-1101/`:

| Image | ELF | README | Board state |
| --- | --- | --- | --- |
| Admitted-ring benchmark | `benchmark-admitted/sunstone-a733-os-gpu-fast.elf` | `benchmark-admitted/os-image/sunstone-a733-os-gpu-fast/README.md` | Control and measured input return |
| Eight vector jobs | `queue-add/sunstone-a733-os-queue-add.elf` | `queue-add/os-image/sunstone-a733-os-queue-add/README.md` | Valid payload; runner expectation mismatch |
| Reference recognition | `recognition-pinned/sunstone-a733-os.elf` | `recognition-pinned/os-image/README.md` | Built, not submitted |

The recognition ELF SHA-256 is
`42920c809e9a6ed624e5455adb796bc7297c5a5526956b88e407823c648b4883`;
the vector ELF SHA-256 is
`93bd14285f913a39085d27ee9431820ba809053f4767df52953a4888693af96f`.
The recognition program comes from the isolated `mimus-source/` worktree at
`99c700db0`, with exhaustive CPU comparison disabled for matched timing.

No approval is pending for the committed job-path changes. Further board runs
wait for the coordinator's next session. New clock/voltage points and storage
writes require coordinator approval. Idle timeout and hysteresis remain with
the lifecycle owner; no such power-policy change is implemented here.

The coordinator lands `work/gpu-fastpath-1101`. Its lifecycle base includes
`c8209f1e` through merge `c0856f1a`. Shared files with the lifecycle worker are
`drivers/gpu/{pvr_gpu_service,pvr_compute_queue,gpu_queue}.c`, their headers,
`platform/a733/gpu_backend.c` and `apps/os/gpu.c`. The mimus timing worker also
touches command/completion instrumentation; this branch uses the separate
`SUNSTONE_GPU_FAST_TIMING` flag. The later CPU-lock evidence remains on
`work/latency-1101`; it is not merged into this branch.
