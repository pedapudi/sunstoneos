# FIFO kernel locks and interrupt-mask attribution

**Status: implemented.**

**Board-qualified scope:** Three eight-core CPU latency images on port 3-1
return normally with CPU rails restored: test-and-set, tickets, and tickets
with the thermal fast path. The matched lock pair reduces worst loaded wait
from 137.625 to 29.625 microseconds and masking from 127.667 to 33.792.
The thermal follow-up records 32.500 microseconds wait and 38.458 masking;
its unexplained tail regressions block performance acceptance and merge.
The 25 microsecond budget remains unmet. These finite runs do not qualify
worst-case timing, sustained operation or power behavior.

## FIFO admission preserves recursive kernel locking

Both global FreeRTOS locks use one 32-bit ticket word, with owner in the low
16 bits and next ticket in the high 16 bits. Each lock occupies a separate
64-byte cache line. Recursive acquisition reserves no ticket and retains the
existing ownership and recursion assertions.

A core with known Arm Large System Extensions (LSE) support reserves its ticket
with `LDADDA`. Privileged code reads that core's atomic-instruction capability
once. EL0 callers use
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
  arithmetic cases and monitored-wait races.
  Seven linked FreeRTOS QEMU cases cover A53 fallback, A55 capability admission,
  A76 LSE, task migration and eight-core queue soak. The model assumes coherent
  acquire/release memory; it does not qualify physical timing.
- `fairness-control/result.json`: executable test-and-set transitions generate
  three overtakes while the waiting core continues trying. The expected
  two-overtake bound for three cores fails. This strengthens the negative
  control without changing the kernel or its linked-test evidence.
- `recorder-final.log` and `final-latency-model/result.json`: nested attribution,
  recursion, bounded retention, freeze behavior and the shared eight-core
  latency workload. QEMU timing is host evidence only.
- `report-check/` and the native producer objects: CPU and DMA DWARF layouts,
  complete word expectations and decoder corruption checks pass.
- `review-evidence/off-comparison-final/comparison.json`: disabled recorder
  executable bytes, full disassembly, writable storage and allocated-object
  layout match; no latency symbols remain.

Independent review accepts the lock, recursion instrumentation, report layout
and thermal fast-path correctness. The full integration gate
`sched-lock-1101-matched-full` passes on `fe5bf23b` with the recorded status
draft, including every central target, configured OS variant and indexed host
check. The supervised, resident and resident-audio baseline builds, kernel
check, eight-core soak, progress check and check index pass. Gate evidence is
`integration/gate-sched-lock-1101-matched-full/` beneath the evidence root.
Checks needing external inputs remain listed as pending; they are not claimed
as executed. Host success does not waive the physical timing regression.

## Matched port 3-1 runs reduce tails and increase some medians

The test-and-set control is entry `1793575947324491365-4f50a160`; ticket locks
are entry `1793576137408460593-a3232b16`. Both use the same chip, workload,
recorder, comparison expectation and baseline session template, with rail
restoration enabled. A55 cores 0–5 run at 1,794 MHz and 1,000 mV; A76 cores
6–7 run at 2,002 MHz and 1,050 mV. The earlier port 3-3 baseline is a different
chip and supplies no numerical comparison here.

Each cell below gives test-and-set → tickets, in microseconds, with
p50/p99/maximum within each triple. Percentiles are histogram upper endpoints;
they can exceed the exact maximum. Cores are not pooled.

| Core | Loaded wake | Task-lock wait | Task-lock hold | Interrupt masking |
| ---: | --- | --- | --- | --- |
| 0 | 74.625/133.292/134.000 → 101.292/117.292/126.083 | 6.958/53.292/77.542 → 20.625/27.958/28.917 | 1.875/7.625/11.708 → 2.458/7.292/8.792 | 3.792/39.958/83.583 → 3.792/29.292/32.667 |
| 1 | 47.958/95.958/144.292 → 50.625/61.292/66.917 | 7.625/71.958/104.500 → 15.292/26.625/28.667 | 4.125/12.625/14.167 → 3.792/13.292/18.125 | 7.292/69.292/110.833 → 11.292/29.292/31.333 |
| 2 | 69.292/138.625/174.125 → 55.958/63.958/63.333 | 14.625/79.958/112.292 → 17.292/27.958/29.625 | 3.625/11.292/12.917 → 3.625/11.958/15.708 | 7.625/71.958/117.792 → 17.292/30.625/32.208 |
| 3 | 69.292/149.292/150.833 → 55.958/63.958/66.375 | 16.625/95.958/107.708 → 19.292/27.958/29.125 | 3.625/12.625/15.750 → 2.458/8.625/9.625 | 8.625/69.292/112.167 → 18.625/30.625/31.792 |
| 4 | 63.958/154.625/180.167 → 53.292/61.292/59.542 | 14.625/79.958/124.125 → 19.958/27.958/28.167 | 3.458/10.292/11.833 → 2.625/4.958/7.625 | 6.625/82.625/127.667 → 19.958/30.625/32.667 |
| 5 | 55.958/149.292/158.583 → 53.292/58.625/59.875 | 14.625/90.625/100.708 → 19.958/26.625/27.125 | 3.625/11.958/18.167 → 3.125/6.625/12.792 | 6.625/74.625/103.500 → 18.625/30.625/33.333 |
| 6 | 77.292/159.958/165.917 → 47.958/55.958/54.250 | 7.292/117.292/137.625 → 17.958/26.625/27.583 | 3.125/8.958/12.042 → 2.958/7.625/9.375 | 3.792/77.292/106.167 → 10.292/29.292/31.042 |
| 7 | 58.625/138.625/136.417 → 47.958/53.292/51.708 | 15.292/90.625/100.083 → 20.625/29.292/28.917 | 2.958/9.292/10.833 → 2.458/7.292/7.833 | 5.625/85.292/103.083 → 19.292/30.625/33.792 |

