# GPU compute support

The generic compute queue is implemented and host-tested. Physical qualification
of the current OS and application images is pending on port 3-3.

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
| Firmware start and stop | Awaiting the current queued image. |
| One and eight queue jobs | Awaiting the current queued images. |
| Ten open/close cycles with sharing | Host checks pass; physical run pending. |
| Timeout recovery preserving a queued peer | Host checks pass; physical run pending. |
| Mimus recognition with exhaustive GPU sum checks | Shader, readout and program host checks pass; physical run pending. |

Bringup supplies the reviewed CPU clock close correction. The shared daemon needs the reviewed generic GPU capture and bounded
binary-expectation changes in its bringup checkout before these runs. The
required commits are `2e679bc9`, `2110837c`, `a00b8341` and `9d7aa701`. Capture, malformed-result, CPU adapter and binary-field host checks
pass. Test evidence is under
`~/.cache/sunstoneos/build/gpu-integrate-1031/qualified/`.

The coordinator's CPU performance measurement does not qualify GPU support or
establish that offloading improves recognition speed. GPU performance selection
is a separate task. The queue serves applications through the
[application kit](../designs/app-abi.md); the OS does not depend on mimus.
