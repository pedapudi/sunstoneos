# Secondary CPUs park until power cleanup authorizes entry

The [secondary entry code](../platform/a733/cpu_entry.S) implements a reserved
AArch64 EL3 entry for A733 cores 1 through 7. Each core reports its identity and
waits in its own mailbox. The [boot coordinator](../platform/a733/cpu_entry.c)
releases it only after the existing [power controller](cpu-start.md) completes
request cleanup and the matching arrival report passes validation.

Host sanitizer checks, instruction emulation and freestanding AArch64 compilation
pass. Core 1 reaches the physical EL3 arrival report and powers itself off before
release. Cache coherence, per-core interrupt setup and eight-core FreeRTOS
startup remain outstanding. The [startup record](a733-smp-supervised.md) holds
the physical evidence and the later interrupt-initialization failure.

## The boot owner reserves memory before starting a CPU

The [interface](../include/a733_cpu_entry.h) borrows one initialized, unpolled
power controller for the entire handoff. Its caller must be the serialized,
privileged boot owner. No other caller may poll or modify that power controller.
The original CPU, register, clock, power and memory reservations remain held
through every success or failure result.

Before initialization, platform startup must reserve the linked executable entry,
vectors, continuation, mailboxes and stacks. It must publish the code to physical
memory, exclude remote firmware writers and remove dirty or cacheable aliases
covering these allocations. The native adapter cannot establish those conditions.
A nonzero epoch identifies this one startup attempt and must never be reused
within the reservation's lifetime.

The entry defines two allocations:

| Allocation | Size and alignment | Ownership |
| --- | --- | --- |
| Mailbox array | 2 KiB, aligned to 128 bytes | Eight slots of 256 bytes; each slot separates a primary command from a secondary report by 128 bytes |
| Stack array | 32 KiB, aligned to 4 KiB | Eight stacks of 4 KiB; the stack top for core `n` follows stack `n` |

Core 0's slot and stack remain unused. Platform image initialization must zero
the mailbox array once, before any secondary CPU starts. An active or retained
slot must never be cleared for reuse. Separate command and report storage avoids
multiple writers sharing the same cache line when later platform integration
establishes cache coherence. This component itself uses cache-disabled access.

The native adapter verifies that the entry, mailbox array and stack array occupy
disjoint physical DRAM ranges below 4 GiB. It also requires the fixed continuation
address to lie outside those ranges. Linker placement, DRAM reservation and
protection from other bus masters remain platform responsibilities.

## Entry validates architecture before writing memory

The trampoline checks EL3, masks exceptions, and requires little-endian access
with the EL3 MMU, data cache and instruction cache disabled. It refuses an
unexpected state without attempting a cache teardown. It installs temporary
EL3 vectors and decodes the core number from MPIDR affinity level 1.
Core 0, unknown affinities and reserved MPIDR bits cannot select a mailbox.

The core requires an armed command with a nonzero epoch, its own core number,
the matching inverse epoch and clear reserved fields. Its report must contain
only zero bytes. A previous release command or stale report cannot authorize
another entry. The core writes its epoch, core number, actual MPIDR, MIDR and
SCTLR values, then publishes the arrival state after a full-system barrier.

The waiting loop uses the architectural event mechanism to avoid a lost wakeup.
It rechecks the immutable command fields and accepts only an armed or released
state. It does not access power-control or hotplug registers. A command change
outside the release transition records a protocol fault and parks the CPU.

All temporary exception vectors enter a stack-free handler. The handler derives
the core and mailbox again instead of trusting interrupted general registers.
It verifies the epoch, records ESR, FAR, ELR and SPSR, publishes a fault and parks.
It disables diagnostic writes before accessing the mailbox, so a nested fault
parks without repeating those stores. Faults before mailbox admission park
without writing a report; the primary detects the missing arrival by its deadline.

The architectural assumptions follow the pinned Trusted Firmware-A
[EL3 entry macros](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/include/arch/aarch64/el3_common_macros.S)
and [register definitions](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/include/arch/aarch64/arch.h).
These sources distinguish architectural initialization from platform secondary
CPU release and require explicit treatment of reset state. This trampoline admits
a restricted reset state; it does not implement the remaining CPU initialization.

## The primary retains ownership through release

Each owner poll performs one mailbox operation or one existing power-controller
poll. The owner first requires a zero slot and publishes the armed command.
It then runs the power controller, which programs the entry address, powers the
CPU and restores the request registers. Its `POWERED` result must precede release.

