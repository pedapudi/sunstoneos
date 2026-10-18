# All eight CPUs have a diagnostic path into the SMP kernel

The [CPU continuation](../platform/a733/cpu_continue.S) supplies the fixed target
used by the [secondary entry](cpu-entry.md). It configures each core's local
interrupt interface, installs separate EL3 and EL1 exception vectors, establishes
an EL1 stack and translation profile, and joins the FreeRTOS secondary-start
handshake. A primary entry uses the same EL1 setup and calls the image's primary
kernel function.

The implementation passes host sanitizer and instruction-emulator checks.
Physical execution remains unqualified. The native build requires a dedicated
eight-core image; existing single-core and QEMU images retain their own startup.
Cache-enabled coherent operation and applicable CPU/DSU errata remain required
work before complete processor support can be claimed.

## The diagnostic profile keeps all CPU caches disabled

The A733 user manual describes six Cortex-A55 cores, two Cortex-A76 cores,
private L2 caches and a shared DynamIQ Shared Unit with L3 and a snoop filter.
The CPU block diagram appears on printed page 241 of the
[pinned manual](https://gitlab.com/tina5.0_aiot/product/docs/-/blob/d0dc4d615be00b76e3a0818d0818eff8f69ffa15/A733/Hardware%E7%A1%AC%E4%BB%B6%E7%B1%BB%E6%96%87%E6%A1%A3/%E8%8A%AF%E7%89%87%E6%89%8B%E5%86%8C/A733_User%20Manual_V1.00.pdf).
The official Trusted Firmware-A implementations for
[Cortex-A55](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/lib/cpus/aarch64/cortex_a55.S)
and [Cortex-A76](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/lib/cpus/aarch64/cortex_a76.S)
require hardware-assisted coherency and contain separate revision-specific
reset workarounds. Those sources do not prove the attached board's current
interconnect state or that the workarounds have run.

This diagnostic profile enables EL1 address translation while keeping instruction
and data caches disabled on every participating CPU. The primary must use the
same profile and exclude cacheable aliases. The table builder creates these
identity mappings:

| Physical interval | Type and permissions |
| --- | --- |
| `0x00000000`–`0x40000000` | Device-nGnRnE, EL1 read/write, non-executable |
| `0x40000000`–`0x48000000` | Normal noncacheable, shared; exceptions below |
| `0x41000000`–`0x41200000` | Code: EL0/EL1 read-only and executable |
| `0x42000000`–`0x42200000` | Monitor allocations: EL1 read/write, non-executable |
| Remaining mapped DRAM | EL0/EL1 read/write, non-executable |

Intervals exclude their upper endpoint. Descriptors select inner shareability;
Normal noncacheable accesses receive the architectural treatment for that memory
type. Page-table walks are also noncacheable. The profile uses
`MAIR_EL1 = 0x4400`, `TCR_EL1 = 0x200803019` and the primary-published level-1
table address in `TTBR0_EL1`. It maps 128 MiB for bring-up and does not establish
complete DRAM coverage or task isolation.

Address translation permits use of Normal memory for exclusive-access operations.
Successful exclusives on noncacheable memory still depend on the physical memory
system. The [Cortex-A55 r2p0 manual](https://documentation-service.arm.com/static/5e7e09f6a3736a0d2e862d2f),
section A6.4.2, specifies a Data Abort for an exclusive load to a noncacheable
region that does not support exclusive accesses. Emulator success cannot
establish that support on the board.

Each CPU therefore attempts one private `LDAXR`/`STLXR` update, with at most
64 attempts. Failure retains the CPU and its resources before scheduler entry.
A dedicated image must also supply a bounded shared exclusion check through
`a733_smp_pre_scheduler(core, epoch, deadline_us)`. Every core calls this function
before publishing EL1 readiness. It must return one only after the image's
cross-core check succeeds; no default implementation supplies that result.

## Primary preparation publishes one immutable boot plan

The [interface](../include/a733_cpu_continue.h) supplies
`a733_cpu_continue_prepare(epoch, deadline_us)` for core 0 at EL3. It requires
disabled translation and caches, little-endian access, a fresh nonzero epoch,
a future absolute deadline and the dedicated linker layout. It constructs shared
tables and publishes the plan only after the table writes complete.

The linker must reserve the following storage:

- Executable entry, continuation and vectors within the code interval.
- Eight EL1 stacks of 32 KiB within `0x41200000`–`0x42000000`.
- Entry mailboxes and EL3 stacks, continuation plan and records, private exclusive
  probe words and both translation tables within the monitor interval.

The plan and each continuation record occupy 128 bytes. Each private exclusive
probe word has a separate 128-byte allocation. The two tables require 4 KiB
each and 4 KiB alignment. The primary must preserve all other reservations in
the monitor interval when placing these objects.

Startup must zero these owned allocations before preparation and publish code
to physical memory. It must exclude remote firmware and other bus masters from
writing them. Preparation validates the linked placement and fresh plan/record
storage; it does not create a physical ownership boundary or remove dirty aliases.
The plan and tables remain immutable for the boot lifetime.

## Each CPU configures only its local interrupt bank

The bounded [controller](../platform/a733/cpu_continue.c) requires an existing
CPU/register reservation and quiescent SGI/PPI sources. Every poll performs at
most one register operation and checks monotonic time before and after it.
Callbacks return success only after ordered, bounded physical access.

The primary must initialize the shared GIC distributor first. The controller
requires separate security states, enabled secure/nonsecure affinity routing,
enabled Group 0 and nonsecure Group 1, and no pending distributor write.
It then scans eight redistributors using `GICR_TYPER`, requires each A733 affinity
once and selects the current core by affinity. It refuses virtual-LPI frames,
duplicate affinities and an unexpected last-frame position.

A secondary requires clear power-request bits before clearing its own
GIC-wakeup-disable bit. It preserves every other hotplug-control bit and verifies
the result. Core 0 skips hotplug accesses. The controller then performs these
local operations:

1. Wait for the GIC-600 redistributor group to finish any power transition.
   Request power for this core by writing zero to `GICR_PWRR`; require `RDPD`
   clear before accessing interrupt state. A rejected request returns to the
   group-stability check under the original deadline.
2. Require LPIs disabled and wait for pending redistributor writes.
3. If the processor is sleeping, wait for `ChildrenAsleep` before clearing
   `ProcessorSleep`, then wait for the children to become awake.
4. Disable SGIs/PPIs, wait for write completion, and clear pending/active state.
5. Assign the local bank to nonsecure Group 1, keep timer PPIs level-sensitive,
   set SGI0 priority to `0xf0`, and enable SGI0.
6. Verify group, modifier, enable and priority state after write completion.
   Group verification requires all SGIs and timer PPIs 29/30 (`0x6000ffff`).
   Unimplemented PPI bits are read-as-zero/write-ignored under Arm IHI 0069G
   section 12.11.12; a full-width all-ones comparison is invalid. The group write
   still assigns every implemented local source to nonsecure Group 1.

The sleep transition follows the pinned
[GICv3 redistributor implementation](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/drivers/arm/gic/v3/gicv3_helpers.c).
A stalled transition, uncertain access or expired deadline retains every resource.
The controller performs no rollback or reset. Only a power request rejected
before completion may be reissued; an access failure terminates the sequence.
Terminal polls issue no callbacks. `GIC_READY` proves this register sequence only.

A733 SGI routing uses affinity level 1 for the target core and affinity level 0
value zero. The encoder produces SGI0 value `(core << 16) | 1` for cores 0–7.
The dedicated FreeRTOS port must use this encoding for remote yields.

## EL3 transfers to a fixed EL1 entry with a private stack

The assembly continuation replaces the temporary entry vectors before calling C.
It establishes the 24 MHz counter frequency used by this board and disables
local timer outputs during startup. After local GIC setup, it configures the GIC
system-register interface, nonsecure AArch64 execution, EL2 timer access and FP
access. Architectural reserved-one fields follow the pinned
[TF-A register definitions](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/include/arch/aarch64/arch.h).

Each CPU records its logical core in `TPIDR_EL1`, installs its own EL1 stack and
the continuation vectors, then enters EL1h with exceptions masked. EL1 installs
the published translation tables, invalidates local translation state, enables
translation and activates its nonsecure Group 1 interface. It checks the resulting
control registers before attempting exclusive operations or image hooks.

On primary core 0, `a733_cpu_primary_continue(epoch)` uses the reserved primary
EL3 boot stack and performs the same transition. After the shared exclusion check,
it calls the required fixed symbol `a733_smp_primary_el1()`. The primary hook must
configure timer PPI30 and start the kernel. This diagnostic path supplies no
secure-timer recovery handler; independent hardware watchdog recovery is required.

Each secondary publishes `EL1_READY`, waits for the port's primary-init flag under
the original deadline, publishes its secondary-ready flag and executes SVC106.
`SCHEDULER_REQUEST` means that the continuation requested the first task restore.
It does not prove that any task executed. The image must provide the matching
FreeRTOS SWI/IRQ handlers and an eight-core configuration.

EL1 SVC119 routes status and detail to the required image function
`a733_smp_exit_privileged`. Other synchronous exceptions and unexpected EL3
exceptions record syndrome, fault address and return address in the current
core's continuation record, then park. Fault handlers derive the core again and
use no stack for diagnostic writes. They suppress recursive diagnostic stores.
The original entry acknowledgment remains unchanged.

## Evidence separates component execution from physical support

The first batch passed host sanitizer checks and native instruction emulation:

```sh
~/.cache/sunstoneos/venv/bin/python checks/a733-cpu-continue.py \
  --evidence ~/.cache/sunstoneos/evidence/cpu-continue-20261016
```

The host model checks all eight local banks with reordered redistributor frames,
wait conditions, preserved unrelated bits, retained failures and table permissions.
It injects uncertain and late completion at all 38 operations in the secondary
sequence. The instruction harness executes the actual C and assembly continuation
for every core. It also checks CPU-interface refusal, failed exclusives, shared-check
refusal, missing primary readiness and late GIC completion.

A focused follow-up checks corrected EL3 reset constants while reusing unchanged
C objects and host evidence. A second follow-up reuses that same executed ELF
and checks the noncacheable-exclusive Data Abort path and SVC119 argument routing.
The final evidence directories are `cpu-continue-registers-20261016` and
`cpu-continue-vectors-20261016` under the same cache location. Each records its
reused evidence identity. The runner refuses to overwrite an evidence directory.

The emulator models privileged registers, barriers, GIC accesses and exception-level
transitions. Image hooks are bounded fixtures; these component results do not
prove FreeRTOS task execution, physical translation behavior, inter-core exclusion,
interrupt delivery or coherent caches. The dedicated image supplies separate
integration checks. Existing entry and power-controller evidence remains valid.

Source snapshots, commands, logs, native objects and executed images remain outside
Git. The largest reported direct C stack frame is 272 bytes. The implementation
reserves eight 32 KiB EL1 stacks in addition to its monitor allocations; linker
alignment contributes to the final image footprint.

Physical acceptance still requires qualified watchdog recovery, all eight CPU
identities, bounded exclusive and SGI checks, scheduler participation, appropriate
reset/errata handling, and coherent cached operation. Performance optimization
follows complete hardware functionality.


## Redistributor power-up is implemented but not physically qualified

The controller follows the pinned Arm Trusted Firmware-A
[GIC-600 power sequence](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/drivers/arm/gic/v3/gic-x00.c).
The `cpu-continue-gic600-power` entry in `sources.lock.json` pins that BSD-3-Clause
source reference. Repository code executes the sequence; no additional firmware
binary or runtime dependency is loaded.

The controller compares group power-down and power-off status before requesting
power. It writes zero to `GICR_PWRR`, selecting the current core rather than all
cores in its redistributor group. It confirms that the power-down bit is clear
before reading local interrupt state. Each poll keeps the existing one-operation
limit and checks the original deadline. Uncertain accesses retain ownership.
Both native access adapters permit the selected frame's power register.

Focused sanitizer cases cover an already-powered primary, a powered-down
secondary, a group transition followed by a rejected request, each of the three
power-operation failures, and both group and power-completion deadlines. Two
native instruction cases reach the primary and secondary kernel-entry hooks.
The models reject interrupt-state access before power-up. The cached adapter
also compiles with the existing supervised image configuration. Evidence is
under `~/.cache/sunstoneos/evidence/gic600-power-20261018/`.

The returning diagnostic has not executed this correction. It needs matching
redistributor power observation and cleanup before physical qualification.
Earlier startup failures returned no report and cannot yet be attributed to
this omission. Redistributor discovery and the later interrupt, cache and
scheduler stages still require board evidence.


Core 1 local initialization and complete restoration pass physically under
`native-local-group-mask-20261018/`. The earlier returned refusal exposed
`GICR_IGROUPR0 = 0xfff0ffff`, which the former full-width comparison rejected.
The corrected required-bit check passes through final SGI0 priority verification,
then verified shutdown and ROM return. This result covers local register setup
with delivery disabled; EL1 transition, interrupt delivery and SMP remain required.
