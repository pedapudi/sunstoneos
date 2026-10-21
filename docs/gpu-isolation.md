# Healthy GPU shutdown uses the pinned firmware and platform sequence

The native backend implements the normal shutdown contract from the pinned GPU
and A733 platform sources. It closes submission, obtains fresh forced-idle and
power-off acknowledgements, checks their independent firmware states, polls GPU
idle conditions, asserts reset with readback, and completes platform power-off.
Successful completion permits firmware-memory reclaim. Software checks cover
this composition; physical firmware startup, computation and shutdown remain
unqualified.

This contract applies to a healthy device with intact firmware control and
owned backing. Failed commands, uncertain DMA, transport loss or deadline expiry
retain backing and outstanding ownership. They require independent recovery.
A power-domain OFF sample alone cannot establish the complete shutdown sequence.

## Firmware state distinguishes acceptance from a denied transition

Each power command starts by clearing the shared synchronization word. Native
completion requires that word to become one. The native memory adapter then
acquires the firmware system-data power state at byte offset eight. Forced idle
requires state two; power-off requires state zero, meaning idle and ready for
full power-down. A synchronization acknowledgement can also acknowledge denial,
so an inconsistent state latches failure before register teardown.

The existing stop controller keeps event and command-return service available
through both handshakes. It retires that service before the reference BIF/MMU,
external-read, system-level cache and firmware-wrapper idle polls. Soft reset
and its readback follow those observations. Reset-cleared counters cannot
substitute for the preceding idle checks.

## Platform power-off advances without blocking the service worker

The stop completion stage admits platform teardown once. Its optional polling
callback performs one bounded native power-off step per worker advance. Pending
completion yields to the worker. The same absolute CLOSE deadline covers both
firmware handshakes, idle/reset and platform teardown.

The native backend requires platform OFF, resource token zero and no delegated
lease before invalidating cached firmware backing and returning its adapter to
OFFLINE. Every callback failure, invalid result, clock regression or expired
deadline latches failure and prevents subsequent stop callbacks. Bookkeeping
preserves a resource release that already completed, even when a later clock
check fails. Reserved allocation parents and copied application output remain
owned after ordinary CLOSE. The optional [standalone GPU lifecycle profile](gpu-render.md#standalone-sessions-can-dispose-and-reconstruct-child-resources)
uses this full healthy-stop proof for explicit child mapping/allocation disposal
and fresh reconstruction; global firmware/provider parents remain pinned.

An optional synchronous platform override remains supported. Its result must be
one and its completed platform state must satisfy the same ownership checks.
No additional NSI request-block API is required by either pinned normal GPU
shutdown path. This source comparison establishes the intended healthy sequence;
it does not qualify arbitrary failed-device recovery or physical operation.

## Pinned sources define the healthy sequence

The board-source revision is `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
Relevant cached inputs retain their original license notices:

- `bsp/drivers/gpu/imagination/pvr_power.c`, lines 94–111 and 353–376,
  supplies forced-idle/off synchronization followed by platform power-off.
- `bsp/drivers/gpu/imagination/pvr_fw_startstop.c`, lines 205–322,
  supplies the local idle checks and reset sequence.
- `bsp/modules/gpu/img-bxm/linux/rogue_km/services/server/devices/rgxpower.c`,
  lines 414–436 and 1613–1628, checks the independent OFF and forced-idle
  firmware states after acknowledgement.
- `pvr_rogue_fwif.h` defines the system-data state at offset eight and the
  OFF/forced-idle values. `bsp/drivers/pm_domain/pck600_domains.c` supplies
  the A733 domain request/status sequence.

## Focused software evidence preserves failure ownership

Evidence is under `~/.cache/sunstoneos/evidence/gpu-returning-integration-20261018/`.
The sanitizer stop fixture uses the actual pinned firmware feature record and
passes 120 retained-failure cases, including asynchronous pending, timeout and
invalid completion results. The native instruction fixture executes the real
backend and memory accessors with modeled mappings and power operations. It
checks both firmware states, denied transitions, invalid acknowledgements,
pending teardown, begin/poll failures, expiry and completed-release retention.

The actual linked compute client check supplies modeled service replies. It
requires owned output copying before CLOSE and STOPPED before success, while
pending and failed shutdown cannot publish success. It supplies no GPU output
or physical power evidence. The frozen candidate reuses accepted startup and
unchanged objects; its artifact record identifies every rebuilt dependency.
Initial RAM/master exclusion, supported settled supply/clock settings, native
firmware execution and physical orderly shutdown remain board prerequisites.
