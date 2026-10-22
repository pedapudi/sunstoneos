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
| 106 debug dump, 107 statistics, 108 clock rate | Recorded |
| Any other type, including 101 and 102 (depth storage is static) | Full reset |

A per-session answer to event 105 (fail only the guilty session's jobs when
the reason is a lockup or overrun of one context) is **pending board
evidence**. It is added only after an injected lockup in one session leaves
the other session's jobs completing correctly.

## A full reset releases memory only after an acknowledged stop

A full reset runs `pvr_gpu_stop` from its first step. An acknowledged forced
idle (state 2), an acknowledged power off (state 0) and the full idle and
reset sequence are the same evidence as an idle park, so a stop that reaches
STOPPED this way is a verified quiesce. The failed jobs complete with RESET
and their slots return to the client. When the handshake is denied or times
out, or the firmware does not answer, the stop latches FAILED. The worker then
masks SPI 63, makes no further GPU or power I/O, and completes the failed
jobs with LOST. LOST slots stay owned by the device for the rest of the boot.
A render target lent to the display stays out of release until the display
owner returns it, on either path.

## Failure classes

| Failure | Class | Retained |
| --- | --- | --- |
| Bad session, slot, length or state; third job; submit during reset or after LOST; close with jobs in flight; register read while parked | 1 | Nothing |
| Firmware hash mismatch | 1 | Nothing; the GPU is not started |
| Missed `max_job_us`, firmware event that starts a reset, or missing cleanup return, followed by an acknowledged stop | 2; counts against three resets per boot | Nothing after the restart |
| Session closed while the other session runs | 2 | Its leases, PENDING_RELEASE until the next acknowledged stop |
| Reply post failure | 3 for that session (LOST) | That session's leases |
| Forced-idle or power-off handshake denied or timed out; firmware silent; any drain-list, readback or domain-off timeout; unpark boot timeout | 3 | Every GPU lease, firmware memory and page table; domain 5, the GPU clock and the DCDC4 token, with no further GPU or power I/O |
| GPU found executing at boot | Image-wide unknown | Reported as an unexcluded remote writer |
