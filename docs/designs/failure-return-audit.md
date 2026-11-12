# Registered owners govern the return to the boot ROM

Status: **implemented and host-tested**. Thirteen registered owner identities cover
session grants, manifest device classes and card-backed storage declarations.
Selected GPU, NPU and storage-read failures return to FEL without a watchdog
reset; [the failure-return record](../evidence/failure-return.jsonl) defines their
physical scope. USB and radio failure-injection stages await reviewed USB launch
admission facts. Audio and headset accessory-dependent qualification remains open.

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
| NPU | NPU extent; application queues | Observes client retirement after the application's producer closes at a safe point when `ss_live()` becomes negative. Retained native callbacks park their task and publish FAULT. | DONE requires the stopped report and native retirement proof. Forced recognition failure with an open queue is board-qualified. An abandoned producer remains PENDING until the shared limit. |
| GPU | Manifest GPU device; application queues | Closes queue admission, finishes accepted work on the worker, acknowledges completed outputs and executes CLOSE/PARK. | DONE requires a suspended worker and native stopped proof. The selected compute failure is board-qualified, including its abandoned output receipt. Reserved, locked or lost ownership remains FAULT. |
| USB host | Host board workload and USB host class grants; device owners | Closes class admission, drains and halts xHCI, retires its interrupt, revokes stopped transport loans, restores the local platform and waits for Type-C source stop. | Host-tested DONE requires every stop proof. Active borrowed JPEG work or uncertain controller, interrupt or source state remains FAULT. An empty-port diagnostic stops before xHCI starts and cannot qualify controller drain. |
| USB0 device | USB0 grant from manifest `usb-device`; device owners | Atomically seals startup before inspecting the original provider state. Stop guards and the provider failure hook record their reason and park during a return window. | Host-tested DONE requires no startup, token, PMIC request, provider fault or controller access. Any claimed startup or uncertain state remains FAULT with resources retained. Returning USB0 grants remain excluded; enabling them requires earlier registration and a proven native close path. |
| Radio | Radio grant and board workload; device owners | Drains the accepted SDIO operation, stops partial startup or a scan through DOWN readbacks, and erases permitted credential copies. Shared Bluetooth must release the module before physical shutdown. | Host-tested DONE covers partial startup and scan-only shutdown with no outstanding transport, PMIC request or credential. Full station, association and entropy-only ownership retain FAULT value `0x905`: crypto-engine (CE) retirement is unproved. |
| I2S audio | Audio grant; device owners | Returns borrowed periods, cancels, drains, reaps and closes capture; retires the DMA worker and interrupt; verifies the controller; then retires local and shared audio leases. | Host-tested DONE covers partial startup and complete capture. Uncertain drain or a conflicting shared clock consumer retains FAULT. Returning audio admission remains excluded pending qualification. |
| Bluetooth headset | Headset grant; device owners | Closes capture and protocol channels, completes HCI Reset, waits for the module owner to assert and verify PM2 reset, then drains and closes UART. | Host-tested DONE requires every protocol, peer-stop and UART proof. Profile version 4 supplies explicit PM2 authority; legacy profiles retain FAULT. Returning headset admission remains excluded pending physical qualification, including the reset route and an attached headset. |
| SD storage | Card-backed large regions or the write-owner binding; device owners | Finishes the issued transfer, refuses further requests and uses its own transaction record to decide whether native close is safe. | DONE covers reads, untouched write sessions and completed consistent transactions. Unsafe phases immediately publish FAULT with the session ID and detail `0x921` through `0x92c`. Failure after a verified 512 MiB read is board-qualified; interrupted filesystem writes are not. |
| DMA | DMA extent; device owners | Stops admission under the sender reservation and retires the idle worker queue, interrupt and controller through read-only final verification. | Host-tested DONE requires closed channels, no queued request or uncollected reply, and the admitted cold or adopted controller state. Closed streams are supported. Device memory and its platform reservation remain held through FEL. Active or abandoned work retains FAULT. |
| CPU clock | CPU clock grant; clock lowering | The thermal task finishes an active transition and lowers CPU-B through the boot-point return path. | DONE is published once. Subsequent passes perform no clock work; refusal publishes FAULT. |
| Thermal | Every OS session; thermal stop | Continues readings until earlier owners drain. A separately published authorization permits bounded interrupt masking and converter stop. | DONE requires converter stop. An earlier device FAULT keeps sampling active and prevents thermal retirement. |
| PMIC | Every returning OS session; PMIC stop | Its owner verifies idle slots, borrowed IDs and locks, publishes its result and parks. | DONE requires no held, retained or failed PMIC state. A refusal is recorded without closing board admission. |
| Per-core terminals | Every returning OS session; last | The supervisor releases the eight terminal tasks only after every earlier owner reports DONE. | FAULT never authorizes cache, RAM or power ownership release. The shared timeout retains the watchdog ending. |

