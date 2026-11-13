# Network and SDIO owners wait for events

Status: socket and native SDIO event paths are implemented and host-tested.
The native source closure compiles with `SUNSTONE_A733_RADIO_IRQ=1`.
The image must select its generated INTID 194 route, source binding and owner
wait before it can run the event path. Physical interrupt delivery, FIFO service
margin and retirement remain unqualified.

## Software publications wake the socket owner

A network configuration may supply `sunstone_net_event_source`. Its bind
callback must establish the transport's interrupt ownership for the worker.
Its deadline callback supplies the earliest absolute transport or protocol
deadline in counter microseconds. Its ready-work callback identifies software
or FIFO work remaining after a bounded transport pass. A supervised native provider also includes
its immutable session end and the owner's blocked-service certificate.
The event profile refuses construction when the precise deadline runtime is
absent or any callback is missing. A failed binding retains the service and
parks its standalone worker. An owned session receives a failed activation.

Requests, cancellation, transport interrupts, DNS results, socket events and
socket disposal notify the owner. The worker checks authoritative state after
every notification. It drains queued requests as software work, then blocks
until the earliest transport deadline or outstanding request deadline. With no
pending timer it blocks indefinitely. A timer that matures during wait setup
receives one immediate service pass. An unchanged expired transport deadline
after that pass is a provider failure, preventing a busy loop. Ready transport
work drains before sleeping. RX consumption, TX completion and packet disposal
publish another hint so coalesced enqueue notifications cannot strand a packet.

An IP-task queue submission arms the client's backpressure bit before attempting
the nonblocking send. The IP event-dequeue hook consumes those bits and wakes
the owner when capacity becomes available. Publication before send covers
capacity becoming free during a failed send. Bind, close, DNS and accept retries
use the same path. No unrelated socket or transport event is required to retry.

Request admission converts the declared tick duration once and adds it to the
current microsecond timestamp. Expiry comparisons use that absolute deadline,
including DNS and accepted-socket handoffs. Conversion overflow refuses the
request before queue publication. An expired accept handoff classifies its
deadline once, then waits for the IP task's disposal acknowledgment. Timeout
cannot release an accepted socket or its reserved identity.

The pinned FreeRTOS+TCP socket wake callback runs before its event-group bits
are set. The callback therefore records the BOUND event before notifying the
owner. A higher-priority owner can safely observe bind completion immediately.
Accepted children receive the same callback; disposal is published only after
the stack's final access. The source is FreeRTOS+TCP revision
`b86ad086e9870099bb0ac18a75dcb7f6dd508cc7`, `FreeRTOS_IP.c`
`eSocketBindEvent`, and `FreeRTOS_Sockets.c` `vSocketWakeUpUser` and
`FREERTOS_SO_WAKEUP_CALLBACK`. The [network source record](../radio-network.md)
defines the pinned stack and license.

## Native SDIO retains the image's interrupt ownership

The [radio source record](../../drivers/net/radio-sdio-reference-sources.json)
pins the selected vendor kernel at
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Its
`bsp/drivers/mmc/sunxi-mmc.c` defines the effective interrupt sequence:

1. `sunxi_mmc_exe_cmd`, lines 2755–2770, writes the request interrupt mask
   together with persistent SDIO and card-detect enables before the command
   argument, publication barrier and command start.
2. `sunxi_mmc_irq`, lines 1271–1365, reads masked controller status and IDMAC
   status, records completion and error bits, acknowledges those sources,
   completes a finished request and separately signals a card function event.
3. `sunxi_mmc_enable_sdio_irq`, lines 2132–2149, preserves unrelated mask bits
   when enabling or disabling the persistent function interrupt.

The native controller uses a different transfer engine from the vendor driver.
`sunxi_sdio.c` uses CPU FIFO transfers and a bounded FIFO batch rather than the
vendor driver's DMA completion path. The event binding sets global interrupt
enable in GCTRL and writes each request's interrupt mask before command start.
The mask selects command completion and errors, plus data completion and the
directional FIFO request for a data transfer. Accounted completion bits are
acknowledged promptly and removed from the mask. The FIFO source is removed
once every requested byte has moved. Existing response, byte-count, busy and
late-error checks still decide completion.

The A733 user manual version 1.00, sections 11.3.8.13–17, defines the mask,
raw status and FIFO watermarks. Event preparation programs and reads back
receive watermark 15 and transmit watermark 240 for the 256-word FIFO.
The transmit reset value zero supplies no trigger. At 25 MHz on four data
lines, the FIFO holds 81.92 µs of data. Either selected watermark leaves about
76.8 µs of service margin. This calculation does not prove native scheduling
latency or assume automatic card-clock suspension. Late FIFO overflow or
underrun remains a retained failure.

