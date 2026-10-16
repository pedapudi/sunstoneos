# A733 system IOMMU: bounded camera mappings and cold installation

The native [table builder and controller](../drivers/iommu/a733_iommu.c) implement
page mappings, a polled initial configuration, and fault capture for the A733
system input/output memory management unit (IOMMU). The IOMMU translates device
addresses into physical RAM addresses and checks device read/write permissions.
This component has host and source-comparison evidence. It has no physical
qualification or native platform handoff.

The initial controller profile translates camera master 1. All other masters
remain excluded under a retained global lease. The component cannot replace an
enabled inherited directory, release that lease, remap live tables, or reclaim
buffers. Independent hardware watchdog and ROM-return qualification remain
prerequisites for physical IOMMU access.

## Two banks serve ten physical masters

The register banks start at `0x03900000` and `0x03910000`, within a 128 KiB
register resource. Each bank has one directory base and a shared second-level
translation lookaside buffer (TLB), which caches address translations. Each
master has a separate first-level TLB and bypass control. Linux installs the
same directory into both banks; the native initial profile follows that model.
There are no per-master address spaces in this implementation.

| Global ID | Bank | Local ID | Master | Micro-TLB offset from `0x03900000` |
| --- | --- | --- | --- | --- |
| 0 | 0 | 0 | USB0/USB1 | `0x00400` |
| 1 | 0 | 1 | CSI camera input, including VIPP | `0x00600` |
| 2 | 0 | 2 | ISP | `0x00800` |
| 3 | 0 | 3 | Video encoder | `0x00a00` |
| 6 | 1 | 0 | Video decoder 0 | `0x10400` |
| 7 | 1 | 1 | Video decoder 1 | `0x10600` |
| 8 | 1 | 2 | Display engine 0 | `0x10800` |
| 9 | 1 | 3 | Deinterlacer | `0x10a00` |
| 10 | 1 | 4 | G2D | `0x10c00` |
| 11 | 1 | 5 | E-paper engine | `0x10e00` |

IDs 4 and 5 are absent and must not receive register accesses. The real-master
mask is `0x0fcf`. Global IDs use six slots per bank, including the two absent
slots. The device tree gives interrupt numbers SPI 170 and 171, version `0x16`,
prefetch value `0x3007f`, and range-invalidation mode 1 for both caches.
The version value comes from source configuration, not a physical register read.

The A733 manual, printed page 406, confirms the two banks, their master groups,
16 GiB address spaces, independent bypass, and shared second-level TLBs.
It does not supply the IOMMU register or page-table descriptions. Its reference
to “MMU-600” does not establish an Arm SMMU programming interface; this driver
uses the board's Allwinner IOMMU v2 implementation.

The clock/reset controller has separate IOMMU0 and IOMMU1 gate/reset registers
at offsets `0x058c` and `0x05b4`. Each has system H/P/M clock gates and a system
reset. The device tree also assigns master power domains. Accessing another
master's micro registers requires that domain to be accessible. Camera-domain
ownership alone cannot authorize accesses to the USB, video, or display paths.

## Page construction has explicit size and address limits

The [public interface](../include/a733_iommu.h) accepts up to eight disjoint
page-aligned spans. Each specifies device address, physical address, length,
and device read/write permissions. Up to four distinct 4 MiB device-address
windows can be represented. These capacity limits bound the initial camera
profile; broader allocation and multi-client policies remain implementation work.

The directory has 4096 32-bit entries and occupies 16 KiB. Each second-level
table has 1024 32-bit entries and occupies 4 KiB. The caller supplies one
exclusive 32 KiB arena, aligned to 16 KiB in both CPU and physical address space.
The builder emits the directory followed by four table slots. Unused entries
and unused slots are zero.

