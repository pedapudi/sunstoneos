# GPU firmware initialization records and resource mappings

Native preparation of firmware initialization records is **implemented**;
no host check remains. It connects the command queues to system, OS, power, recovery,
trace, and timing resources. The prepared records have not been used by a
physical GPU. Firmware startup, platform integration, and qualification remain
outstanding.

This document assumes familiarity with page tables and firmware interfaces.
The [resource layout and preparation API](../include/pvr_fw_state.h) supports
the pinned A733 firmware profile: revision 36.56.104.183, one GPU cluster,
16 KiB system-level cache, and firmware ABI major 1. Callers must validate
the firmware identity and hardware separately. They must supply the configured
GPU clock in hertz; a CPU clock or advertised peak rate is not a substitute.

## Fixed initialization records point to backed resources

The firmware configuration area occupies the top 192 KiB of its 16 MiB heap.
The native layout reserves these required addresses:

| Record | MIPS address | Record bytes | Allocated bytes |
| --- | --- | ---: | ---: |
| Connection control | `0xc0fd0000` | 16 | 4,096 |
| OS initialization | `0xc0fe0000` | 104 | 4,096 |
| System initialization | `0xc0ff0000` | 544 | 4,096 |

Other resources occupy `0xc0100000..0xc0129fff`. The allocator must reserve
that range, firmware code/data, and the remaining configuration and reserved
heap areas. The resource getter reports every object's address, logical size,
and page-rounded allocation size.

The nineteen resources comprise five queue regions, two synchronization words,
three recovery/system/OS data records, a fault page, utilization and runtime
records, three trace resources, and the three fixed initialization records.
They occupy 45 pages, or 184,320 bytes. With the pinned firmware code/data
and four MIPS page-table pages, the currently prepared allocation totals
323,584 bytes. GPU application memory contexts and workload resources are additional.

The [builder](../drivers/gpu/pvr_fw_state.c) accepts CPU buffers and explicit
physical-page lists. It checks exact sizes, CPU overlap, physical overlap,
alignment, address width, and wrapper-intercepted physical addresses before
changing any buffer. It returns uncached mappings for the complete allocations.
Discontiguous physical backing and addresses above 4 GiB are supported.

The platform must still prove RAM ownership, CPU/physical correspondence,
physical aliases with input storage, and device addressability. Passing the
returned mappings to [MIPS preparation](gpu-mips.md) checks their placement
against firmware code/data and the page-table backing.

## Initialization preserves queue ownership and reference defaults

Queue contents remain unchanged. The [command transport](gpu-queues.md) owns
their formatting through `pvr_ccb_init()`. The builder zeroes the complete
allocations for the other fourteen resources, including padding, before
writing initialization fields.

The OS record points to the queue controls, commands, return statuses, recovery
data, and OS data. Its compatibility records start with layout version 3 and
zero hardware/firmware identities, which firmware must populate.

The system record points to runtime, trace, system-data, and utilization
resources. It contains the fault page's physical address, the reference PDS
and USC execution-heap bases, the supplied GPU clock, and the startup marker.
The PDS and USC are GPU program execution units; their heap bases describe GPU
virtual addresses and do not create application mappings by themselves.
The firmware-started flag remains clear until firmware writes it.

The fault page contains the reference diagnostic word `0xdeadbee0`. Runtime
configuration records the supplied clock and one-cluster profile. Trace
control describes two buffers of 12,000 words each. Their host-only pointer
fields refer to the supplied CPU buffers and must be updated if those buffers
move. Logging, firmware performance counters, and the firmware watchdog start
disabled, matching the reference initializer. Independent board watchdog
protection remains a separate prerequisite for physical access.

Preparation performs no allocation, cache publication, MMIO, or queue activation.
It is valid only while the device cannot access these records. A failed call
preserves every resource and the mapping output. Live reset and restoration
need a separate protocol that preserves firmware diagnostics until collected.

## No host check covers the complete prepared state

No host check combines state preparation, queue formatting, firmware staging
and MIPS page-table construction over discontiguous physical pages above 4 GiB,
walks all resource mappings back to their physical pages, or compares the 19
complete allocations byte for byte with the original initialization functions.
The combined table holds 75 mapped pages: 25 code, five data and 45 resource
pages. By design record preparation leaves queue contents unchanged, and 36
atomic refusals cover invalid profiles, missing backing, sizes and overlaps.

The native component compiles for AArch64 with strict alignment and
freestanding options. It contains 2,400 bytes of code/read-only data, no
writable static data, and a 96-byte preparation frame. No device access or
performance measurement has occurred.

The recorded results of the removed host checks are in [the evidence record](evidence/gpu-fw-state.jsonl).