The retained platform admission already excludes independent GIC handlers and
module writers. The generated image route supplies interrupt group, priority,
CPU and handler authority. Its EL3 installer sets level INTID 194 to CPU0 at
Secure priority `0xd8` and preserves the common return procedure. The native
owner checks the readable level, CPU route, Nonsecure priority `0xb0`, and initial
disabled, inactive and clear pending state. Group registers read as zero from
Nonsecure EL1; their proof belongs to the EL3 installer. A refused binding
neither clears inherited pending state nor authorizes controller access.

The interrupt handler issues one SPI disable and notifies the owner. It touches
no controller register. The owner accumulates and acknowledges raw status,
services the FIFO, programs useful sources, rechecks status and rearms the SPI.
The kernel critical section preserves a notification that races rearm or sleep.
Deferred GIC disable propagation is observed without reissuing the disable.
The platform's event-preparation API retains the owner, lifetime and serialized
host-access checks during startup, operation and stopping.

Function interrupt bit 16 remains recorded while its receive data drains.
Only that bit is suppressed in the controller mask; command and FIFO interrupts
remain enabled for the transfers that drain it. An empty AIC receive-status
read permits acknowledging bit 16 and checking for reassertion. A cleared source
allows an idle receive owner to sleep for its next function interrupt. A
reasserted source retains the declared protocol observation deadline.

Controller reset, clock-update completion and DAT0 release have no guaranteed
completion interrupt in this path. Their observations use 50 µs absolute
deadlines capped by the original operation limit. AIC credit and function-ready
observations retain their declared protocol interval. Supply and module settling
use their original absolute bounds. No tick count paces the bound native owner.

Both normal shutdown and failure abort require the interrupt retirement hook
before closing controller clocks. Retirement masks delivery, clears and verifies
the controller mask and global enable, observes GIC disable propagation and waits
for the active handler to return. Only then may it clear stale pending state.
An active interrupt is never cleared in software. A failed proof retains clocks,
power, handler and backing storage. The binding changes no voltage, rail or
firmware operation. An image without the binding retains its polling reference.

## Host checks cover idle sleep and publication races

`checks/event-misc-network.py` executes the production request publications,
socket-event capture and wait selection with the real FreeRTOS host kernel and
precise deadline runtime. The owner remains blocked throughout a one-second
idle observation. It wakes for a request, cancellation, a modeled interrupt and
a socket bind publication. A request submitted 123 µs after a tick expires
2,000 µs after submission. An expired accepted-socket fence stays asleep for
its publication. Invalid elapsed provider deadlines and overflowing request
durations are refused. Full IP-queue cases cover bind and close, including
capacity becoming free during a failed send. Bounded ready work drains without
sleeping. Reintroducing an idle tick delay or removing the backpressure
subscription makes the check fail.

The real TCP/UDP generation-revocation model also passes with the socket
callbacks enabled. It exercises the existing polling reference. It does not
execute the event profile's native transport provider or establish physical
interrupt latency, throughput, recovery or idle power.

`checks/event-misc-sdio.py` runs the actual controller and interrupt owner with
the real FreeRTOS host kernel. The owner stays blocked during a full idle second.
Cases cover command completion, a transmit larger than the FIFO, a receive tail
below the watermark, function-bit retention during CMD53, a rearm race, delayed
GIC disable propagation, missing completion, late error and initial refusal.
Its tick-wait mutation fails. The platform host-access functions are production
code in this test. Separate cases run the complete platform resource graph to
prove that normal and failed shutdown cannot bypass interrupt retirement.
The AIC transport case checks empty-receive sleep and exact credit cadence.
Native compilation selects the binding in seven production source files.

The dedicated radio client registers its own precise wait. Scan-only sessions
keep that client blocked until quiescence or the session end. A network client
checks its copied reply queue and ready software admissions before sleeping.
Network activation, reply publication and request-queue capacity notify it.
The generic reply publisher preserves an application's notification index zero.
The client uses its declared absolute exchange limit until completion, then
waits under the session limit. Admission restored after clean
link recovery and release of a contended client gate also notify waiting
clients. The owner includes any deadline supplied by the shared Bluetooth
consumer. A matured shared
deadline receives one immediate service pass; an unchanged expired deadline
then refuses the wait. The host client cases cover idle sleep, copied replies,
capacity publication, restored admission, ready work and both notification-index
assignments.

A bounded board pass publishes ready work when it stops after progress or
changes its orchestration stage. The next wait consumes that publication before
arming a timer. Shutdown and abort exclude the inactive socket and protocol
timers. Due RSN callbacks run under the existing supplicant serialization even
while receive waits for a function interrupt. Only active protocol states supply
timers; completed authorization and retirement records cannot create an immediate
retry loop. Focused board-poll and transport cases exercise those transitions.

The scan application's status consumer waits to its original work finish through
`ss_wait_until`. The radio owner publishes each changed completion state before
notifying that consumer; repeated states produce no hint. The consumer checks
its finish limit before accepting completion. Host cases exercise the actual
application, status publisher and absolute OS wait across publication races,
preemption gaps, early hints and expiry. The relative wait and private return
reserve have separate compatibility cases.
