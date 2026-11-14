# Bounded scheduler wake models

These host checks cover the protocol in
[`scheduler-partitioned.md`](../../../docs/designs/scheduler-partitioned.md).
They establish properties of finite abstractions. They do not establish Arm
instruction correctness, hardware interrupt completion, latency or starvation
bounds under unlimited arrivals.

Run the suite with the project Python environment:

```sh
/home/sunil/.cache/sunstoneos/venv/bin/python checks/scheduler-wake-model.py \
  --build-dir /path/to/external/evidence
```

The evidence directory receives a JSON result with state counts, elapsed times
and peak resident memory. Negative cases print shortest action traces. Every
negative case must violate its designated invariant; unexpected passes fail the
suite. No board access occurs.

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

## Every published hint reaches an owner inspection

`mailbox` exhausts sequentially consistent interleavings of two sources and one
owner. Each source publishes two or three distinct slots through a one-entry
outbox, so every later hint either refreshes its copy of the owner's consumed
count or spills into the bitmap; each publication then sends an SGI. The owner
takes the SGI, reads each source's count and entries, releases its consumed
count, exchanges the spill bitmap when the spill count moved, inspects, and
takes another pass when counts moved during the pass. The invariant forbids a
quiescent state, both sources done and no SGI pending, with a published slot
never inspected. Three negatives fail it: a source that sends no SGI, an owner
that releases its consumed count before reading the entries (the source
overwrites an unread entry), and a source that increments the spill count
before setting the bit. The model does not encode weak memory; the
implementation relies on release and acquire message passing only, with no
store-to-load ordering. `checks/scheduler-owner.py` runs the same protocol in
the owner code with concurrent threads.

`last_put_close` buffers the final reference decrement and admission close
independently. Both sides execute a store-to-load barrier before checking the
other word. The positive case requires at least one retirement notification.
The negative removes the last-put barrier and lets both callers miss the
notification, even though the closer retains its own barrier. This extends the
pin-versus-close lifetime check to the event that wakes the reclaimer.

`migration_ack` begins with one already-pinned publisher that sampled the old
route, plus the coordinator's retained reference. The publisher pauses before
hint publication. The old owner can detach and forward hints; the destination
ends forwarding only after publishers retire and the old owner acknowledges
its final drain. The negative lets the old owner remember an empty inbox while
that publisher still holds its reference. The destination can then end the
epoch with an unprocessed old-route hint. New route acquisition and weak-memory
ordering remain outside this small acknowledgment model.

`route_epoch` adds actual routing admission to that acknowledgment boundary.
One lifetime-pinned publisher can pause before registering in its sampled bank.
Two migrations close old banks, adopt destinations and reuse a bank without
resetting its participant count. A publisher gets one validation attempt;
rejection never publishes an old-route hint. The invariant forbids an accepted
hint for an already-retired epoch. Removing epoch validation admits a delayed
epoch-zero publisher after that bank reopens for epoch two. This finite model
uses sequentially consistent steps and does not prove AArch64 memory ordering.

The executable `checks/scheduler-route.py` checks the same stale-snapshot race
in actual control code, plus delayed admitted publication and deletion before
adoption. Its negative compiles an external copy with the validation clause
removed. The unchanged fixture must then detect stale route admission after
two transfers. The production object contains no pause hook or LL/SC loop.
