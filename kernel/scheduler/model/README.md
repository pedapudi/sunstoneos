# Bounded scheduler wake models

These host checks cover the protocol in
[`scheduler-partitioned.md`](../../../docs/designs/scheduler-partitioned.md).
They establish properties of finite abstractions. They do not establish Arm
instruction correctness, hardware interrupt completion, latency or starvation
bounds under unlimited arrivals.

Run the suite with the project Python environment and a cached TLC jar:

```sh
/home/sunil/.cache/sunstoneos/venv/bin/python checks/scheduler-wake-model.py \
  --tlc-jar /path/to/tla2tools.jar --build-dir /path/to/external/evidence
```

The evidence directory receives configurations, complete TLC counterexamples
and a JSON result with state counts, elapsed times and peak resident memory.
Python negative cases print shortest action traces. Every negative case must
violate its designated invariant; unexpected passes fail the suite. TLC runs
with one worker and a 1 GiB Java heap. The runner sets Java's temporary directory
to the evidence directory. No board access occurs.

## Publication buffers expose the store-to-load race

`Wake.tla` has two producers and one owner. Producers release-publish distinct
fixed-slot hints into private buffers. Independent propagation actions commit
those hints to coherent inbox storage. The owner independently buffers its
release dispatch publication and pending-SGI clear. Acquire observations read
committed coherent state. A DMB blocks the following load until preceding local
stores have propagated. Releases alone do not drain those buffers.

The owner begins inside an active SGI with the coalescing flag set. Claiming
that flag, sending a hardware interrupt, hardware pending state, active state,
final inbox inspection and exception exit are separate transitions. An eligible
pending interrupt prevents indefinite WFI sleep. Idle entry and lowering to a
priority outranked by both producers share the same Boolean dispatch state.
A lost wake means both publishers finish while the owner commits sleep without
observed work or a pending interrupt.

The correct configuration exhausts all reachable states. Removing either DMB
admits the store-buffering lost wake. Moving the pending clear after inspection
admits a coalescing race. Active interrupt state alone cannot satisfy the
pending-interrupt condition after exception exit.

This is a multi-copy-atomic, per-location coherent operational abstraction.
It enumerates buffer propagation and instruction interleavings. It does not
encode the complete Arm axiomatic memory model, speculative execution or every
possible acquire read-from relation. The separate buffers model the required
store-to-load ordering explicitly; stronger hardware guarantees need an
instruction-level model and linked-code review.

## Gate, lifetime and transfer checks use separate finite state spaces

`explore.py` uses breadth-first search with full immutable states as dictionary
keys. It explores every successor until the reachable queue is empty. Positive
cases have no depth cutoff. Negative cases stop at the first invariant violation.

- Notification cases cover every ordered pair of give, bit set, overwrite,
  no-overwrite and state/value clear actions. Two producers each perform at most
  two calls. A call uses one whole-word CAS or registers before taking a FIFO
  ticket. Gated clear/set steps preserve registration count. The owner consumes
  under the same gate. A second gated wait can restore a matching WAITING word,
  allowing a delayed fast snapshot to span a complete gate interval. Wait
  generations reset the completion history. Stable observation must preserve
  an established completion despite intervening clears. Negative cases remove
  the sticky latch, registration during payload mutation or observer exclusion.
  The observer negative reads the actual payload and applies the same completion
  check as gated consumption. A read between payload clear and set can miss an
  established completion; a stable post-set read with its latch set succeeds.
  This case checks completion preservation; duplicate insertion is checked by
  the separate block case.
- The block case separates gated wait entry, PREPARING, physical switch,
  notification completion, delayed hint and timeout. Two producers race one
  timeout. The winner cannot change after notification, and current-wait ready
  insertion occurs at most once. Removing the timeout winner check fails.
- Retirement cases explicitly buffer two publishers' reference increments and
  the deletion admission close. Reference-pin and deletion DMB guards precede
  admission and zero-count observations. Delayed publication, live bits and
  detached bits prevent incarnation reuse until references and owner drains
  retire. Removing either DMB or the detached drain fails.
- Migration cases have two producers and two owners, an off-CPU transfer and
  destination adoption. Producers retain their sampled owner epoch through
  delayed publication. Old-owner bits require forwarding after adoption;
  forwarding retirement waits for references and live/detached bits. Priority
  raises republish pending work and force an urgent owner request. Negative
  cases remove forwarding, retire forwarding early or omit priority repair.

The gate model assumes native atomic CAS and count/payload RMW linearization.
FIFO serving publication and transfer adoption use abstract acquire/release
steps. Their weak-memory implementation is not proved by the gate model.
Notification arithmetic is bounded by call count and does not model 32-bit
wrap, every API return value or the complete FreeRTOS notification API. The
second wait abstracts prior completion/consumption before installing its next
WAITING state. Permanent descriptors and valid caller handles are assumptions;
calling a deleted handle does not gain validity. Retirement keeps admission
closed after reuse eligibility, so it proves the retirement boundary rather
than a complete new-incarnation API session.

The models split notification, block completion, dispatch, lifetime and transfer
properties to keep exploration small. Their conjunction does not prove that a
future implementation composes those protocols correctly. Source review,
linked-kernel stress and physical qualification remain required.
