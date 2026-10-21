# Bounded writes require separate storage authority

The [MMC core](../drivers/storage/sunxi_mmc.c) implements a bounded write
primitive for identified SD and eMMC cards. The default entry points of the
[native storage platform](mmc-platform.md) expose read-only sessions, and its
command and FIFO filters reject writes. The
[storage update service](storage-update.md) is the only writer. It binds one
platform instance and owns one granted range, a durable host backup, an intent
and result record per write, and a readback comparison. Only that binding opens
the native CMD24 and FIFO filters. No physical media write has been attempted.

Physical recovery qualification, power-loss durability and boot selection remain
prerequisites for recoverable boot and update. A callback returning success does
not establish those conditions.

## Requests preserve an exact input and range

`sunxi_mmc_write` requires a separate authority descriptor and an immutable
request containing a policy token, first sector, block count, source, byte count
and absolute deadline. Missing authority refuses the request without callbacks
or controller access. The existing initialization and read APIs do not enable
writing.

The core accepts 1–8 whole 512-byte sectors. It checks the complete range against
card capacity and command-address limits before callbacks or MMIO. The current
user area is the only admitted partition. Each sector uses an independent
CMD24; multiple sectors are not an atomic transaction.

Admission copies the request, authority descriptor, actual card/session identity
and at most 4096 payload bytes into private core storage. The transmitted bytes
come from that snapshot. The original source remains borrowed until successful
result collection, and a fault retains it. Descriptor storage may be reused
after admission; source and callback-owned storage must remain exclusive.

The private snapshot adds 4096 bytes, plus request and identity metadata, to
every embedded `sunxi_mmc` object. The result structure also gains write
diagnostics. Consumers must rebuild against these layouts. The operation uses
no allocator or filesystem.

Every active write poll authenticates the core address, controller-session
token, card kind, CID, capacity and card configuration against the copied
identity. The policy provider receives temporary copies of that identity and
the exact request. Mutation or refusal retains the operation. A successful
provider return is followed by a fresh deadline check before controller access.

An eventual native policy provider must resolve the opaque token through actual
owned state. At minimum, that state must bind the physical card and session,
partition, exclusive sector interval, accepted input, durable backup identity
and qualified recovery procedure. It must keep those resources reserved across
partial failures. Neither a caller-supplied range nor a readiness flag can mint
that authority.

## Protection and completion are separate checks

The write profile requires CSD command class 4, a 512-byte write-block length,
and cleared whole-card permanent and temporary write-protect flags. For eMMC,
the active partition must remain the user area, with 512-byte sectors, command
queuing and cache disabled. User-area permanent/power-on protection enable bits
must also be clear.

The core never changes card protection, clears a lock, switches partitions or
toggles cache state. Per-group protection is enforced by the card; a write-protect
or locked-card status causes a retained fault. Physical write-protect policy
belongs to the native authority provider. Successful CSD decoding does not
establish that policy.

A request begins with CMD13 confirming ready-for-data and transfer state. Each
CMD24 then follows this bounded sequence:

1. Require a successful command response before supplying payload words.
2. Submit at most one 512-byte burst per poll, sized from one status sample:
   256 words minus the FIFO fill level, and nothing while FIFO-full is set.
3. Require command and data completion, an empty FIFO, idle controller/card
   busy state and no late controller error.
4. Poll CMD13 until the card reports ready-for-data and transfer state. A
   programming response permits a later bounded status query within the same
   deadline; an unexpected state or error retains the operation.

Data completion observed before every payload byte has been submitted is an
inconsistent write transfer. The core faults before another FIFO write.
The following sector cannot start until the preceding sector's status confirms
completion. The final deadline check precedes publication of a completed result.

`submitted` counts attempted FIFO bytes. A failed burst counts every word it
attempted, because any prefix may have reached the FIFO. `confirmed_blocks` counts sectors followed by
successful ready/transfer status. `write_started` becomes true before attempting
the first command-register write, because a failed callback may still issue it.
These fields diagnose partial progress; they do not prove media contents.

Command/FIFO uncertainty, removal, CRC failure, timeout, changed identity or lost
authority freezes the core and retains the source and all platform ownership.
Result collection copies fault diagnostics without releasing anything. There
is no retry, reset, rollback or implicit continuation through another controller.

READY status does not establish power-loss durability, atomic updates or a
verified backup. The update service supplies readback comparison and the host
intent and result records; boot selection remains unimplemented.

## Sources and focused validation

The A733 user manual revision 1.00 defines the command direction at bit 10 and
FIFO-full status at bit 3 in sections 11.3.8 and 11.3.9. The source references
for protocol behavior are pinned to the board kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`:

| Source | Relevant behavior |
| --- | --- |
| [MMC block requests](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/mmc/core/block.c) | CMD24 selection, byte/sector addressing, write command class and status errors |
| [MMC operations](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/mmc/core/mmc_ops.c) | Bounded status-based programming completion |
| [MMC definitions](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/include/linux/mmc/mmc.h) | Command, error, partition, cache and sector-control fields |
| [Pinned bootloader writes](https://github.com/orangepi-xunlong/u-boot-orangepi/blob/b791be842935b27268ae3d00e943a9075495f30a/drivers/mmc/mmc_write.c) | Single-block command and post-write status sequence |
| [Micron eMMC 4.41 datasheet](https://mm.digikey.com/Volume0/opasdata/d220001/medias/docus/8719/emmc-ps8200-v441-enh-plus-family%20%281%29.pdf) | CSD write length and permanent/temporary protection fields |

The Micron document supplies protocol-field evidence; it does not identify the
board's storage part. Published sources and exact hashes stay in the external
source cache and source lock.

The [focused check](../checks/sunxi-mmc-write.py) uses already-identified card
fixtures and a private model of range authorization. The modeled provider
establishes no physical ownership or backup. It checks one SD byte-addressed
write chain and one eMMC sector-addressed chain, FIFO payloads, programming
status, partial failures and retained ownership. Refusal cases include
protection, cache/partition state, invalid ranges, missing or changed authority,
callback expiry and premature data completion.

One read exercises the affected shared command path. Existing identification
and capacity suites are reused. Compilation covers the expanded core, native
platform/adapter and existing MMC fixtures; compiling a fixture does not execute
its previous workloads.

The focused write/status/ownership cases and affected read passed under
AddressSanitizer and UndefinedBehaviorSanitizer. All three existing fixtures
compiled, and the core plus platform/native adapter compiled for AArch64 SRE.
The expanded platform's PMIC alias-refusal case also passed after its fixture
moved to unused, aligned backing large enough for the object.

Evidence is preserved under
`~/.cache/sunstoneos/evidence/mmc-write-20261016/`. `batch1/` records source
checks, executed write/read cases and compiler-derived dependencies.
`continued/` records the changed alias case and remaining compilation results.
The successful write/read cases were reused after that fixture-only correction.

For the recorded native build, compiler stack reports give direct frames of
864 bytes for core initialization and 672 bytes for platform initialization.
These exclude callees and callbacks; task stack admission must cover the full
call chain. Physical execution and worst-case task stack use remain unqualified.

## Required storage work

Physical recovery qualification of the update service and physical storage
qualification remain prerequisites for recoverable boot. CMD25/CMD23 writes,
reliable writes, cache/flush support, erase, protection management and alternate
eMMC partitions remain required modes. Wider buses, faster clocks, voltage
switching, tuning, DMA, interrupts, 4 KiB sectors, command queuing, hotplug,
recovery and power transitions remain in scope. Performance optimization follows
functional hardware coverage.