The primary also requires an arrival report with the current epoch, core number,
valid affinity and expected CPU identity. Cores 1 through 5 must identify as
Cortex-A55; cores 6 and 7 must identify as Cortex-A76. MIDR variant and revision
remain available for later errata selection. The part identifiers match the pinned
[Cortex-A55](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/include/lib/cpus/aarch64/cortex_a55.h)
and [Cortex-A76](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/include/lib/cpus/aarch64/cortex_a76.h)
definitions. Physical observations identify cores 0 and 1 as Cortex-A55 r2p0;
cores 2 through 7 still need identification.

Every operation uses the power controller's original absolute microsecond
deadline, with monotonic checks before and after it. Mailbox and power operations
must share the same physical counter timebase. A failed or late release may
already have allowed the secondary to continue. The owner therefore retains
all resources without rollback, reset, disposal or automatic retry.
Terminal polls perform no callbacks.

On release, the secondary selects its reserved stack, acknowledges release and
branches to the fixed linker symbol `a733_cpu_secondary_continue`. No command
field supplies an executable target. The coordinator's `DISPATCHED` result proves
that release acknowledgment was observed within the deadline. It does not prove
that the continuation completed or that a scheduler task ran.

## The continuation establishes the remaining CPU state

Platform integration must supply `a733_cpu_secondary_continue`; there is no
default implementation. The entry passes:

| Register | Value |
| --- | --- |
| `x0` | Core index, 1 through 7 |
| `x1` | Startup epoch |
| `x2` | Physical address of this core's report |
| `x3` | Physical address of this core's command |
| `SP_EL3` | Top of this core's reserved 4 KiB stack |

DAIF remains masked. The entry retains the admitted cache-disabled, MMU-disabled,
little-endian EL3 state. The continuation must replace the temporary vectors
before changing translation, cache state or `TPIDR_EL3`. It must establish
CPU-specific initialization, coherent memory, GIC wakeup and redistributor state,
EL1 exception handling and FreeRTOS startup. It must never return; an unexpected
return records a fault and parks the CPU.

The [native primary adapter](../platform/a733/cpu_entry_native.c) is restricted to
core 0 executing at EL3 with the MMU and data cache disabled and little-endian
access. It checks that local state before each operation, uses ordered physical
mailbox accesses and publishes commands with `DSB SY` followed by `SEV`.
It is a boot component and cannot be called from ordinary EL1 tasks.

## Reusable evidence covers the handoff implementation

The changed behavior is secondary entry, arrival reporting and primary release
after verified power cleanup. The principal risks are stale authorization,
release before cleanup, uncertain writes and damage to another core's mailbox.
The focused batch passes:

```sh
~/.cache/sunstoneos/venv/bin/python checks/a733-cpu-entry.py \
  --evidence ~/.cache/sunstoneos/evidence/cpu-entry-20261016
```

The evidence directory must be new; the runner refuses to overwrite an existing
batch. It preserves source snapshots, commands, logs, objects and the executed
ELF image outside the repository.

The sanitizer fixture composes the owner with the existing power controller for
every secondary target. It checks premature-release prevention, copied
configuration, uncertain and late completion at every mailbox operation, invalid
arrival reports, missing arrival, clock regression, stale slots and terminal
silence. Existing power-controller instruction-reference evidence remains valid.

The instruction harness executes the linked trampoline for every secondary core.
It checks register arguments, stack selection, arrival and release, malformed
commands, stale reports, architecture and affinity refusal, command mutation,
exception diagnostics, unexpected return and nested faults. Every memory access
must remain in the selected mailbox; secondary writes to primary command storage
are refused by the harness. System registers, barriers and events are modeled.
The harness does not reproduce physical exception routing or cache coherence.

Freestanding compilation produces 1,692 bytes of owner code/read-only data,
2,152 bytes for the native adapter and 4,248 bytes for the entry and vectors.
The largest reported direct C stack frame is 512 bytes. The owner requires runtime
`memcpy` and `memset`; other undefined symbols connect the power controller,
entry allocations and platform continuation. These software measurements do not
establish physical behavior; the later core-1 evidence is recorded separately.

The supervised RAM-only diagnostic uses a reviewed return procedure and manual
power removal after failure. Watchdog-dependent images retain their separate
recovery prerequisites. Physical acceptance still requires secondary arrival,
coherent memory, per-core interrupt handling and scheduler participation on all
seven secondary processors.