Worst loaded wake maximum falls from 180.167 to 126.083 microseconds.
Task-lock wait p50 upper endpoints rise on every core. CPU0's wake p50 upper
endpoint rises from 74.625 to 101.292 microseconds, while its p99 upper
endpoint falls from 133.292 to 117.292.
This experiment supports reduced contention tails, with an explicit median
tradeoff. One run per condition does not establish repeatability.

The ELF and output hashes pin the measured bytes:

| Locks | Image SHA-256 | Output SHA-256 |
| --- | --- | --- |
| Test-and-set | `5f9682c532863908c553c6f0d0e73f3216ac71cdd935160349af44aa9c31bcfc` | `5d0a31af710553feedb42eecaa6ac73afb59acd1c993a03cb7624f56514096b4` |
| Tickets | `01b015b859b88caa14a2fe9bfbf2c8e3b0ac4d726f589e15277531e31766faac` | `6f90f170ed606cfa26900dbfc74542f4b11013ebe0486cead73f1ead98ad0f41` |

The coordinator builds the paired images from source integration `e74bc278`.
Their run records identify launch-tool revision `a96c8c1a`. Each saved local build ELF matches
its queue entry's `run/image.elf` hash.

`followup/compare.py` decodes the raw payloads, checks their authoritative
hashes and resolves both addresses against each exact frozen ELF. Decoded
records, both address and preceding-call source mappings, and the full site
inventory reside in `followup/`. Assembly context-restore exits use instruction
addresses; C call sites use the preceding AArch64 instruction.

## Retained sites identify scheduler blocking and thermal rearm

The table lists distinct entry/exit pairs among the retained loaded sections.
Times are the largest retained duration for that pair. Site percentiles are
unavailable.
Generated `freertos-a733-smp-cached/tasks.c` contains the FreeRTOS scheduler;
`portASM.S` is the instrumented SMP port.

| Kind | Entry → exit symbol | Test-and-set µs | Tickets µs |
| --- | --- | ---: | ---: |
| Mask | `ulTaskGenericNotifyTake` → `ulTaskGenericNotifyTake` | 127.667 | — |
| Mask | `vTaskSuspendAll` → `ulTaskGenericNotifyTake` | 117.792 | — |
| Mask | `ulTaskGenericNotifyTake` / `vTaskYieldWithinAPI` → `FreeRTOS_Yield` | 104.375 | 33.792 |
| Mask | `apps/os/thermal.c:rearm` → `rearm` | 107.750 | 31.000 |
| Task-lock hold | `vTaskSuspendAll` → `vTaskExitCritical` | 18.167 | 18.125 |
| Task-lock hold | `vTaskSwitchContext` → `FreeRTOS_IRQ_Handler` | 14.167 | 9.625 |
| Task-lock hold | `vTaskSwitchContext` → `FreeRTOS_Yield` | 11.833 | 8.792 |
| Task-lock hold | `vTaskEnterCritical` → `vTaskExitCritical` | — | 8.708 |

A dash marks absence from the retained loaded table. That path can still
execute in the recording window. Each image retains 24 loaded mask samples: 23 scheduler
samples and one thermal sample. Retention does not measure site frequency.
Initialization maximum masking is 264.833 microseconds for test-and-set and
266.333 for tickets, at `prvAddNewTaskToReadyList` → `vPortRestoreTaskContext`.
These initialization intervals are outside the loaded table.

Scheduler notification and suspension changes require broader ownership work.
They remain with the partitioned-scheduler stream; this branch does not change
`freertos-smp-tasks.patch` or owner run queues.