A device address uses bits 33:22 for the directory index, bits 21:12 for the
second-level index, and bits 11:0 for the page offset. Directory entries contain
the table address and valid bit 0. Leaf entries contain the page address,
valid bit 1, read bit 2, and write bit 3. Both formats put physical address bits
33:32 into entry bits 9:8. Directory permissions are not populated: the pinned
allocator computes permission flags but does not install them in the entry.

The builder accepts device addresses from `0x1000` through `2^34 - 1` and CPU
physical addresses from `0x40000000` through `2^34 - 1`. Complete spans must fit
inside those bounds. Linux also describes a high-DRAM alias above 16 GiB; its
boundary conversion requires separate verification and is refused here.
The cold camera controller further restricts device spans to addresses below
4 GiB, matching the camera allocation mask.

The builder rejects incomplete pages, excess table capacity, address overflow,
physical aliases between mappings, and mappings over its own table storage.
It validates all inputs before modifying the arena or output binding.
Canonical validation compares every directory and leaf word with the admitted
spans, including unused entries. The platform must additionally reserve the
CPU aliases and prove that every physical span is authorized RAM.

For the current camera path, the video image postprocessor (VIPP) parameter
image occupies a separate 4 KiB read-only page. Each 640 by 480 NV16 plane uses
307200 bytes, or 75 write-only pages. Parameter, Y, and UV mappings remain
separate. A device-specific tail or prefetch requirement must be established
before choosing additional mapped guard space. An unmapped adjacent page detects
an out-of-bounds access but does not prove that a device will avoid that access.

## Installation requires resolved global ownership

The platform admission callback acquires a stable nonzero token before its
first side effect. Pending admission and failures retain that token. Completed
admission must establish all of the following:

- Exclusive control of both IOMMU banks and all ten real masters.
- Exclusion of every master, with outstanding bus transactions and page-table
  walks independently drained.
- Resolved inherited firmware ownership, including table and buffer lifetimes.
- Accessible master power domains, retained clocks/resets, and IRQ ownership.
- Exclusive CPU/table/buffer reservations and an established CPU-to-physical
  alias for the table arena.
- Qualified independent watchdog recovery and ROM return.

No current platform adapter supplies this proof. Clearing an enable register
or observing an idle camera cannot establish it. The callback performs resource
admission and memory validation; native code constructs and installs the tables.

After admission, the controller refuses either enabled bank or existing bank
or master fault status. Thus even a completed admission cannot replace active
inherited mappings. It publishes the complete table arena to the point of
coherency before writing either directory base. The publication callback must
complete cache cleaning and its barrier before reporting success.

The controller writes both directory bases, prefetch controls and invalidation
modes. It disables IOMMU interrupts for polled operation, selects translation
for CSI, and selects bypass for the remaining masters. It then follows the
source's initial global flush, programs automatic clock gating, and enables
both banks. Ordinary configuration writes receive readback checks. Each poll
performs at most one platform or register operation and checks monotonic time
before and after it.

ACTIVE means this initial sequence completed. The higher-level global owner
may then admit CSI traffic while retaining every other master's exclusion.
The tables, clocks, domains and global token remain owned. Bypass does not
isolate another master from RAM; allowing a bypassed master to run requires
its own memory and resource policy.

## Cache invalidation does not establish safe reclamation

The source flush writes 3 to each bank's global flush request and 1 to every
real master's micro-TLB flush request. It waits 10 microseconds and clears the
global requests. It does not check a completion bit. The native controller
preserves this initial sequence and the minimum delay, with bounded polling.
It does not infer that the delay proves drain or permits memory reuse.

The source also has separate range invalidations for the TLB and page-table
walk cache. TLB range endpoints encode addresses shifted by 12; walk-cache
endpoints encode addresses shifted by 22. Each request uses the same fixed
delay followed by an explicit clear. These operations do not provide an
independent completion or reclamation contract, so live remapping and unmapping
remain unsupported.

