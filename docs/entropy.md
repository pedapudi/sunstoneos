# Repeated A733 hardware random-byte requests

The entropy service implements bounded hardware random-byte requests through the
A733 crypto engine. Each request returns 1–256 bytes after every constituent
32-byte TRNG task succeeds. The service uses the actual clock registry, CE
controller, native cache operations and singleton linker memory pool.

Output is **unqualified for production security use**. Assessed health thresholds
alone do not establish raw-source quality or successful startup testing for the
current test-enable epoch. The inspected hardware documentation provides no
startup-completion counter or raw-sample export interface. Counting requests for
conditioned output cannot resolve those missing facts. Wi-Fi key generation and
other security consumers must wait for those qualification requirements.

The service interfaces are in `include/a733_ce_entropy.h`; the implementation is
`drivers/crypto/a733_ce_entropy.c`. The native constructor is in
`drivers/crypto/a733_ce_native.c`. The underlying command format, clock checks
and security boundaries are described in [the CE driver record](crypto-engine.md).

## Health tests run at hardware defaults

The CE driver follows the vendor's non-secure sequence ([crypto-engine.md](crypto-engine.md)): it programs no
health-test register and reads none, so the TRNG's repetition-count and adaptive-proportion tests run at whatever the
hardware resets to. Nothing in this service establishes that they run, what their cutoffs are, or that the source
passes any assessment. Output remains **unqualified**.

## One reserved page supports sequential tasks

The native constructor runs after platform initialization during serialized
boot, before concurrent users. It reserves the one native CE owner and rejects
a second construction. An initialized owner remains reserved permanently. It obtains the existing singleton linker pool,
acquires one page and binds real CE MMIO/cache operations. It does not construct
a second allocator over the same RAM. Binding replaces caller address fields
with the allocated page's physical descriptor and output extents.

The shared CE session owns the actual page, token and controller. The complete
page remains in device ownership after binding, including between requests.
A mixed capability profile lets a [SHA-256 client](crypto-hash.md) bind the same
`engine` pointer. One typed client slot serializes entire entropy requests and
hash streams; it does not create another engine reservation. The common pool service lock must serialize binding, requests, polls
and every allocator or consumer transition. A successful task permits the
controller's CPU access and reuse of these same buffers under continuous
reservation. It does not authorize pool release, global reset or clock removal.

Each request receives a generation number and one absolute deadline. The service
runs the actual controller for each 32-byte task and accumulates bytes privately.
It copies the complete requested result once, after the final successful task
and deadline check. Polling the consumed generation cannot copy it again.
Invalid output aliases and stale generations are refused. Generation exhaustion
refuses further requests instead of recycling handles.

Rearm requires the prior result to have been consumed, stable health and clocks,
clear completion and error status in both security banks, disabled task IRQs,
and cleared launch state. It reuses the retained descriptor and output page.
The manual's successful-task status and the Linux/U-Boot completion/cache paths
support this task-level reuse. They do not provide an independent global drain
primitive.

Platform authorization grants immutable authority for the controller's retained
lifetime, including later requests. Per-request deadlines only bound execution;
they cannot extend a platform authorization that expires separately. Actual
clock tokens remain authenticated. The service also checks its object identity,
controller binding and allocation identity before operating the controller.

## Failures quarantine resources and erase private staging

A failed task, changed health profile, stale binding, uncertain operation or
expired deadline prevents output publication. Caller output remains unchanged.
The service erases its private staging and block temporary; the controller also
erases its CPU temporary. It retains the actual page and registry token.
Potentially active DMA output remains quarantined and is not cleared on failure.

Registry contention during fault retention causes metadata-only retries. It
cannot resume hardware activity or publish a result. The API supplies no cancel,
reset, retry, release or shutdown operation. Full reclamation still needs an
independent engine-drain protocol.

## The association stage uses the output for bring-up only

The OS image's association stage (`apps/os/ce_probe.c`, `os_ce_entropy_*`) fills a 256-byte reservoir with 32-byte TRNG
tasks, using the non-secure bank and the hardware's default health tests, and hands the bytes to the supplicant's nonce
and key-generation callback. This is bring-up use: it lets the four-way handshake run on the board. It is not production
use. No characterization or startup test exists for this source, the source is not qualified (`entropy_qualified()` is
unchanged and still refuses), and the profile declares it with the `entropy_unqualified` byte, which only the association
stage accepts. A key made from these bytes is not a production key.

## Evidence and remaining qualification

No host check composes the real service, CE controller, memory pool and clock
registry with deterministic DMA fixtures to cover secure-write uncertainty,
repeated requests, partial-result failure, rearm refusal, stale binding and
deadline failure with busy retention. No host check covers the pinned health and completion
references and the native constructor calls. The shared-session extraction is
covered by the [hash composition model](crypto-hash.md), including both client
orders and deferred health provisioning.
A relocatable link resolves the CE service against the actual pool, registry
and translation implementations. Final image linker symbols, runtime `memset`
and the pool module's separate DMA constructor remain external link inputs;
this is not a bootable-image check. The three CE objects contain 16,264 bytes
of code and no static data or BSS. Their largest direct compiler-reported stack
frame is 352 bytes, without a whole-call-chain bound.

Exact executed inputs, objects and logs are preserved in
`~/.cache/sunstoneos/evidence/ce-service-20261016/batch1/`.

Production use still requires raw-source characterization, a valid assessed
profile, successful startup testing for each enable epoch, actual secure access,
clock/supply/mapping authority, recovery qualification and physical CE operation.
The engine's remaining cipher modes and its asymmetric, key-ladder and
secure-key APIs remain separate hardware coverage requirements.

The recorded results of the removed host checks are in [the evidence record](evidence/entropy.jsonl).
