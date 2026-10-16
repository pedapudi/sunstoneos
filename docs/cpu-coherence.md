# Cached CPU startup has separate reset and translation components

The [CPU coherence component](../platform/a733/cpu_coherence.c) supplies revision
admission, reset workarounds, immutable translation tables and cold cache-entry
primitives for the A733. It is implemented and checked on the host and in an
instruction emulator. No boot image calls it yet. Physical cache coherence,
exception integration and complete processor support remain unqualified.

The A733 contains six Cortex-A55 cores, two Cortex-A76 cores and a DynamIQ Shared
Unit (DSU), which connects their private caches to a shared cache and snoop
filter. The A733 manual describes that topology on printed pages 240–241.
Pinned Trusted Firmware-A (TF-A) implementations require hardware-assisted
coherency for both CPU families. These sources establish the intended architecture;
they do not establish the attached board's current cache, reset or ownership state.

The physical boot-CPU record at
`~/.cache/sunstoneos/evidence/a733-sram/result.json` reports MIDR `0x412fd050`,
a Cortex-A55 r2p0. MIDR identifies the CPU implementation and revision.
The other seven MIDRs and the DSU revision still require physical observation.

## Reset controls follow a bounded revision policy

The [public interface](../include/a733_cpu_coherence.h) accepts these CPU revisions:

| Core slots | CPU | Accepted revisions |
| --- | --- | --- |
| 0–5 | Cortex-A55 | r0p0, r0p1, r1p0, r2p0 |
| 6–7 | Cortex-A76 | r0p0, r1p0, r2p0, r3p0, r3p1, r4p0, r4p1 |

Other revisions refuse before any control write. Admission also requires the
expected implementer, architecture, CPU part and MPIDR affinity. The address
profile requires 4 KiB translation granules and at least 40-bit physical
addressing. A CPU that supports only 36-bit physical addresses refuses.

The DSU revision admission list is r0p0, r0p1, r1p0, r1p1, r2p0, r2p1, r3p0,
r3p1, r4p0 and r4p1. This is a bounded software policy for the register layout,
not an inventory of DSU revisions present on the board. Unknown revisions refuse.
The boot coordinator must compare every core's observed DSU identity and
configuration before permitting cached execution.

The planner applies these source-defined reset controls:

| CPU and condition | Reset actions |
| --- | --- |
| A55 through r0p1 | Disable dual issue and L1 page walks for 768277/846532 and 798797/903758 |
| A55 r0p0 without the source-defined private-L2 configuration | Set write-stream controls for 778703 |
| A55 through r1p0 | Install instruction patches in slots 0 and 1 for 1221012 |
| A76 through r1p0 | Disable static prediction for 1073348 |
| A76 through r2p0 | Apply 1130799, 1165347, 1207823 and 1220197 control masks |
| A76 through r3p0 | Apply 1257314, 1262606, 1262888 and 1275112 control masks |
| A76 through r4p0 | Apply 1791580 and 1868343 control masks |
| Every admitted A76 | Apply 2356586 and 3888013; install patch slots 3–5 for 1946160; retain static store-bypass mitigation |
| DSU r0p0 | Disable high-level clock gating for 798953 |
| DSU before r2p0 with the accelerator coherency port | Apply clock-gating mask for 936184 |

Register updates preserve unrelated bits and receive readback checks. The
instruction-patch sequences follow the published write order; the component does
not invent a patch-store readback contract. These patch slots belong to the boot
owner for the complete boot lifetime.

The DSU registers are shared. Core 0 applies their workaround masks while every
other core remains parked. A secondary verifies required masks and refuses if
they are missing. It never repairs shared DSU state. CPU power-down and DSU reset
remain separate operations; this component performs neither.

## Ownership precedes every hardware write

A caller supplies a retained cold-boot token and fresh zeroed controller storage.
The token refers to an existing exclusive reservation; a nonzero integer cannot
establish physical ownership. The boot coordinator must own all CPU, DSU, code,
table, stack and diagnostic allocations and exclude foreign firmware and DMA.

All participating CPUs must be parked with translation and caches disabled and
exceptions masked. Previous owners must have cleaned dirty aliases and removed
conflicting mappings. Reset-state cache invalidity must be independently
established. The component does not discard unknown dirty data or use a global
set/way invalidation to conceal unresolved ownership.