The six order classes are queues, device owners, CPU clock, thermal, PMIC and
terminals. Native hardware remains on its authorized owner task. The USB, radio,
audio, headset and storage retirement changes have design and code review for
simplicity, hardware performance and correctness. Their host evidence does not qualify
physical timing or fault recovery.

The supervisor publishes one absolute deadline before requesting any owner. It
allows at most three seconds and preserves the session's final 10 ms guard.
Shared lock waiters recheck that deadline on each existing wait tick and after
acquisition; shared pin callbacks refuse register access after expiry. A startup
task that sees closing before the deadline is published yields without starting
more hardware work. Normal transaction deadlines remain in force when earlier.

## Physical qualification

[The failure-return records](../evidence/failure-return.jsonl) qualify three
selected failures on port 3-3: GPU compute, NPU recognition with an open queue, and
failure after reading and hashing 512 MiB from storage. Each preserves its
expected application first record, reports no secondary owner fault, retires all
eight cores and returns to FEL without a watchdog reset. These cases establish
healthy-device retirement after application failure. They do not establish
recovery from a device fault or an interrupted filesystem write.

USB and radio failure-injection stages await reviewed USB launch admission facts.
The USB empty-port diagnostic can exercise partial startup and Type-C stop only;
started-xHCI retirement needs an admitted attachment. Audio and headset require
accessories for their external capture paths. Headset return also needs physical
version 4 reset authority ([Bluetooth return](bluetooth-return.md)). The pre-launch
host refusal in the record contains no native execution and supplies no
device-failure verdict.

## Other actors preserve the failure window

- The claim, application tasks and application fault stubs publish the first
  failure once. `ss_fail` and `ss_fail_value` use one failure-record path even
  when a return claim is refused; their fallback does not add a board note.
  Later application failures preserve the first record.
- The supervisor restarts the watchdog and enforces one three-second budget
  for the complete return. Session deadlines continue to bound execution.
- Thermal and device interrupt handlers retain their existing source masking
  and notification protocols. An interrupt that proves lost device ownership
  publishes the owner's fault; it does not certify drain.
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
`0xc`. Its group is `QUIESCE` (`0xc`); the separate USB0 guard group is `0xb`.
Bits 23:16 identify the registered owner. Bits 27:24 identify the original
failure group, and bits 11:0 retain its detail or
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
binding. The manifest class `usb-device` selects USB0; USB host classes select
the separate USB host owner. The builder still refuses compositions whose
hardware facts are absent.
Synthetic host cases exercise those declarations without granting physical
launch authority.

`checks/os-failure-step.py` requires a registration for every derived actor and
executes the production registry. It checks ordered progress, collective request
publication, completion races, fault retention, later faults behind a pending
owner and the shared timeout. Removing each registration or inserting
`platform_exit` into each registered callback must fail the check. The source
scan covers direct callbacks; it is not a complete C call-graph proof.

The owner checks (`checks/os-quiesce-*.py`) exercise native adapters or extracted
production steps with modeled hardware. The USB0 check uses the actual provider
layout and registry. It tests both orders of starting and sealing the owner,
and protects provider memory against access after startup wins. GPU and DMA checks execute their actual
worker retirement paths under QEMU. The linked failure-return fixture executes
the image's claim, registry, supervisor and terminal code, with modeled hardware
and owner publications. QEMU OS boot covers startup, application faults and the
storage task composition. None of these host results establishes physical drain.
