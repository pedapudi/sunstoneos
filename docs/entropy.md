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

## Health provisioning requires an assessed profile

The secure health controls provide a repetition-count cutoff, an
adaptive-proportion cutoff, a delay between windows and test enables. The
controller accepts continuous testing with a 512- or 1024-sample window.
It requires an authenticated profile bound to the actual silicon, supplies,
RC16M clock and enabled double-source mode. It supplies no default thresholds.

With cold provisioning selected, the controller first refuses either enabled
test. Under complete CE exclusion and independent prior DMA drain, it selects
double-source mode and writes the supplied cutoffs and window. It enables both
tests last and verifies readback. Native writes preserve reserved control bits
and refuse reconfiguration once either test is enabled. Any uncertain operation
retains ownership. Existing enabled profiles can instead be adopted after exact
readback verification.

The manual gives these cutoff formulas:

- Repetition count: `C = 1 + ceil(-log2(alpha) / H)`.
- Adaptive proportion: `C = 1 + CRITBINOM(W, 2^-H, 1-alpha)`.

Here `C` is the cutoff, `H` is assessed minimum entropy per raw sample, `alpha`
is the selected false-alarm probability and `W` is the test window length.
`CRITBINOM` denotes a binomial quantile. The manual's example prose associates
169-bit samples with inconsistent values for entropy and window length. Those
examples are insufficient to characterize this board and are not used as defaults.

`checks/crypto-health-cutoffs.py` evaluates both formulas with exact integer
and 80-digit decimal arithmetic. Given an assessed `H`, `alpha = 2^-k` with
`20 <= k <= 40` and the source type, it prints the four register words in the
form the controller accepts: the two cutoffs, a zero delay for continuous
testing and both enables with `W` of 1024 for a binary source or 512
otherwise. For example, a binary source assessed at 0.5 bits per sample with
`alpha = 2^-20` gives `{0x29, 0x319, 0x0, 0x30400}`. Each run first reproduces
the adaptive-proportion cutoffs in SP 800-90B Table 2 for `alpha = 2^-20`
(941, 840, 748, 664 and 589 for binary sources at `H` from 0.2 to 1, and 410,
311, 177, 62 and 13 for non-binary sources at `H` of 0.5, 1, 2, 4 and 8) and
the standard's repetition-count example. The script never chooses `H`. The
values it prints are only as valid as the raw-source assessment that supplies
`H`, and no such assessment of this silicon exists.

[NIST SP 800-90B](https://nvlpubs.nist.gov/nistpubs/SpecialPublications/NIST.SP.800-90B.pdf)
describes raw-source assessment and health testing before conditioning. Its
startup requirement covers at least 1024 raw samples. The A733 command returns
SHA-256-conditioned output, so output statistics or a chosen discard count do
not demonstrate that requirement. The
[2025 errata](https://csrc.nist.gov/files/pubs/sp/800/90/b/final/docs/sp800-90b_errata_potential_updates.pdf)
do not supply an A733 profile or startup protocol. No standards-compliance claim
is made.

Inspection of the board's published monitor firmware identified security setup
but did not identify a CE health profile. That bounded inspection does not
exclude indirect firmware paths. Secure MMIO access still requires actual
platform authorization; no existing secure-monitor entropy service is assumed.

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

## Evidence and remaining qualification

The focused model in `checks/a733-ce-entropy.c` composes the real service, CE
controller, memory pool and clock registry. It uses deterministic DMA fixtures
to cover secure-write uncertainty, repeated requests, partial-result failure,
rearm refusal, stale binding and deadline failure with busy retention. These
fixtures do not characterize hardware randomness.

`checks/a733-ce-entropy.py` checks the pinned health/completion references and
native constructor calls to the existing singleton pool and CE operations.
The unchanged descriptor, initial admission and cache-operation behavior retain
the evidence recorded in the CE driver record.

The initial entropy-service sanitizer model, source checks and native AArch64
compilation passed. The shared-session extraction is covered by the focused
[hash composition model](crypto-hash.md), including both client orders and
deferred health provisioning.
A relocatable link resolves the CE service against the actual pool, registry
and translation implementations. Final image linker symbols, runtime `memset`
and the pool module's separate DMA constructor remain external link inputs;
this is not a bootable-image check. The initial three CE objects contained 16,264 bytes
of code and no static data or BSS. Their largest direct compiler-reported stack
frame was 352 bytes, without a whole-call-chain bound.

Exact executed inputs, objects and logs are preserved in
`~/.cache/sunstoneos/evidence/ce-service-20261016/batch1/`.

Production use still requires raw-source characterization, a valid assessed
profile, successful startup testing for each enable epoch, actual secure access,
clock/supply/mapping authority, recovery qualification and physical CE operation.
The engine's remaining cipher modes and its asymmetric, key-ladder and
secure-key APIs remain separate hardware coverage requirements.
