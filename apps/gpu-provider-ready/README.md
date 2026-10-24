# Returning GPU provider-readiness checkpoint

This checkpoint runs the GPU diagnostic's actual constructors and shared PMIC
service, verifies both boot supplies, parks the service, and returns before any
GPU power request. It is implemented and checked offline. Physical execution
requires fresh coordinator admission after board recovery.

The earlier [power and identity diagnostic](../gpu-power-id/README.md) did not
return on its physical attempt. Its uploaded binary matched the frozen image;
no native phase report was available. A separate instruction model reached
scheduler start through its actual constructors. Neither observation identifies
the physical failure location. The failed image and evidence remain immutable.

## Completion proves readiness without claiming GPU power state

The constructor sequence is the same as the power/identity diagnostic: platform
initialization, shared provider/task construction, GPU native/platform setup,
PMIC identity, and consumer/domain-5 row registration. The row is never requested.
The client calls no GPU power, identity, firmware, submission or shutdown API.

An idle registered row causes the shared service to verify DCDC2 and DCDC4.
The checkpoint requires PROVIDER_READY, SUPPLY_DONE, DCDC2_VERIFIED and
DCDC4_VERIFIED, with no authority fault. SUPPLY_DONE alone is insufficient:
the shared owner also sets it when verification fails.

After the real provider parks, the client requires:

- GPU platform/controller NEW, no token, delegation or attempted lease, and an
  untouched supply consumer with no rail or PMIC request.
- One IDLE domain-5 row with request zero and no count; every domain remains
  UNKNOWN with no users, and the domain access/write trace remains empty.
- Every GPU resource slot free, with no token ever issued by its manager.
- Empty rail holder, borrow, pin and sharer accounting, plus empty PMIC request
  slots and no outstanding supply request.

The service's old supply-token value can remain as an immutable handle after
release; the ledger proves that it owns nothing. Service idle alone would not
prove this release. The checkpoint checks the ledger after park, then samples
the monotonic clock again before printing its distinct completion line:

```text
COMPLETE GPU provider readiness: supplies verified, parked, no GPU power request
```

The owner skips IDLE rows whose request and answer generations are both zero.
It does not poll UNKNOWN domains.
Consequently, this path performs no GPU or PCK register read or write. Constructor
translation checks for PCK pages are architectural address translations, not
loads from those pages. Domain UNKNOWN is software state; it does not claim
that inherited GPU_TOP or NPU power is OFF. The code does not observe or change
those hardware states.

PMIC startup still uses its qualified S_TWI0 and PL0/PL1 ownership. Supply
verification reads both rails without changing regulator settings. CPU startup
and return retain their counter, memory, timer and GIC accesses. The native
service deadline is 1.5 seconds from monitor entry. Pending operations observe
that deadline when their one-tick wait resumes. Failed or missing completion
retains ownership for coordinator recovery, even if a CPU error report returns.

## The new image includes the accepted monitor cleanup fix

The build reuses 35 objects from the failed diagnostic and rebuilds only the
consumer and `platform/a733/monitor.c`. The monitor source and boot ABI must
match the accepted restoration-readback evidence. It uses the original qualified
monitor flags, including `-mgeneral-regs-only`.

The updated monitor checks restored GIC state within the existing three
100-millisecond cleanup windows. Failed restoration produces status 6 rather
than success and retains interrupt ownership. Its 32 accepted instruction cases
are reused without rerunning them. The earlier failed diagnostic used the older
monitor object and did not have these readback checks. Including the fix improves
this checkpoint's return proof; it does not identify the previous failure.

## Focused evidence covers the new boundary

```sh
~/.cache/sunstoneos/venv/bin/python apps/gpu-provider-ready/build.py --output /absolute/new/image
~/.cache/sunstoneos/venv/bin/python apps/gpu-provider-ready/check.py \
  --image /absolute/new/image/image.elf --output /absolute/new/client-results.json
```

The instruction fixture executes the linked constructors through scheduler
start, with architectural reads and translations modeled. It then executes the
actual client and readiness proofs with modeled service progress and park.
Only image/result RAM is mapped; any device MMIO would stop the fixture.
Eleven client cases cover success, each missing supply-verification bit, a used
request or token, a recorded domain access, retained rail accounting, a request
appearing during park, failed park, deadline equality and backward time.

The separate `owner-check.c` uses the existing PMIC/owner model and actual shared
driver source. With initial GPU_TOP OFF and ON, both cases verify both supplies,
release every temporary rail/borrow, and record zero domain or CCU-level accesses.
The retained NPU state and PMIC settings remain unchanged. These are model
observations, not measurements of physical power or service latency.

The client adds two clock samples and bounded scans of existing fixed-size
accounting arrays. No new waits, scheduler framework or recovery mechanism is
introduced. The existing provider park remains the only stop operation.

## Physical admission needs recovered CPU and PMIC ownership

The coordinator must establish current ROM identity, DDR/CPU/GIC continuity,
cache-off entry, and authority over `[0x41000000, 0x42200000)` for the returning
platform. PMIC admission needs the inherited S_TWI0 gate/reset, APBS route,
PL0/PL1 configuration, stable supplies and excluded writers. Both DCDC2 and
DCDC4 must be observed enabled at 800 mV for this checkpoint to report readiness.

The checkpoint needs no GPU parent-rate observation, GPU register access grant,
or inherited domain-OFF predicate. Other device state remains under coordinator
authority; the fresh ledger does not import or prove those devices' leases.
No external writer may interfere with CPU RAM, shared PMIC operation or return.
Old powered-lifetime facts do not survive a board power recovery automatically.

A successful return would establish this checkpoint's provider boundary and
CPU return. A nonreturn would still leave the checkpoint's own return path
unresolved. Neither result would by itself qualify GPU power, identity, firmware
or computation.
