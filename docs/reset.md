# Reboot path

Status: **implemented** in the resident lifetime of the integrated OS image.
A host model passes. WDT0 expiry and the cold reboot it causes are **unproven**
on the board ([watchdog.md](watchdog.md)).

The resident OS image reboots the board in one way: it stops renewing the
armed WDT0 and lets it expire. No code in the image writes a watchdog
register to cause a reset. The resident contract therefore stays as the
stage-2 loader arms it ([boot-slots.md](boot-slots.md)): one WDT0 writer, mode
`0x41` (four-second interval, enabled), configuration `1` (reset the whole
system, A733 User Manual V1.00, section 8.3.6.5), output `2`, and no disarm.

## What stops renewal

The supervising task (`apps/board/main.c`, resident branch) calls
`board_resident_renew` (`apps/os/services.c`) every 20 ms. That function
refuses renewal in each of these cases:

- a byte of `a733_fault_pending` is nonzero, because some core entered the
  EL3 fault handler ([fault-capture.md](fault-capture.md)); this covers a
  vector fault, a deliberate exit, an exception taken at EL3 and an entry that
  fails the handler's checks, on any of the eight cores;
- a reported service is older than its window;
- a service is still silent at the loader's first expiry, four seconds after
  arming;
- the renewal itself fails.

The check is one relaxed 64-bit load per pass, with no lock or barrier.

On a refusal, or when the board stops being live, the task keeps its
`board_fail` and Type-C revocation, then calls
`a733_smp_exit_privileged(5, detail)`. Status 5 means that renewal stopped.
The detail is one plus the index of the first stale or silent service, and 0
for a pending fault, a failed renewal or a board that stopped being live.
The exit is an SMC, so it reaches the supervising core's slot as a deliberate
exit, and on core 0 `a733_smp_stop` also writes it to the resident result.
WDT0 then expires within four seconds of the last renewal and resets the board
into a cold boot, which the stage-2 loader starts again.

Each park keeps every lock, buffer and DMA job of its core until the reset.
The other cores run for at most about 4 s, and the OS image writes no storage.

## Sequences left out

- **Short interval.** The pinned board kernel `sunxi_wdt.c` (`wdt_v103`,
  `sunxi_wdt_restart`) writes mode interval 0 (0.5 s) with enable and
  restarts. That adds a second WDT0 writer and a mode that the contract
  checks refuse, to save at most 3.5 s before a reboot that loads a slot for
  several minutes.
- **Software reset.** The pinned bootloader `b791be84`
  (`arch/arm/mach-sunxi/board_sun60iw2.c`, `sunxi_board_reset_cpu`) writes
  `WDT_SRST_REG`. Manual section 8.3.6.3 requires the watchdog timer to be
  disabled for that use, which the resident contract forbids.
- **PRCM system reset.** The manual lists no such register.

The RTC general-purpose registers (manual section 9.1.7.14) survive reset, but
bootloader `rtc.c` uses indices 0, 2 and 6 and the boot0 owners of the others
are not known, so no reset reason is stored there. A cold boot and a watchdog
reset cannot be told apart ([watchdog-reset-evidence.md](watchdog-reset-evidence.md));
the previous-boot report of [fault-capture.md](fault-capture.md) is the only
record of why the board restarted.

## Check

`checks/reset-latency.py --output DIR [--baseline-services OLD/services.c]
[--capture fault-capture/result.json]` compiles `apps/os/services.c` into
`checks/reset-latency.c` with a modeled counter and WDT0. It parks each core
once at every millisecond of the renewal second and takes the worst case for
task placement: every service and the supervisor run on other cores. A park
of core 0 also stops the tick and every task. The model requires every park
to end in an expiry within 4.02 s (one 20 ms pass plus 4 s), the supervisor
to refuse within one pass, and the stop detail to name each stalled or silent
service. With `--capture` it also requires that the instruction check saw
each kind of park on each core store its pending byte.

| Measure | Merge base | This image |
| --- | --- | --- |
| Modeled parks that end in a reboot | 1,000 of 8,000 (core 0 only) | 8,000 of 8,000 |
| Park to watchdog expiry | Unbounded on cores 1..7 | At most 3.999 s |

## Board evidence still required

WDT0 expiry in the resident lifetime must reboot into a cold boot; this step
is shared with the watchdog expiry trial in [validation.md](validation.md).
