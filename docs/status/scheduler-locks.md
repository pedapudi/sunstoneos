# FIFO kernel locks and interrupt-mask attribution

**Status: implemented.**

**Board-qualified scope:** Ticket locks and paired interrupt-mask attribution have
no physical qualification. Port 3-3 is paused after entry
`1793572859635159128-187e5901`; its pause record requires recovery review.
Neither comparison image is submitted. The existing global-kernel baseline,
entry `1793566204991504422-8a47d893`, exceeds the 25 microsecond lock-wait and
masking budgets. It does not establish ticket-lock performance.

## FIFO admission preserves recursive kernel locking

Both global FreeRTOS locks use one 32-bit ticket word, with owner in the low
16 bits and next ticket in the high 16 bits. Each lock occupies a separate
64-byte cache line. Recursive acquisition reserves no ticket and retains the
existing ownership and recursion assertions.

A core with known Arm Large System Extensions (LSE) support reserves its ticket
with `LDADDA`. Privileged
code reads that core's atomic-instruction capability once. EL0 callers use
`LDAXR`/`STXR` until the capability is known. Contended acquisition waits with
`WFE` and `LDAXRH`; the exclusive monitor prevents a lost release event.
Release increments only owner with `STLRH`, preserving concurrent ticket
reservations and 16-bit wraparound.

FIFO order bounds overtaking after ticket allocation. With eight contenders,
waiting can still include seven complete preceding holds. This does not imply
that the existing 25 microsecond budget is met. LL/SC ticket reservation can
retry under contention; it has no separate hard progress bound.

## The latency recorder retains paired return addresses

The existing recorder supplies initialization, idle and loaded distributions,
acquisition counts and raw wake samples. Each core also retains its three
longest nonzero interrupt-mask sections and task-lock holds, separately for
each phase and for all intervals combined. Each entry contains duration,
start counter, entry return address and exit return address. The context-restore
exit uses its assembly instruction address. A macro's return address identifies
the caller of its enclosing function; it is not a stack trace.

Tables use fixed storage, bounded replacement and no allocation. Only frozen
tables are sorted and copied. The complete application report is 63,872 bytes,
within the existing 65,536-byte output limit. The decoder rejects malformed
ordering, inactive entries with context, missing sites and phase crossings.

Nearest-rank p50 and p99 are conservative histogram upper endpoints, with
sixteen bins per octave and at most 6.25 percent width. Maxima are exact.
Phase distributions include only wholly contained intervals. Closing a phase
can omit a boundary sample; aggregate records retain intervals across phases.
Histogram and attribution lookups can observe different sides of that boundary.

Enabled measurements include acquisition-recorder work performed while a lock
is held. Mask and hold endpoints precede close-recorder bookkeeping and the
unmask or unlock tail. These intervals expose overruns but cannot establish a
complete duration bound. Instrumentation-disabled OS executable bytes,
addresses, writable storage and object layout match the ticket-lock control.

## Host evidence establishes ordering and report integrity

Evidence resides under `~/.cache/sunstoneos/build/sched-lock-1101/`.

- `final-lock-check/result.json`: 19,447 three-core states, 262,144 wraparound
  arithmetic cases, monitored-wait races and a test-and-set starvation control.
  Seven linked FreeRTOS QEMU cases cover A53 fallback, A55 capability admission,
  A76 LSE, task migration and eight-core queue soak. The model assumes coherent
  acquire/release memory; it does not qualify physical timing.
- `recorder-final.log` and `final-latency-model/result.json`: nested attribution,
  recursion, bounded retention, freeze behavior and the shared eight-core
  latency workload. QEMU timing is host evidence only.
- `report-check/` and the native producer objects: CPU and DMA DWARF layouts,
  complete word expectations and decoder corruption checks pass.
- `review-evidence/off-comparison-final/comparison.json`: disabled recorder
  executable bytes, full disassembly, writable storage and allocated-object
  layout match; no latency symbols remain.

Independent source review accepts the lock, recursion instrumentation and
report layout. Integration results are recorded in the gate evidence.

## Physical comparison requires recovery of port 3-3

The CPU control profile uses the baseline's workload, top CPU operating points,
voltages and session-template inputs. `SUNSTONE_LOCK_TAS_CONTROL=1` selects the
original lock only in recorder builds; the default uses tickets. The two built
images differ in that selection and their build identity. Their build
directories are `old-v2/` and `ticket-v2/` beneath the evidence root.

The complete comparison expectation preserves report coverage and metadata,
while allowing measured durations and first-over-budget provenance. Timing
budget failures remain measurements to report, rather than acceptance claims.
No image is retried and no paused-port recovery is attempted by this stream.

After coordinator recovery, submit the attribution control, then ticket locks,
through the queue on port 3-3. Record per-core loaded wake, lock wait, lock hold
and masking p50/p99/maximum, then resolve retained addresses against each exact
ELF. Dominant sites and any driver changes remain unknown until those runs.
