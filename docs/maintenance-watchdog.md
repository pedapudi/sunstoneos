# Storage maintenance requires an independently armed watchdog

The maintenance supervisor bounds native USB storage sessions with the CPUX
hardware watchdog. It adopts an already armed timer and permits a reload only
after useful storage work and a host exchange complete. Physical timer reload,
whole-system reset, ROM return and recovery from a stalled interconnect remain
unqualified. The implementation does not authorize a physical storage launch.

The dedicated maintenance launcher must transfer its immutable session identity,
storage grant, initial reload observations and qualified timing bounds. Existing
diagnostic contracts do not provide this transfer. Their cached runtime exits at
its diagnostic deadline even if another component feeds the watchdog.

## One storage grant defines the session

The transfer in `include/a733_maintenance_watchdog.h` references the same immutable
storage grant that authorizes the card range and host nonce. It also binds the
launch epoch, generation, lease, executable digest and recovery evidence digest.
The recovery digest must equal the storage grant's recovery digest. Digests
identify artifacts; trusted launch admission must establish their applicability.

Times are absolute microseconds from the shared 24 MHz physical counter. The
launcher records counter observations before and after its initial restart;
their converted values must be ordered and at most one millisecond apart.
The qualified minimum interval bounds the earliest possible watchdog expiry.
The maximum recovery interval includes the required reset and ROM-return bound.
Neither interval has a built-in default.

The storage cutoff must precede the absolute recovery deadline by at least the
maximum recovery interval. The supervisor reserves one second for a storage
operation, plus the qualified guard, before the earliest watchdog expiry.
It also reserves the qualified guard before the storage cutoff, so a committed
reload can complete by that cutoff. It refuses activity at the earlier bound.
A reload cannot extend the immutable session cutoff.

The native singleton binds the actual storage service, USB transport, shared
counter, shared register mutex and distinct storage/USB task handles before the
scheduler starts. Admission checks EL1, Device identity mappings, watchdog mode,
reset policy, output pulse, RTC clock configuration and ROM redirect state.
It does not arm, reload or disarm the timer during binding.
Revocation before or during binding is permanent. Atomic admission transitions
cannot replace that revocation with a live lease.

## A verified chunk and host response permit one reload

Only the storage task calls `a733_maintenance_storage_observe`. The function reads
the actual service after each bounded worker step. It validates the copied
grant, operation deadline, card ownership and completed BACKUP or WRITE counters.
A WRITE completion includes the service's actual readback verification.

The USB task calls `a733_maintenance_poll` after publishing transport callbacks.
It reads atomic records rather than accessing the service concurrently. A reload
requires all of these conditions:

- A newly verified storage chunk has a retained completion record.
- Its USB IN response has actually completed.
- The immediately following ordered OUT request has arrived.
- The storage task has validated and admitted that next request.

Each chunk permits at most one reload. Two completion records cover a fast next
chunk finishing before the USB task observes the preceding response. There is
no general progress history. Atomic version and field operations use sequential
consistency, so a reader rejects an observation interrupted by publication.

INFO, PMIC activity, polling and elapsed timestamps never create a permit.
Initialization must finish inside the initial transferred interval. A stalled
host, including one blocked in a durable file write, receives no keepalive.
An active request has a fixed deadline of at most one second. A late worker or
USB observation revokes feed and retains ownership.

FINISH permanently disables further reloads while allowing its final response
to drain inside the interval already earned. Loss, provider failure, deadline
expiry and an invalid owner permanently revoke the lease. None of these outcomes
quiesces DMA, cancels a media write or releases retained storage resources.

## Only the committed reload may write hardware

The native adapter uses the common register mutex. It checks the watchdog,
source and redirect configuration before committing a reload, then writes
`0x14af` to CPUX watchdog control at `0x0205000c` with ordered Device access.
It checks the configuration and counter again afterward. An uncertain write
retains the register mutex and revokes future feeds.

The atomic commit establishes ownership of one bus transaction. Concurrent
revocation cannot retract that transaction; it prevents subsequent reloads.
The platform clock bounds normal execution and mutex waits. It cannot recover
a CPU or interconnect stall. Independent hardware expiry must provide that
recovery under the admitted physical qualification.

The A733 manual describes the restart register as a keyed write-one action.
It exposes no readable countdown or reload acknowledgment. Matching register
readback does not prove that a restart occurred. The qualified timer bounds
remain an external prerequisite even when all configuration checks pass.

The admitted executable must have one watchdog writer and preserve RTC source
selection and CPU0 ROM redirect ownership until reset. All platform participants
share the register mutex. The supervisor does not grant other RTC or legacy
watchdog writers permission to change those resources.

Admission and every reload compare the RTC control word at `0x07090000` with
the recorded value except bits 8:7. Those bits report a pending calendar
day or time write and remain set while an RTC owner's write settles; they do
not select the low-speed or watchdog clock path. A calendar write that overlaps
a reload therefore does not revoke the lease. Every other control bit and the
words at `0x07090060` and `0x07090160` must match exactly.

## Source and validation boundaries

The register plan follows the pinned A733 User Manual V1.00, watchdog section
8.3 and RTC section 9.1. The Linux `sunxi_wdt.c` v103 register layout and the
published A733 bootloader supply matching restart and reset configuration
references. The pinned RTC clock driver identifies the source-selection
registers at `0x07090000`, `0x07090060` and `0x07090160`.

The focused check composes the actual storage service, channel, MMC,
PMIC, rail, pin and clock code with modeled MMIO, time and host completions.
It covers delayed response completion, admission of the next request, one-use
renewal, final response draining, operation expiry and permanent loss. A separate
focused case verifies the final reload completion guard at the session cutoff. Native
compilation covers the changed core and hardware adapter. The composed check and both changed native objects passed. The recorded evidence
is in `~/.cache/sunstoneos/evidence/maintenance-watchdog-20261017/batch2`.
The cutoff case and changed core object passed separately in the adjacent
`cutoff` directory; the prior cases and unchanged native adapter were reused.
The `bind-revoke` check separately passed revocation before and during admission,
with only the changed core object rebuilt.
These checks cannot establish physical reload or recovery behavior.

The original watchdog diagnostics and USB observer remain nonfeeding. Their
existing evidence is reused; maintenance work does not require replaying those
checks. The distinct maintenance launch/runtime profile and physical recovery
admission must be connected before any write-capable image is eligible to run.