The native controller requires EL3, little-endian access, disabled EL3/EL1
translation and caches, and masked exceptions. It reads identification and
configuration registers before planning writes. Each poll performs at most one
register operation and checks monotonic time before and after that operation.
The native clock adapter requires the board's 24 MHz physical counter.

A failed operation, dropped control write, regressing clock or elapsed deadline
enters `RETAINED`. The token and all resources remain owned. Subsequent terminal
polls perform no operations. `PREPARED` means reset controls completed; it does
not authorize scheduling or establish physical coherence.

## EL3 and nonsecure EL1 use matching memory attributes

A cached EL1 image cannot retain an unrelated MMU-off EL3 monitor and assume all
shared accesses have matching attributes. The component builds separate EL1 and
EL3 tables and supplies a cold entry primitive for each exception level.
Both regimes use the following identity mappings:

| Physical interval | Memory type | Access |
| --- | --- | --- |
| `0x00000000`–`0x40000000` | Device-nGnRnE | Privileged read/write, execute-never |
| `0x40000000`–`0x48000000` | Normal write-back, write-allocate, Inner Shareable | Read/write, execute-never, except below |
| `0x41000000`–`0x41200000` | Same cached Normal type | Read-only executable code |
| `0x42000000`–`0x42200000` | Normal noncacheable, Inner Shareable | Privileged read/write, execute-never monitor and tables |

Intervals exclude their upper endpoints. Ordinary RAM is accessible to EL0
through the EL1 tables. This profile therefore provides neither task isolation
nor coverage of all installed RAM; its mapped DRAM extent is 128 MiB.

EL3 RAM descriptors select the nonsecure physical address space, matching
nonsecure EL1 cache tags. EL3 Device mappings remain secure for distributor and
platform-control accesses. EL3 descriptors use their architectural access-bit
requirement and single execute-never bit. EL1 descriptors use separate privileged
and unprivileged execute-never bits.

Each regime needs its own 4 KiB level-1 table and 4 KiB level-2 table. All four
physical pages belong to the monitor interval, with exclusive matching CPU
aliases. Table walks remain noncacheable, matching the table-storage mapping.
No mutable cached alias of the tables or monitor is permitted.

Both regimes use `MAIR = 0x44ff00`. The translation-control values are
`TCR_EL1 = 0x200803019` and `TCR_EL3 = 0x80823019`. They select 39-bit virtual
addresses, 40-bit physical addresses, Inner Shareable noncacheable table walks
and 4 KiB granules. EL1 disables its second address range; EL3 includes its
reserved-one fields. Table construction refuses misalignment, overlapping
output arrays, unsupported regimes and out-of-reservation table addresses
before modifying either array.

## Cache entry requires published immutable memory

The [assembly primitives](../platform/a733/cpu_coherence.S) provide explicit
publication and cold enable operations. They accept absolute physical-counter
deadlines, check time between maintenance operations and reject a regressing
counter. The caller must maintain the counter's frequency and ownership.
A deadline cannot interrupt a hung cache instruction or bus transaction;
qualified independent watchdog recovery remains mandatory.

Publication requires EL3 with translation and caches disabled and an owned range
inside mapped DRAM. It checks the CPU's 64-byte data-cache line size, rounds the
start down to a complete line, cleans each line with `DC CVAC`, and completes
with `DSB SY`. Ownership must include both partially covered boundary lines.
The operation preserves dirty data. It does not prove that a previous owner's
secure/nonsecure or virtual aliases were cleaned.

The EL1 and EL3 enable primitives require masked exceptions, disabled translation
and caches, a reserved table root, and a published canonical table graph. Code,
stacks, vectors and return addresses must already lie within their admitted
mappings. Every CPU must have completed its reset controls. The coordinator
must have selected one consistent profile for all participants.

Each primitive completes prior stores, invalidates its local instruction cache,
installs the translation registers and invalidates local translation state.
The translation invalidation repeats with a completion barrier between requests,
covering A76 erratum 1286807. The primitive then enables translation and both
caches, synchronizes instruction execution and checks control/root readback.