## Already-armed thermal rearm avoids a global critical section

`apps/os/thermal.c:rearm` checks an atomic masked flag before entering the
kernel critical section. An already-armed interrupt returns without acquiring
the global locks or accessing the GIC. Actual rearm retains the same task/ISR
critical section and its pending/masked recheck. The GIC helper already issues
one `GICD_CTLR.RWP` read per attempt; it has no polling loop to move.

An IRQ after a false snapshot leaves pending work for the next owner pass,
just as an IRQ after the former critical section exits. A masked flag set by
an ISR before hardware disable cannot race actual rearm, which still acquires
the same kernel exclusion. This is a local no-op optimization; IRQ routing,
power policy, clocks and voltages retain the matched profile.

The actual AArch64 thermal-source fixture passes 20 cases in
`followup/thermal-cadence-v2/`. Cases cover repeated already-armed passes with
zero extra lock/GIC calls, an IRQ after that pass, pending IRQ consumption,
deferred GIC completion and an IRQ winning before the protected recheck.
The focused normal OS baseline build passes in `followup/baseline/`.

The single follow-up is entry `1793576768527805851-5709adc9`, port 3-1,
using the same expectation, frozen template and rail restoration. It returns
normally with restored rails. Source revision is `fe5bf23b`; the runner records
`a96c8c1a`. Image SHA-256 is
`2c928eebe1203b3b9d464b343d67819a604a4ce3dae628b7977ea7d538e10227`;
output SHA-256 is
`22bcc3b590c70003d12b31f683557d490a55c3103f8e928810492a362c69c08e`.

Loaded acquisitions, including recursion, fall from 7,253 to 7,124 for the
task lock and from 8,402 to 8,275 for the ISR lock. Loaded windows are
128,089.167 and 128,107.333 microseconds respectively. All 24 retained loaded
mask sections now resolve to notification/yield paths; thermal rearm is absent
from that bounded table. The fixture establishes that an already-armed pass
avoids locking. These capture counts alone do not measure thermal frequency,
per-operation CPU cost, throughput or power.

Each follow-up cell below gives p50/p99/maximum in microseconds, with the same
histogram and recorder limitations as the matched-pair table.

| Core | Loaded wake | Task-lock wait | Task-lock hold | Interrupt masking |
| ---: | --- | --- | --- | --- |
| 0 | 95.958/117.292/116.667 | 21.292/27.958/30.917 | 2.375/7.292/9.542 | 3.958/30.625/38.458 |
| 1 | 50.625/63.958/62.708 | 16.625/27.958/28.542 | 4.458/12.625/16.125 | 16.625/30.625/31.250 |
| 2 | 53.292/69.292/68.667 | 17.292/27.958/28.542 | 3.792/11.958/16.875 | 17.292/30.625/32.292 |
| 3 | 55.958/61.292/66.708 | 19.292/27.958/29.000 | 2.542/8.625/11.750 | 18.625/30.625/31.417 |
| 4 | 53.292/61.292/62.792 | 19.958/26.625/27.417 | 2.625/4.792/5.625 | 19.292/30.625/31.292 |
| 5 | 50.625/58.625/56.208 | 19.958/26.625/27.750 | 3.125/6.292/10.875 | 18.625/33.292/34.625 |
| 6 | 47.958/53.292/52.917 | 17.958/27.958/32.500 | 2.792/7.958/12.500 | 6.625/30.625/37.417 |
| 7 | 47.958/53.292/52.792 | 19.958/29.292/31.667 | 2.375/6.625/7.542 | 18.625/34.625/36.792 |

The worst wake maximum falls from 126.083 to 116.667 microseconds, while
worst task-lock wait rises from 29.625 to 32.500. The largest per-core masking
p99 upper endpoint rises from 30.625 to 34.625; masking maximum rises from
33.792 to 38.458. The masking and wait regressions are unexplained by this
single window. They cannot be dismissed as variation or accepted as a timing
improvement. Performance acceptance and merge remain blocked.

The largest follow-up mask still begins in `ulTaskGenericNotifyTake` /
`vTaskYieldWithinAPI` and ends in `FreeRTOS_Yield`. The largest task-lock hold
is 16.875 microseconds at `vTaskSwitchContext` → `FreeRTOS_IRQ_Handler`;
`vTaskSuspendAll` → `vTaskExitCritical` reaches 15.500. Further work requires
scheduler-level diagnosis beyond this bounded thermal change. No further image
is submitted in this scope.

`followup/thermal-decoded.json` retains the exact frozen-ELF mappings;
`followup/compare-thermal.py` reproduces the additional table. Independent raw
decode, hash, symbol and quantitative reviews reside in
`followup/review-evidence/`.
