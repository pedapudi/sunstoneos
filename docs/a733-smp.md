# Eight-core diagnostic image awaits physical recovery qualification

The dedicated `a733-smp-diagnostic` target links the A733 CPU power controller,
secondary entry, EL3-to-EL1 continuation and eight-core FreeRTOS scheduler.
Its intended check covers six Cortex-A55 cores and two Cortex-A76 cores.
The image builds, its admission checks pass under host sanitizers, and instruction
emulation verifies all eight scheduler interrupt destinations and bounded boot
exclusion checks. The complete image has not executed on a board or in a machine
emulator. Physical CPU startup, shared atomic operations and scheduling remain
unqualified.

This is a functional diagnostic with instruction and data caches disabled.
All CPUs use the [continuation's canonical Normal-noncacheable mapping](cpu-entry.md).
Physical support for exclusive accesses to that mapping still requires testing.
Cache-enabled coherence, CPU-specific errata and production memory isolation
remain required work.

## Boot owns resources until an independent watchdog resets the board

The image requires a reserved, published ELF image and private RAM allocations.
It enters at EL3 on core 0 with the MMU, instruction cache, data cache and
big-endian mode disabled. Other firmware and bus masters must relinquish the CPU,
clock, rail, interconnect, GIC and image allocations before entry.
A nonzero lease in a host-provided contract names that external reservation.
The image cannot establish ownership or remove cacheable aliases.

The [native boot adapter](../platform/a733-smp/boot.c) restricts each power
callback to its selected CPU's architecture, entry, hotplug and status words.
It verifies core 0, EL3 and cache-disabled access before each callback.
The existing [power controller](cpu-start.md) supplies the register sequence;
the adapter does not duplicate it. The published board-firmware comparison in
that document remains the sequence evidence.

The boot owner prepares the shared translation tables, disables GIC distributor
groups and shared interrupt sources, then enables affinity routing and the
required groups. Each of seven secondary CPUs must complete the existing
arrival, power-cleanup and release protocol before core 0 enters EL1.
All components use the same 24 MHz physical counter and absolute startup deadline.
A failure retains every allocation and register reservation, without a retry.

Each CPU first completes the continuation's bounded private exclusive-access
probe. All eight then enter a shared exclusion check before any FreeRTOS spinlock.
Each CPU increments a shared counter 32 times under a bounded exclusive lock.
Core 0 requires a total of 256 increments and releases the boot barrier.
This small check can expose immediate failures; success would not qualify every
inter-core memory-ordering case.

## Scheduler interrupts address A733 affinity level 1

The image generates its own FreeRTOS port using the existing SMP fixes and an
[A733-specific patch](../patches/freertos-a733-smp.patch). CPU index `n` maps to
Aff1=`n`, Aff0=0. SGI0 therefore uses `ICC_SGI1R_EL1 = (n << 16) | 1`.
The generated primary readiness loop uses acquire/release publication and checks
the startup deadline. Redistributor discovery supplies the primary priority
register address; the image does not assume frame order.

The continuation installs separate EL3 and EL1 vector tables and reserved stacks
for every CPU. Core 0 alone enables physical-timer PPI30 and generates kernel
ticks. Each core receives an explicit SGI0 and runs two pinned tasks: a busy
background task and a higher-priority task that preserves a distinct `d8` value
across 32 blocking switches. Core 0 requires every worker's completion, background
progress, SGI receipt and clear failure record before reporting success.
All C code uses general registers only; the FP assembly fixture supplies the
floating-point activity.

## Recovery qualification must precede contract publication

The [contract ABI](../platform/a733-smp/boot.h) occupies a page outside result
clearing. A coordinator must provide a matching compiled build-input digest,
nonzero session epoch, generation and inverse generation, resource lease,
absolute deadlines and the recorded watchdog configuration. Reusing the generation
already present in the result record is refused. The digest identifies selected
build inputs; the coordinator separately records and verifies the exact ELF hash.

The current adapter admits only a four-second watchdog interval with RTC 32 kHz
clock selection, whole-system reset and the documented output setting.
It checks watchdog MODE, CFG and OUTPUT readback, FEL flag, ROM redirect flag and
entry, clear alternate redirect, and the low physical FEL input before GIC or
CPU-power writes. It copies the contract before those checks and bounds startup
by one second from admission. The diagnostic deadline leaves at least one
second before the declared watchdog expiry.

These readbacks cannot prove oscillator progress, watchdog independence, the
arm timestamp or successful ROM return. The coordinator must first qualify that
specific watchdog interval, clock source and protected FEL recovery procedure.
The pending [SRAM watchdog trial](watchdog-expiry.md) has not yet established
that evidence. Its result must also match this contract's clock source. A host-to-image adapter that arms and publishes this
contract has not been integrated. Physical execution is blocked until that adapter
and recovery evidence exist.

A future qualified coordinator must load every file-backed ELF segment, reserve
every memory segment and verify the image. It must prepare the contract with
`magic` clear, arm and read back the watchdog under exclusive board ownership,
then publish `magic` last and enter the image before its startup deadline.
The declared arm time must describe that actual arm operation.
The coordinator must retain the physical board lock through watchdog expiry and
verified ROM return. An observation timeout does not release board ownership.

The diagnostic never feeds, disarms or resets the watchdog. Success and failure
park with resources retained until the externally armed reset occurs.
Watchdog reset can invalidate DRAM contents; retained result addresses do not
promise post-reset evidence preservation. The recovery adapter must provide an
independently qualified observation or preservation mechanism before a board trial.

## Linker reservations separate code, task memory and monitor state

| Range | Purpose |
| --- | --- |
| `0x41000000..0x41200000` | Read-only executable code at EL0 and EL1 |
| `0x41200000..0x42000000` | Writable non-executable scheduler data and eight EL1 stacks |
| `0x42000000..0x42004000` | Diagnostic result reservation |
| `0x42004000..0x42005000` | Immutable host contract page |
| `0x42005000..0x42200000` | EL1-only, non-executable mailboxes, continuation records, tables and EL3 stacks |

The linker names both result and contract reservations and rejects overflowing
allocations. Startup clears ordinary and monitor BSS independently, preserving
the contract and previous result until admission checks finish.
The result header records the epoch, generation, stage, status, detail and counter
timestamps. Stage 5 is terminal; status 0 and completion mask `0xff` report the
implemented diagnostic's success. Status 1 means admission refusal, 2 means boot
failure, 3 means deadline expiry, and 4 means a scheduler diagnostic failure.
Secondary faults also retain per-core continuation or diagnostic records.
Their ELF symbol addresses appear in the image-check evidence.

## Focused evidence preserves the executed inputs

Run the separate checker after integrating the continuation dependency:

```sh
~/.cache/sunstoneos/venv/bin/python checks/a733-smp-image.py \
  --freertos ~/.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2 \
  --evidence /absolute/new/evidence-directory
```

The checker snapshots the repository dependencies and pinned FreeRTOS inputs,
builds outside the repository, and refuses an existing evidence directory.
It checks contract tampering, deadline refusal, watchdog-source refusal and
per-core MMIO boundaries under AddressSanitizer and UndefinedBehaviorSanitizer.
It inspects ELF segments and reserved symbols, then executes the linked scheduler
interrupt calls for all eight destinations. Linked shared-lock checks cover primary
and secondary completion, a missing peer and a held lock with deadline expiry.
The instruction model does not reproduce physical inter-core contention.

Reusable evidence is in
`~/.cache/sunstoneos/evidence/a733-smp-image-20261016`.
The initial configure needed the available Make generator and explicit linker
propagation into compiler checks. Linking then required the runtime `memmove`,
task-delete configuration and vector-table alias. Those failures and corrected
commands remain in the evidence directory. The successful host admission result
was reused throughout. The continuation's corrected EL3 reset constants required
only its assembly object, build identity and final link to change.
The image manifest records executed and final artifact identities separately.
