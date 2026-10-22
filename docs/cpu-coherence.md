# Cached CPU startup has separate reset and translation components

The [CPU coherence component](../platform/a733/cpu_coherence.c) supplies revision
admission, reset workarounds, immutable translation tables and cold cache-entry
primitives for the A733. The cached FreeRTOS image calls these components.
Production preparation and cached EL3/EL1 entry pass physically on every secondary.
A returning diagnostic also passes concurrent atomic increments and ordered
payload exchange across all seven secondaries, with explicit cache cleaning and
a matching final physical-memory copy. Evidence:
`secondary-coherence-probe-20261018/physical-113/`. Timer PPI30 and peer SGI0
delivery also pass on every secondary, followed by quiescence, cache cleaning
and restored shutdown under `secondary-interrupt-probe-20261018/physical-114/`.
Boot-CPU participation, general task-context handling, broader runtime memory
use and eight-core FreeRTOS scheduling remain unqualified.

The A733 contains six Cortex-A55 cores, two Cortex-A76 cores and a DynamIQ Shared
Unit (DSU), which connects their private caches to a shared cache and snoop
filter. The A733 manual describes that topology on printed pages 240–241.
Pinned Trusted Firmware-A (TF-A) implementations require hardware-assisted
coherency for both CPU families. These sources establish the intended architecture;
they do not establish the attached board's current cache, reset or ownership state.

The physical boot-CPU record at
`~/.cache/sunstoneos/evidence/a733-sram/result.json` reports MIDR `0x412fd050`,
a Cortex-A55 r2p0. MIDR identifies the CPU implementation and revision.
Physical secondary records identify cores 1–5 as A55 r2p0 and cores 6–7 as
A76 r4p1. The [startup record](a733-smp-supervised.md) holds their observations,
preparation and restoration results.

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
| `0x00000000`–`0x40000000` | Device-nGnRnE, Outer Shareable field | Privileged read/write, execute-never |
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
aliases. No CPU data mapping of the tables is cacheable, and EL1 has no data
mapping of the monitor interval at all. The builder sets the Outer Shareable
field (SH = `0b10`) of level-1 entry 0 in both regimes; for Device memory the
field does not change behaviour, and the single-core images no longer set it
after the builder returns.

Both regimes use `MAIR = 0x44ff00`. The translation-control values are
`TCR_EL1 = 0x200803519` and `TCR_EL3 = 0x80823019`. Both select 39-bit virtual
addresses, 40-bit physical addresses, Inner Shareable table walks and 4 KiB
granules. EL1 walks are write-back read/write-allocate cacheable
(IRGN0 = ORGN0 = `0b01`), the value that `platform_init()` already uses for
its own tables. EL3 walks stay noncacheable. EL1 disables its second address
range; EL3 includes its reserved-one fields. `A733_CPU_COHERENCE_TCR` in
[the header](../include/a733_cpu_coherence.h) is the only source of the EL1
value: assembly expands it into its `mov`/`movk` sequence, and host checks read
it through [tools/a733_tcr.py](../tools/a733_tcr.py). Table construction
refuses misalignment, overlapping output arrays, unsupported regimes and
out-of-reservation table addresses before modifying either array.

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

The flush primitive, `a733_cpu_coherence_flush()`, runs `DC CIVAC` (clean and
invalidate to the point of coherency) on every 64-byte line of a range inside
`0x40000000`–`0x48000000` and completes with `DSB SY`. It requires EL3 or EL1
with translation and caches disabled and masked exceptions, and checks the
deadline before and after the loop. Clean and invalidate never discards data,
so a refusal part way through leaves memory unchanged. With the MMU off, EL3
addresses the Secure physical address space and EL1 the Nonsecure space, so
the caller's exception level selects which alias the flush reaches.

The EL1 and EL3 enable primitives require masked exceptions, disabled translation
and caches, a reserved table root, and a published canonical table graph. Code,
stacks, vectors and return addresses must already lie within their admitted
mappings. Every CPU must have completed its reset controls. The coordinator
must have selected one consistent profile for all participants.

