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

The complete recognition attempt in `sol-gpu-completion-1031/recognition-return/`
produces no native result. The frozen firmware diagnostic from the unmerged GPU-reference branch in
`dev-runs-1031/3-1-gpu-return-204909/run/watchdog-report.json` stops in its
compatibility callback at `gpu_backend.c:375`, caller `0x41088a50`, detail
`0x04040201`. Its source location describes that frozen image rather than the merged backend.
The callback stop does not establish an ABI mismatch or workload fault.

The [reference comparison](../reference-backsolve-gpu.md) identifies missing
executed-count health binding, secure-setup writes, initial timer correlation,
pre-job MMUCACHE invocation and device-SLC ownership handling. Production uses
2 MiB workload pages despite the selected DDK's BRN 71317 restriction.
These source differences have inferred consequences; no recorded failure is
attributed to them without further evidence.

Trusted programs, graphics profiles, cleanup/disposal/reopen and GPU/NPU
composition have modeled evidence. `gpu-compute-lifecycle-20261020/root-integration.json`
does not prove physical reuse. The generic compute queue has design/prototype
headers in the merged base; its implementation is not merged.

Remaining work includes current service startup, complete recognition, graphics,
physical failure recovery, buffer reclamation/reopen, cache visibility,
thermal transitions and representative concurrent workloads.
See [backend](../gpu-backend.md), [power](../gpu-power.md),
[rendering](../gpu-render.md), [compiled programs](../gpu-compiled-programs.md)
and [the application adapter](../gpu-mimus.md).
