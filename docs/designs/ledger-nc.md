# Non-cacheable device memory from the unified ledger

Status: **implemented and host-tested**. The linked instruction model checks
conversion, cache maintenance, translation invalidation and USB payload aliases.
The USB memory path has no board result. One empty-port run on port 3-1 remains
pending physical recovery. [The records](../evidence/ledger-nc.jsonl) identify
the tested images and the limits of the models.

Readers are assumed to know AArch64 stage-1 translation, cache maintenance and
the [unified ledger](unified-memory.md).

## Free chunks change policy before allocation

`os_owner_memory(..., OS_MEMORY_DEVICE)` requests zeroed Normal non-cacheable
(NC) memory. `os_owner_memory_below()` adds an exclusive physical limit for a device
whose DMA reach is narrower than the CPU's. USB uses the current 4 GiB working
limit. The ledger holds ownership for the session; its child allocators retain
their existing device-drain and loan-release contracts.

The ledger removes a wholly free 2 MiB chunk from its free lists under its
critical section. `ledger_convert()` refuses a chunk outside the admitted map
or one still covered by a private application level-1 block. The policy layer
validates every registered descriptor's physical address and old attribute
before performing maintenance. A live allocation anywhere in the chunk prevents
conversion, including a request for a different free page in that chunk.

The conversion sequence is:

1. Clean and invalidate every line through the old mapping with `DC CIVAC`,
   completing the operation before any descriptor changes.
2. Clear the valid bit in every kernel and private application descriptor.
3. Complete `DSB ISH; TLBI VAAE1IS; DSB ISH; ISB` for all ASIDs at the chunk.
4. Write the descriptors with the new attribute, then complete `DSB ISH; ISB`.
5. Publish the chunk's new ledger policy and satisfy the allocation.

A refused cache operation leaves the descriptors and ledger policy unchanged.
No maintenance failure occurs after the make step. Cacheable walks consume the
cacheable table stores after `DSB ISH`. The explicit uncached-walk build also
cleans descriptor stores before completing their barrier.

## Table access preserves the monitor boundary

The OS maps only its EL1 root, slot-1 level-2 page and USB level-3 pages through
a write-back (WB) alias at `0x79e00000`. The pages are EL1-only and execute-never.
The monitor's identity block remains invalid at EL1. The alias's own immutable
level-3 page participates in the cold Non-secure flush and checksum on every
core. All cores complete entry before the OS starts editing these tables.

Lazy ledger initialization precedes USB construction and application admission.
It reserves every manifest-pinned region first. Eleven level-2 pages, one per
upper gigabyte, then come from the end of the largest free write-back extent.
This takes 45,056 bytes without fragmenting the low aligned free range. Each
mapped upper slot splits before any application root exists.

Application admission reads the live kernel tables. `app_space_build_tables()`
preserves each block's cache policy when it copies a level-2 table, and the OS
registers every private copy for later conversion. A wholly owned, uniformly
write-back gigabyte retains its level-1 application mapping. These regions stay
allocated until session end; the hook refuses conversion while such a mapping
exists. Runtime splitting of an application level-1 block is not implemented.

## USB backing follows the manifest

A host manifest declares `usb.devices` from 1 to 127, defaulting to 24.
The limit is the address count of a USB bus; controller admission also checks
its actual slot capability. Images sharing the OS and TinyUSB objects must
declare the same positive count. A control without a host device supplies no
count and does not constrain those shared objects.

The parent lease contains context pages, endpoint rings, copy buffers and the
4 MiB payload window. Its geometry follows the device and endpoint counts in
`a733_usb_memory.h`. The ledger allocation also includes the existing retained
JPEG leases and rounds to complete 2 MiB chunks:

| Device records, four endpoints each | Parent bytes | Ledger backing |
| --- | --- | --- |
| 24 | `0x4fe000` | 8 MiB |
| 127 | `0x904000` | 14 MiB |

The boot constructor obtains NC backing below 4 GiB and validates its identity
translations. It then binds only the payload pages at
`0x7a040000..0x7a440000`, read-only at EL0 and execute-never. Contexts, rings,
guard pages and retained JPEG buffers have no EL0 alias. The alias update uses
break-before-make and all-ASID invalidation before publication of the typed
pool. A constructor refusal retains the allocation and prevents another attempt.

HID parser state is CPU-only and comes from cached kernel ledger memory when
the generic host stack binds. Its size follows the device and HID-instance
counts. Zeroing completes before host publication and TinyUSB initialization.
Allocation failure prevents initialization; later failure retains the state.
The linked symbol is one pointer, including in the 127-device image.

OS images reserve no linked USB arena and accept no legacy
`SS_EXTENT_USB_MEMORY` geometry record. Legacy diagnostic images retain their
separate fixed profile. The application's packed ABI is unchanged.

## Validation and remaining qualification

`checks/ledger-nc.py` executes the real ledger hook and CPU adapter, including
all 32,768 maintenance operands per chunk, ordering, failure atomicity, private
copies, pinned extents and refusal of a chunk in use. Omission controls remove
cache maintenance or translation invalidation and must fail the model.

`checks/usb-ledger-memory.py` checks manifest counts, shared CMake composition,
typed parent leases and refusal paths at 24 and 127 records.
`checks/usb-hid-ledger.py` checks cached parser allocation, callback readiness
and retained failure paths at both counts.
`checks/usb-ledger-alias.py` executes the cold table builder, OS constructor,
translation readbacks and alias update in a linked USB image. It verifies
payload-only permissions and the absence of a linked NC arena.

The models establish instruction execution and software ownership, not physical
cache visibility or controller DMA. The required empty-port board run remains
unexecuted. Attached-device traffic, repeated lifecycle and the separate
manifest declaration for serial slot depth remain open.