Before it writes any translation register, the EL1 primitive runs the same
sequence as the exported `a733_cpu_coherence_check_el1()`. It flushes the
root page and the level-2 page that root entry 1 names, reads both pages back
with Device loads and compares their 64-bit additive sum with the value that
core 0 recorded at EL3. Root entry 1 must be a plain table descriptor to
another page of the monitor interval. A mismatch or a malformed root returns
failure with translation still off. The cached single-core images enable
translation in their patched `platform_init()` and call the exported check
there first.

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

## EL1 table walks read descriptors through the data caches

**Status: implemented and checked in instruction models; not run on the board.**
With `TCR_EL1` IRGN0 = ORGN0 = `0b01`, a descriptor read that misses the
walk caches can be served from a core's data cache or the DynamIQ Shared Unit
(DSU) L3 cache instead of DRAM. A cacheable walker can also hit a stale line
over a table page, so the tables follow this sequence:

1. **Cold entry, core 0, EL3, MMU off.** `a733_cached_memory_prepare()` flushes
   the whole table section (the five table pages and the checksum line) before
   it writes the first descriptor. This removes any Secure dirty line that a
   later eviction or the existing `DC CVAC` publication in
   `platform/a733-smp-cached/continue.c` could write back over a descriptor.
2. **Build.** The builder and `a733_cached_memory_prepare()` write every
   descriptor. No code writes a table after preparation returns. As its last
   step, core 0 stores the 64-bit additive sum of the 1,024 words of the EL1
   root and level-2 pages in `a733_cached_el1_table_sum`. That symbol is a
   64-byte line of its own placed after the five pages in the
   `.bss.a733_cached_tables` section, so the table pages keep their addresses
   and table memory grows from 20 KiB to 20 KiB plus 64 bytes. Only whole
   table pages receive the EL3 Normal noncacheable mapping; the checksum page
   keeps the Device mapping of the other monitor pages.
3. **EL1 entry, every core, MMU off.** The EL1 enable primitive, or
   `a733_cpu_coherence_check_el1()` in the single-core images, flushes the two
   EL1 pages in the Nonsecure space, reads them back and compares the sum (see
   the previous section). A dirty Nonsecure line written back before the check
   makes the check refuse.
4. **Runtime.** The EL1 walker is the only reader of the EL1 tables. EL1 has no
   data mapping of the monitor interval, and EL3 does not access the tables
   after preparation.

The tables are immutable once core 0 returns from preparation, and the record
barrier in `continue.c` holds every secondary until then, so no lock is
needed. For a flush with the MMU off to remove a stale line from another
core's cache, `DC CIVAC` must reach every cache in the Outer Shareable domain.
The Arm Architecture Reference Manual states this for Device-type accesses, but
no copy of that manual is pinned here.

The counts below come from `checks/cpu-table-walks.py` on the cached
eight-core diagnostic image, against the same image built from the previous
integration commit:

| Per boot | Before | Now |
| --- | --- | --- |
| `a733_cpu_coherence_enable_el1()` instructions, success path | 75 | 2,233 |
| EL3 `DC CIVAC` before the first descriptor write | 0 | 321 |
| EL1 `DC CIVAC` per core | 0 | 128 |
| EL1 `LDP` readback per core | 0 | 512 (1,024 words) |
| `DC CVAC` publication of the table section in `continue.c` | 320 | 321 |
| Table section bytes | 20,480 | 20,544 |

For the eight-core image this is 1,666 table-line operations per boot, or
1,345 after `continue.c` drops its table publication. A cached single-core
image adds 449. At 494 MHz and at most 70 cycles per maintenance operation the
estimate is about 0.25 ms, plus about 0.1 ms per core for the readback at
200 ns per Device load. The work runs once per boot.

When `A733_DEVICE_MEMORY_LAYOUT` reaches 2, `a733_cached_memory_prepare()`
also maps `0x8000_0000`–`0xFFFF_FFFF` at EL1 as two 1 GiB write-back blocks
(`0x0060000080000705` and `0x00600000c0000705`, EL1 read/write, never
executable) and leaves the EL3 entries invalid. A mapped write-back block
receives speculative reads, so preparation first calls
`a733_dram_decode_check()`, which the DRAM-controller component must provide.
It must confirm on every boot that DRAM decodes linearly to `0x3_4000_0000`
with no protected region, including the boot0 secure region, inside the two
blocks. The header constant is 1, so this code is compiled out.

