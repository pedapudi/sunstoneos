# Radio consumers draw copied bytes from the actual CE entropy service

The radio entropy adapter connects asynchronous A733 crypto-engine requests to
the synchronous random-byte callbacks used by WPA2 and FreeRTOS+TCP. One radio
task refills a fixed reservoir. The radio and actual IP task consume copied
bytes without calling the hardware. Independent source review, the focused
sanitizer composition and native compilation passed. Physical entropy remains
unqualified.

The adapter is defined in [a733_radio_entropy.h](../include/a733_radio_entropy.h)
and implemented in [a733_radio_entropy.c](../drivers/net/a733_radio_entropy.c).
It borrows an initialized [CE entropy client](entropy.md), its actual shared
session and the existing common pool mutex. It creates no allocator, CE owner,
clock token or mutex. Other CE clients must use the same session and mutex.
The dedicated entropy client cannot have another request owner.

## Physical qualification remains an independent prerequisite

Construction requires an external qualification callback bound to the actual
CE session and an immutable qualification record. That authority must establish
raw-source characterization and successful startup testing for the current
health-test enable epoch. It must also cover the session's retained lifetime.
Neither programmed health thresholds nor a count of conditioned output requests
establishes those facts. This adapter supplies no successful production callback.

The qualification context, CE callback backing, task identities, source client,
pool and session remain allocated throughout the image lifetime. All consumers
preserve the exact retained CE token. Reset, token release and profile replacement
require a separate recovery protocol and invalidate this adapter's assumptions.
No configuration field can make the current board entropy physically qualified.

## Only the recorded owner advances CE work

Call the constructor during serialized boot before scheduling. The owner task
calls `a733_radio_entropy_poll` before its normal radio iteration. Each call
takes the existing pool mutex without waiting and advances at most one actual
CE request or poll. The adapter requests 256 bytes at a time and preserves one
absolute refill deadline across CE-client contention and partial completion.
Pool-mutex contention before the first admitted refill remains pending.

A completed refill enters private staging. The owner checks the deadline and
actual clock token before publishing it into a 512-byte reservoir. Contention
at this final token check preserves staging and retries metadata; it does not
repeat a completed hardware task. The short reservoir lock never spans a CE
call, mutex operation or callback. The owner stops requesting bytes when less
than a complete refill fits.

The constructor binds the actual initialized counter used by the CE clock
registry. Once a refill has a deadline, owner polling observes that counter
before attempting the pool mutex. Expiry therefore closes copied-byte admission
even when another pool consumer prevents CE access. Partial source staging is
erased when the owner can obtain the mutex and complete metadata retention.

The application must prefill before starting WPA2 or the network stack, then
poll often enough for its admitted workload. No new radio-session call is wired
by this component alone. Full native association still requires concrete boot
admission, firmware artifacts, SoC identity and an application coordinator.

## Synchronous consumers cannot block or access CE

The byte callback matches the existing supplicant interface. The word and
sequence callbacks match the existing network interface. They accept only the
recorded owner or the task returned by FreeRTOS+TCP's actual
`FreeRTOS_GetIPTaskHandle` accessor. No caller-supplied task ID grants access.

Each successful call copies 1–256 fresh bytes and erases the consumed reservoir
positions. A sequence request consumes a fresh independent 32-bit word; endpoint
addresses do not seed a deterministic generator. No PRNG or fallback seed is
provided. A refusal leaves the caller's entire output unchanged.

Insufficient bytes, short-lock contention or invalid ownership returns failure.
The existing network service closes output when either randomness callback
fails. Reservoir capacity therefore does not establish a sustainable network
rate or scheduling bound. Those depend on actual consumers and CE service time.

## Faults preserve resources and discard private copies

The consumer checks the actual atomic session, counter and registry fault flags
before copying bytes. The session publishes its fault before attempting registry
retention, so registry contention cannot hide another CE client's failure.
An observed fault closes admission and erases reservoir contents. Owner polling
also checks CE binding, request results, deadlines and final token authority.
Failure erases adapter staging, retains the actual CE session and invokes the
entropy client's retained path to erase partial conditioned bytes. Pending
retention retries metadata only. The device-owned page is never cleared or
released while DMA access may remain possible.

Previously completed consumer calls cannot be revoked. Concurrent fault and
consumer calls are ordered at the short reservoir admission check. A later
fault cannot authorize another consumption. The current cooperative mappings
do not protect private state from arbitrary privileged corruption.

## The selected check exercises only the new connection

[The focused fixture](../checks/a733-radio-entropy.c) reuses existing CE model
helpers with the actual entropy service, CE session, controller, clock registry
and pool. It does not execute the earlier CE test functions. Task identity,
mutex dispatch, the counter, MMIO and DMA-produced bytes are explicit model inputs.

The selected cases cover refill and circular-buffer consumption, copied and
wrong-task refusal, output aliases, exhaustion, mutex contention, final-token
contention, late authority loss and deadline retention. The
[runner](../checks/a733-radio-entropy.py) preserves compiled input dependencies,
the executable and native objects before one sanitizer execution. It compiles
the adapter against the actual single-core and cached eight-core task headers.
Existing CE protocol and network-stack evidence remains separate and reusable.
The five direct native consumers of the session header are compiled once to
cover atomic fault publication; assertions preserve the prior field layout.

The preserved evidence is under
`~/.cache/sunstoneos/evidence/radio-entropy-20261017/batch2/`.
It includes the successful sanitizer result, compiled source dependencies,
executable and seven native objects. One earlier build stopped before execution
because renaming the archived fixture's `main` removed its implicit return rule;
the scoped diagnostic adjustment leaves those unexecuted cases unchanged.

Execution does not establish random-source quality, physical CE behavior,
native radio application execution, or physical network operation.
