# Registered owners govern the return to the boot ROM

Status: **implemented and host-tested**. The registry covers every service grant,
manifest device class and card-backed storage declaration. Physical qualification
of this protocol is pending port recovery; [the evidence record](../evidence/failure-return.jsonl)
identifies the admission blocker. The earlier NPU failure-return result is recorded
in [the NPU evidence](../evidence/npu.jsonl); it does not qualify other owners.

A failure window starts when `failure_claim` publishes `failing` and ends at the
ROM return or the shared three-second limit. The application failure leaves
`board.fatal` clear. The supervisor requests bounded owner steps in order through
[the registry](../../apps/os/quiesce.c). Each step reports DONE, PENDING or FAULT.
Hardware operations execute on their existing authorized tasks.

DONE requires the owner's stop proof. PENDING holds the next order class. FAULT
records the error and permits later classes to run, while keeping terminal
release blocked. Faulted owners retain their resources. A completion publication
cannot overwrite FAULT. This distinction preserves DMA memory and storage when
cleanup cannot establish that the hardware stopped.

After DONE, the supervisor continues bounded observations of each stopped proof
until terminal release. A late NPU interrupt or worker fault revokes DONE through
the registered callback. Completed callbacks do not restart shutdown operations
or clock work. FAULT is final.

## Every granted owner has a verdict

Owners in the same class receive one collective request publication before any
of their tasks can observe it. This matters when radio and headset share a task.
Registrations precede service startup. Lazy storage startup also checks the
request before touching hardware and waits for concurrent mailbox publishers.

| Owner | Admission source and order | Return behavior | Verdict and remaining gap |
| --- | --- | --- | --- |
| NPU | NPU extent; application queues | Observes client retirement after the application's producer closes at a safe point when `ss_live()` becomes negative. Retained native callbacks park their task and publish FAULT. | DONE requires the stopped report and existing native retirement proof. An abandoned producer remains PENDING until the shared limit. |
| GPU | Manifest GPU device; application queues | Closes queue admission, finishes accepted work on the worker, acknowledges completed outputs and executes the existing CLOSE/PARK sequence. | DONE requires a suspended worker and the native stopped proof. Reserved, locked or lost ownership remains FAULT. Failure receipts distinguish abandoned completed outputs from successful application consumption. |
| USB host | Host board workload and manifest USB devices; device owners | Closes class admission and masks its interrupt on the host owner. | FAULT: the native host has no controller drain-and-release operation. An empty port does not prove controller retirement. Returning USB host sessions remain unqualified. |
| Radio | Radio grant and board workload; device owners | The radio owner advances the existing scan shutdown and wipes the credential. | DONE requires completed DOWN readbacks, no held or pending transport or PMIC request, and credential removal. Bootstrap, full network and entropy-only states report FAULT. |
| I2S audio | Audio grant; device owners | The stream owner cancels, drains, reaps and closes capture. | FAULT: local close retains the audio parent leases, and DMA pool retirement rejects streams. |
| Bluetooth headset | Headset grant; device owners | Requests segment stop and waits for disconnect while its module owner continues running. | FAULT: HCI/UART and standalone module ownership have no complete release API. Returning headset admission remains excluded. |
| SD storage | Card-backed large regions or the write-owner binding; device owners | Completes the current transfer, refuses new requests and closes on the original owner. A failed close retains descriptor ownership and is not retried. | Read-only DONE requires native close and descriptor release. Write-enabled volumes without application close report FAULT: completing CMD25 does not establish filesystem transaction consistency. |
| DMA | DMA extent; device owners | Stops admission under the sender reservation, then asks its worker to retire an idle queue through the existing mask and bootstrap proof. | DONE requires closed channels, no queued request or uncollected reply, and adopted-controller retirement. Active, abandoned, cold and stream states remain FAULT. |
| CPU clock | CPU clock grant; clock lowering | The thermal task finishes an active transition and lowers CPU-B through the existing boot-point return path. | DONE is published once. Subsequent passes perform no clock work; refusal publishes FAULT. |
| Thermal | Every OS session; thermal stop | Continues readings until earlier owners drain. A separately published authorization permits bounded interrupt masking and converter stop. | DONE requires converter stop. An earlier device FAULT keeps sampling active and prevents thermal retirement. |
| PMIC | Every returning OS session; PMIC stop | Its owner verifies idle slots, borrowed IDs and locks, publishes its result and parks. | DONE requires no held, retained or failed PMIC state. A refusal is recorded without closing board admission. |
| Per-core terminals | Every returning OS session; last | The supervisor releases the eight terminal tasks only after every earlier owner reports DONE. | FAULT never authorizes cache, RAM or power ownership release. The shared timeout retains the watchdog ending. |

The CPU clock operating points, voltage tables and device power sequences are
unchanged. A missing shutdown contract needs a source-backed design and its
required review before the owner can report DONE.

## Other actors preserve the failure window

- The claim, application tasks and application fault stubs publish the first
  failure once. Later application failures preserve it.
- The supervisor restarts the watchdog and enforces one three-second budget
  for the complete return. Session deadlines continue to bound execution.
- Thermal and device interrupt handlers retain their existing source masking
  and notification protocols. An interrupt that proves lost device ownership
  publishes the owner's fault; it does not certify drain.
- The returning profile leaves USB0 untouched. Its native retention callback
  still calls `board_fail`; admitting active USB0 requires a separate conversion.
- Unexpected interrupts, stack corruption, masked scheduler contexts and EL1/EL3
  exceptions retain the exceptional endings described in
  [the return design](failure-return.md). They are not device stop proofs.
- Resident liveness renewal and application algorithms retain their own lifetime
  rules. This registry runs only after the returning supervisor begins closing.

## Fault records identify owners without losing their original diagnostics

Each descriptor keeps its first site, value and caller in RAM, with release and
acquire publication. The supervisor reports these records even when an earlier
owner remains PENDING. It also flushes available records at the global timeout.

The RTC retains one secondary record after an application first record, at level
`0xc`. Its group is `QUIESCE`: bits 23:16 identify the registered owner, bits
27:24 identify the original failure group and bits 11:0 retain its detail or
source line. The value and caller retain their original meanings. The descriptor
keeps the full original site, including auxiliary bits. The
[decoder](../../tools/a733-crumbs.py) prints the owner, source group and code.

Additional owner records remain separate in RAM; the limited RTC slot keeps the
first reported secondary fault. A watchdog reset invalidates those RAM records.
The original application failure remains in the application result record when
the image returns normally. Non-application first records retain the existing
RTC replacement policy.

## Checks follow the session grants

`tools/session.py` derives `quiesce_actors` from the service declarations in
`apps/os/os-services.h`, the granted manifest device bits, workload and storage
binding. The builder still refuses compositions whose hardware facts are absent.
Synthetic host cases exercise those declarations without granting physical
launch authority.

`checks/os-failure-step.py` requires a registration for every derived actor and
executes the production registry. It checks ordered progress, collective request
publication, completion races, fault retention, later faults behind a pending
owner and the shared timeout. Removing each registration or inserting
`platform_exit` into each registered callback must fail the check. The source
scan covers direct callbacks; it is not a complete C call-graph proof.

The owner checks (`checks/os-quiesce-*.py`) exercise native adapters or extracted
production steps with modeled hardware. GPU and DMA checks execute their actual
worker retirement paths under QEMU. The linked failure-return fixture executes
the image's claim, registry, supervisor and terminal code, with modeled hardware
and owner publications. QEMU OS boot covers startup, application faults and the
storage task composition. None of these host results establishes physical drain.