### Board measurement before the profile is adopted

No image with this profile runs on the board before the Nonsecure dirty-line
experiment in [cold entry](cold-entry.md#the-el1-tables-need-a-nonsecure-dirty-line-experiment)
passes. Each run records the CPU-L, CPU-B and DSU clock rates.

- **Boot.** Each changed image class reaches the scheduler, and its recorded
  `TCR_EL1` equals the header value.
- **Walk A/B.** Image A is built from the commit before this profile and image
  B from this profile. A forced-walk loop runs `TLBI VMALLE1`, `DSB`, `ISB` and
  one load from a different 2 MiB block per iteration. It records the PMU
  events `CPU_CYCLES` (`0x11`) and `DTLB_WALK` (`0x34`) on core 0 (Cortex-A55)
  and core 6 (Cortex-A76). A GPU frame, an NPU inference and a recognizer
  inference record the same events and `ITLB_WALK` (`0x35`) on core 0. B must
  not use more cycles than A beyond the spread of five runs; otherwise only the
  TCR value reverts. The eight-core image schedules tasks on the Cortex-A76,
  so it takes the new value only after the core-6 loop and the same workloads
  on core 6 pass. The estimate is 50 to 75 cycles per DRAM descriptor read at
  494 MHz against 30 to 50 cycles for an L3 hit.
- **Core-6 harness.** The secondary cached-entry probe
  (`boot/a733/native-el1-cache-probe.S` with `SUNSTONE_SECONDARY_CACHE`)
  completes cached entry on both Cortex-A76 cores. It enters through a
  digest-pinned production image, which still sets the previous `TCR_EL1`, so
  it measures image A only. Image B needs a production image built from this
  profile and a new pin.
- **Stage 2.** The decode check reports 12,288 MiB and the location of the
  secure region; EL1 then makes one read and one write per 1 GiB block.

The Cortex-A55 Technical Reference Manual (100442_0200_00, section A5.5.1)
states that walks are cacheable only with write-back IRGN and ORGN and the
cache enabled. The Cortex-A76 equivalent is not pinned; the core-6 run supplies
a measurement in its place. Whether the A733 has a system cache beyond the DSU
is unknown.

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

[The table-walk check](../checks/cpu-table-walks.py) runs a linked cached
image in a Unicorn instruction model with modeled system registers, cache
maintenance and counter. It executes the actual `a733_cached_memory_prepare()`
and checks that the table-section flush precedes the first descriptor write,
that the checksum is the last table write, and that the EL1 flush, `DSB SY` and
readback precede every translation register write. A corrupted table word,
root entry or checksum leaves `SCTLR_EL1.M` clear and writes no translation
register. It also covers the flush refusals, the header's TCR fields, the
absence of table writes after preparation (through the writer scan of
`checks/device-memory-capacity.py`), and the order of the check before the
`TCR_EL1` write in each single-core image passed with `--single-core-image`.
Its literal scan allows the old EL1 value only in the non-cacheable
continuation profile and in tools and probes that describe frozen,
digest-pinned images:

```sh
~/.cache/sunstoneos/venv/bin/python checks/cpu-table-walks.py --output OUT \
  --image BUILD/a733-smp-cached-diagnostic.elf \
  --single-core-image BUILD/sunstone-a733-crypto-single-core.elf
```

`--baseline` records the same counts for an image built before this profile.
The image must not use the extended device-memory profile, whose preparation
requires boot admission; `checks/device-memory-capacity.py` covers that
profile on eight QEMU CPUs.

The source archive is retained at
`~/.cache/sunstoneos/sources/cpu-coherence/tf-a.tar.gz`, SHA-256
`c60838d0f422a919b4cfce0357234b91e6dbefcfc532946d32983ed381123f62`.
Extracted references and pin proposals remain outside Git. The existing
[A733 manual pin](cpu-continue.md) is reused. No board access, benchmark or
existing SMP test ran for this component.