A late result after enable can leave caches active. Failure therefore retains
all ownership and requires the caller to stop; it cannot authorize memory reuse
or automatic cache disable. Every participating EL3 context must adopt the
matching translated profile before concurrent cached operation. The current
noncacheable diagnostic continuation cannot call these primitives unchanged.

## Exception and power hooks remain integration requirements

Reset controls cover only part of the CPU errata contract. The planner returns
required integration-hook flags as obligations, without treating them as proof
that another component implemented those hooks.

- A55 1530923 and A76 1165522 require the speculative-address-translation
  workaround. EL3 exception entry must save and block lower-level table walks;
  restoration must write the translation-control register last. Any active EL2
  runtime needs the corresponding protection.
- A76 branch-history mitigation requires the source-defined 24-iteration sequence
  at relevant exception entries. The fixed store-bypass control does not supply
  that protection or complete a security-boundary policy.
- A76 2743102 requires a completion barrier in the CPU power-down path.
- Future shared translation invalidations must include the applicable duplicate
  invalidation and CVE-2025-10263 handling. This component uses local invalidation
  for cold entry and implements no live remapping or address-space recycling.

A production image must integrate these hooks, the mapped EL3 vectors, primary
and secondary arrival coordination, and retained fault records. It must also
qualify cached publication and shared atomic operations on the physical CPUs.
The existing noncacheable image remains a separate diagnostic prerequisite.

## Evidence compares native controls with pinned reset code

The first batch passed host AddressSanitizer/UndefinedBehaviorSanitizer checks
and 24 native instruction cases:

```sh
~/.cache/sunstoneos/venv/bin/python checks/a733-cpu-coherence.py \
  --evidence ~/.cache/sunstoneos/evidence/cpu-coherence-20261016
```

The host checks cover every supported CPU/core combination, matching EL1/EL3
memory types and permissions, shared DSU ownership, preserved control bits and
failure/deadline injection at every register boundary of the A76 reset path.
They also check unsupported identity, feature and initial-state refusal.

The native checks execute the compiled controller and register adapter for the
four accepted A55 revisions and seven accepted A76 revisions. Their final control
values and patch contents match isolated unchanged reset bodies from pinned TF-A.
The remaining cases exercise publication and both cache-enable primitives,
including expired deadlines, late enable and control readback failure.
System registers, cache instructions and barriers are modeled; these results
cannot establish physical cache behavior.

A focused follow-up tightens physical-address admission to 40 bits. It checks
36-bit refusal and 40-bit acceptance for all eight core identities and compiles
the changed C source for AArch64. The reset/reference and cache-instruction
results remain valid because their relevant inputs are unchanged.
Evidence is under `~/.cache/sunstoneos/evidence/cpu-coherence-parange-20261016/`.
Both directories retain source snapshots, binaries, commands, logs and hashes.
The largest reported direct C stack frame is 128 bytes.

All TF-A references use revision
`0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e`:

| Source | Implementation basis |
| --- | --- |
| [A55 reset code](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/lib/cpus/aarch64/cortex_a55.S) | A55 revision checks and reset actions |
| [A76 reset code](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/lib/cpus/aarch64/cortex_a76.S) | A76 revision checks, patches and security/power obligations |
| [DSU macros](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/include/lib/cpus/aarch64/dsu_macros.S) | Shared-unit revision and accelerator-port conditions |
| [Translation descriptors](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/lib/xlat_tables_v2/xlat_tables_core.c) | EL3 access bits, physical security state and memory attributes |
| [Translation architecture helpers](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/lib/xlat_tables_v2/aarch64/xlat_tables_arch.c) | Execute-never fields, TCR and completion ordering |
| [EL3 common macros](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/include/arch/aarch64/el3_common_macros.S) | Speculative-translation entry/restore obligations |

The source archive is retained at
`~/.cache/sunstoneos/sources/cpu-coherence/tf-a.tar.gz`, SHA-256
`c60838d0f422a919b4cfce0357234b91e6dbefcfc532946d32983ed381123f62`.
Extracted references and pin proposals remain outside Git. The existing
[A733 manual pin](cpu-continue.md) is reused. No board access, benchmark or
existing SMP test ran for this component.
