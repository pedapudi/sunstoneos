# GPU recovery retains memory unless the firmware acknowledges the stop

Status: **design**; not implemented. The design is
[designs/gpu-runtime.md](designs/gpu-runtime.md). This document assumes
familiarity with the [firmware events](gpu-events.md) and the
[shutdown sequence](gpu-stop.md). The failure classes are those of section 1.8
of [performance-architecture.md](performance-architecture.md). Class 1 is refused before any device access.
Class 2 leaves the device in a known state and releases what it can. Class 3
leaves the device state unknown and retains every resource the device can
reach.

## Firmware events either complete a request or start a full reset

The service worker drains the firmware-to-host queue on each GPU interrupt.

| Event | Action |
| --- | --- |
| 103 grow, 104 reconstruct free lists | Existing handlers; each batch checked once against the render session's free-list pool |
| 105 context reset, any reason or flag | Full reset |
| 109 GPU restart request, 112 firmware page-fault notification | Full reset, as the pinned driver answers 109 with `pvr_power_reset` |
| 101, 102 depth-buffer backing | Full reset; depth storage is static, so the firmware does not send them |
| 106 debug dump, 107 statistics, 108 clock rate | Recorded |
| Any other type | Full reset |

A per-session answer to event 105 (fail only the guilty session's jobs when
the reason is a lockup or overrun of one context) is **pending board
evidence**. It is added only after an injected lockup in one session leaves
the other session's jobs completing correctly.

## A full reset releases memory only after an acknowledged stop

A full reset runs `pvr_gpu_stop` from its first step. When the stop reaches
STOPPED through the firmware's forced-idle and power-off acknowledgements, the
orderly-stop contract of [pvr_gpu_stop.h](../include/pvr_gpu_stop.h) holds, the
failed jobs complete with RESET and their slots return to the client. When the
handshake is denied or times out, or the firmware does not answer, the stop
latches FAILED, the worker asserts reset and powers domain 5 off as
containment, and the failed jobs complete with LOST. LOST slots stay owned by
the device for the rest of the boot. Neither path changes the contracts of
`pvr_gpu_stop.h` or [gpu-power.md](gpu-power.md).

## Failure classes

| Failure | Class | Retained |
| --- | --- | --- |
| Bad session, slot, length or state; deadline above `max_job_us`; third job; submit during reset or after LOST; close with jobs in flight; register read while parked | 1 | Nothing |
| Firmware hash mismatch | 1 | Nothing; the GPU is not started |
| Missed deadline, firmware event that starts a reset, or missing cleanup return, followed by an acknowledged stop | 2; counts against three resets per boot | Nothing after the restart |
| Session closed while the other session runs | 2 | Its leases, PENDING_RELEASE until the next acknowledged stop |
| Reply post failure | 3 for that session (LOST) | That session's leases |
| Forced-idle or power-off handshake denied or timed out; firmware silent; any drain-list, readback or domain-off timeout; unpark boot timeout | 3 | Every GPU lease, firmware memory and page table; ownership of the GPU clock, the DCDC4 token and domain 5 |
| GPU found executing at boot | Image-wide unknown | Reported as an unexcluded remote writer |
