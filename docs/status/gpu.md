# GPU compute support

The generic compute queue is implemented and host-tested. Firmware startup and
shutdown return normally on port 3-3. Compute and lifecycle qualification remain
incomplete because the one-job application stops at its first submission.

The service retains one program and memory map, with up to four buffer leases.
Two admitted applications can share that trusted context through independent
submission slots and completion queues. One job executes at a time. Completion
uses the GPU interrupt with a 4 ms backup poll. Shared mutable mappings bypass
the GPU system cache, and GPU data pages are at most 256 KiB.

Healthy close powers the device off and retains its memory for reopen. A job
timeout or reset can resume the queue only after idle checks, reset readback,
power-off and firmware-memory reclaim. Failed isolation retains buffers and owed
records. Final application completion quiesces both callers before CPU return
or resident suspension. The default GPU operating point is 1008 MHz at 960 mV.

## Qualification

| Operation | Current result |
| --- | --- |
| Firmware start and stop | Pass on port 3-3: zero jobs, GPU off, seven secondaries retired and normal return. |
| One queue job | First submission returns a non-OK result; the application stops and the watchdog resets the board. |
| Eight queue jobs | Not submitted; depends on the one-job result. |
| Ten open/close cycles with sharing | Host checks pass; physical run pending. |
| Timeout recovery preserving a queued peer | Host checks pass; physical run pending. |
| Mimus recognition with exhaustive GPU sum checks | Shader, readout and program host checks pass; physical run pending. |

The firmware evidence is queue entry `1793516275891220461-b5cf700f`; its image
SHA-256 is `07caeef0d3fffdf1f5c5bd4520e48e447d8bbbbaaa733f14afb8bb0654262ed1`.
The failed one-job evidence is entry `1793516370919560948-dd320841`; its image
SHA-256 is `f18415f1c8c9160628717f9a59d4c751c3bd3e3eece30ba050a4192b14906938`.
Both images use OS revision `e124d65f`. Evidence is under
`~/.cache/sunstoneos/board-queue/entries/`, with the native fault and watchdog
classification in each entry's `run/result.json`.

The one-job fault is application detail 4 at `queue_add_main`, before any job is
accepted. The diagnostic application records the submission result and job
number in the retained fault. Host checks verify all five non-OK results.
Initial thermal throttling is a possible cause; the board evidence does not
identify the returned result. The shared queue requires coordinator recovery
after the host reboot before the diagnostic can run. No later image is frozen.

Bringup includes the CPU clock close correction, generic GPU and thermal capture,
and complete binary-result matching. The three baseline builds, both integration
gates and the check index pass. Host evidence is under
`~/.cache/sunstoneos/build/gpu-integrate-1031/qualified/`.

The texture-frame fixture executes the service's current scheduling code with
modeled time and task readiness. Three scheduler probes, three texture scenes
and mutations of the burst limit, CPU scheduling interval and submission
exemption pass. Six service cases and the hello baseline build pass. The separate
texture QEMU fixture fails its extended-pool preparation assertion at
`checks/gpu-texture.c:248`, before the worker starts. Evidence is under
`~/.cache/sunstoneos/build/gpu-integrate-1031/texture-scheduling-1101/`.

The coordinator's CPU performance measurement does not qualify GPU support or
establish that offloading improves recognition speed. GPU performance selection
is a separate task. The queue serves applications through the
[application kit](../designs/app-abi.md); the OS does not depend on mimus.