Page-table publication is distinct from payload cache maintenance. Camera
memory admission must prepare frame pages for device writes and the parameter
page for device reads. CPU access to captured data requires producer stop,
controller disable, independent bus drain, final error checks and cache
synchronization. The camera owns those operations. A capture-done bit cannot
release the IOMMU lease or table memory.

A system reset affects the bank and its shared translations. The source also
resets master bits at bank offset `0x10` during fault handling. Neither form of
reset proves that external bus transactions have retired. The native component
performs no reset, automatic recovery, or fault acknowledgment.

## Fault snapshots retain diagnostic and ownership state

Fault capture reads both bank status registers, both directory/leaf walk-status
registers, both walk error addresses and data words, and every real master's
status, address and data. Array order follows the master table above. High and
low address words remain separate to preserve the raw evidence.

The source identifies bank status bits 16 and 17 as directory and leaf walk
faults; low bits identify local masters with permission faults. The native core
retains any nonzero bank, walk, or micro status as a fault, including statuses
whose interpretation needs more evidence. Captures are sequential samples,
not atomic snapshots. A failed read retains partial data with `complete` clear.

A fault retains the global token and all resources. Further diagnostic reads
are allowed while the admission proof remains valid; an empty later snapshot
cannot restore ACTIVE. The integration layer must continue holding power and
clock leases and must serialize every caller.

## Sources and focused evidence

All Linux anchors refer to revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` in
[the board kernel repository](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/iommu).
The source cache is `~/.cache/sunstoneos/sources/iommu/`.

| Source | Basis |
| --- | --- |
| `sunxi-iommu-v2.c:40–155, 460–546, 634–769` | Registers, two banks, master bypass/reset, initial flush, directory programming, range invalidation |
| `sunxi-iommu-v2.c:1246–1408` | Fault addresses, status meanings, source acknowledgment/reset behavior |
| `sunxi-iommu-pgtable.h:109–151` | 34-bit address geometry and high-DRAM alias conversion |
| `sunxi-iommu-pgtable.c:25–68, 114–173, 193–235` | Entry formats, directory allocation and leaf encoding |
| `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi:418–518` | Master IDs, skip entries, clocks, resets, IRQs and invalidation configuration |
| A733 User Manual V1.00, printed pages 97–100, 406 | Clock/reset controls and IOMMU topology |

The manual pin is `a733-user-manual-1.00`, SHA-256
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
The reference check verifies its three Linux input hashes before compiling
isolated source functions. Pin proposals for central integration accompany the
external evidence; this component does not modify the central source lock.

The changed risks are incorrect permissions or addresses, publishing incomplete
tables, overwriting inherited global state, and releasing resources after an
unproven flush. The focused [host check](../checks/a733-iommu.c) independently
walks published page tables, including a physical-address transition across
4 GiB. It checks adjacent unmapped pages, permissions, atomic refusal, all-master
admission, cache publication, dropped writes, deadlines, time regression,
partial admission and retained faults. AddressSanitizer and UndefinedBehaviorSanitizer
pass. The core also compiles freestanding for AArch64.

The [source comparison](../checks/a733-iommu-reference.py) compiles unchanged
Linux initialization, bypass, flush and leaf-encoding functions. All 128 KiB of
modeled registers match except the explicit polled interrupt masks. All 180
leaf entries match across physical-address extensions and permissions.

Raw sources, executables, generated reference harnesses, logs and snapshots are
under `~/.cache/sunstoneos/evidence/iommu-20261016/`. The source comparison in
`reference1` remains valid after the admission and fault-reporting changes;
the compared table and initialization operations did not change. Final host
and AArch64 evidence is recorded in `snapshot2`. No physical accesses ran.

Integration must provide native memory/cache/MMIO operations and global
resource admission before connecting the camera. Qualification must then cover
translation faults, recovery, every other master, shared-bank concurrent traffic,
power transitions, buffer lifetime, and an independently established invalidation
completion contract. These remain hardware-support requirements.
