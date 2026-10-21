# Bounded storage updates preserve a host backup

The storage update service owns one finite range in the normal data area of an identified SD
card or eMMC device. It uses the native MMC platform and its shared clock, pin,
rail and PMIC providers. The existing platform API remains read-only until the
service binds the actual platform instance. A bound platform cannot be returned
to another client. Faults retain its buffers and resource leases.

This implementation is not physically qualified. Independent watchdog recovery,
protected ROM return, the native maintenance launcher and actual card startup
admission remain prerequisites for physical use. A prepared bundle does not
satisfy those prerequisites. No storage writes have been performed by this work.
The persistent boot layout and slot selection are specified in
[boot-slots.md](boot-slots.md); host planning for them exists, and no card has
been written. Power-loss durability and wider storage modes remain required
work.

## Trusted bootstrap grants an exact card range

The immutable launch grant contains the card kind, selected controller, complete
card identification register (CID), capacity, first sector and sector count. It also contains a fresh session
nonce, absolute deadline, allocation identity and recovery evidence identity.
The native service compares these fields with the actual identified platform
session. An application request cannot replace the grant or acquire another
range.

A trusted allocation artifact assigns the exact extent to SunstoneOS. The host
does not infer ownership from unused sectors, partition labels or an empty
filesystem. A write-capable launch additionally identifies protected original
bytes and commits the complete ordered candidate image. A read-only launch
exports a backup and cannot become a writer within that session.

The host is part of the trusted bootstrap. Native firmware cannot observe an
`fsync` on the host or independently establish physical reset qualification.
Hashes identify bytes; they do not establish either property. The production
host preparation path checks the existing independent-reset evidence and its
referenced artifact bytes. No signature requirement is introduced.

## A boot-slot plan becomes one session per changed range

A grant covers one contiguous range, so a boot-card install or update that
changes several ranges needs one backup and one write session for each.
`tools/storage-update.py plan` produces those sessions. It reads four inputs:

- a plan from `boot-slots.py plan` or `boot-slots.py rollback`, with its files;
- the boot-slot allocation record that assigns the slot regions;
- an ordinary-file image of the card's current contents;
- a card record that identifies the card and the board that holds it.

The card record is a JSON object with `kind` 1 (SD), `host` 0 (SDC0), `cid`
as four 32-bit words, `sectors` equal to the image size, and `target` with the
`port` and `sysfs` strings that the recovery qualification names. The planner
never opens a device. It writes a new private directory with mode 0700 whose
files are sealed read-only.

SunstoneOS owns these regions of a microSD boot card:

| Sectors | Region | Assigned by |
| --- | --- | --- |
| 16..495 | Pinned boot0 | Fixed layout |
| 8192 | Slot A header | Fixed layout |
| 16384 | Slot B header | Fixed layout |
| 32800..40959 | Monitor TOC1 package | Fixed layout |
| Slot A and slot B regions, at or beyond 40960 | Slot payloads | Boot-slot allocation record |

No session writes any other sector. In particular, sectors 0..15 hold the
partition table, and the 4 MiB unit at sectors 24576..32767 holds the backup
package sector that the stock U-Boot header names. Whether a stock backup
package exists there is a property of each card, so `plan.json` records the
hash of that unit in the card image and whether it is all zero.

Each write of the boot-slot plan must lie inside its own region, or planning
stops. A range whose card bytes already equal the planned bytes is listed as
skipped. Every other range must still hold the bytes that the boot-slot plan
recorded as its previous contents; a card image that changed since planning
is refused. The remaining ranges become sessions in the boot-slot plan's
order: slot payload, slot header, boot0, then the TOC1 package. Each session
lists:

- its card range, including the hash of its allocation record;
- a candidate file holding the planned bytes;
- an allocation record that assigns that one range to SunstoneOS and
  names its region, the board target and the hashes of the boot-slot plan and
  allocation;
- the hash that a read-only backup of the range must produce.

A session runs in three steps. A read-only capture with a mode-0 grant over
the session's card range produces the backup. `storage-update.py prepare`
then takes that backup, the session's candidate and allocation, and
`--plan DIR/plan.json --session N`. It refuses unless the card range, the
backup hash and the candidate hash all equal the session's values. A
connected `apply` performs the writes.

The order does not make an interrupted install bootable. The container's
boot0 is the pinned stock boot0, and the monitor package replaces the stock
TOC1 at the same sector, so a card left between sessions can hold a boot0
and a TOC1 package that were not packaged together. Recovery uses the explicit restoration below: each session's read-only
backup is the restoration source for its range. After an uncertain session,
capture that range again, then restore it or make a new card image and plan
again.

## Backup bytes precede write admission

The service reads the granted range through the actual MMC platform in chunks
of at most 1,024 bytes. It hashes the complete stream while the platform retains
exclusive controller ownership. Each response binds its sequence and range
offset and includes a chunk digest.

The host creates a private directory with mode 0700. Backup files use exclusive
creation, reject symlinks, and begin with mode 0600. After writing all bytes, the
host synchronizes the file, changes its mode to 0400, synchronizes the metadata
and directory, then reopens and verifies the actual file identity, size and
hash. It closes the writable descriptor and retains the read-only descriptor
and directory lock for the operation. These controls protect ordinary tool
operations; they do not isolate the backup from a malicious trusted host.

The host synchronizes the backup manifest before acknowledging it. Native
admission requires the service's own completed read hash. In a write-capable
session, that hash must also match the immutable launch grant. A backup from an
earlier session therefore cannot silently authorize a changed card image.

## Each write is committed before card commands

The host snapshots the complete candidate and constructs an ordered hash chain.
Each chain entry commits its relative sector offset, byte count, next entry and
actual chunk bytes. The immutable launch grant contains the first commitment.
The service verifies each received chunk against that commitment before issuing
any write command. The last chunk must terminate the chain. Each write chunk
holds one 512-byte sector, and the commitment offset counts sectors.
Prepared artifacts with a two-sector chain no longer match and must be prepared
again; the host refuses them by the size of the chain file.

Before sending a chunk, the host creates and synchronizes an immutable intent
record containing the session, sequence, physical sector and payload identity.
The service copies the bytes into permanent private storage and admits the
exact request through its concrete range provider. The MMC core issues bounded
single-sector writes. Native admission permits CMD24 and FIFO writes only while
that actual request owns the selected controller.

After successful card status, the service reads the chunk back through the same
platform and compares the actual bytes. Only then does it advance the range and
publish a response. The host synchronizes the corresponding result record.
Completion requires the full candidate readback digest. Card status and readback
do not establish power-loss durability, atomic replacement or bootability.

Each request has an absolute deadline of at most one second, bounded by the
session cutoff. A write request covers one CMD24, its card-status polling and
the one-sector readback within that deadline. The SD specification allows up to
250 ms of write busy per block for SDHC and 500 ms for SDXC. At the
identification clock the transfers and status commands are estimated at tens
of milliseconds, so the one-second deadline leaves at least 500 ms for one sector's
programming. Programming that outlasts the deadline faults the request and
retains the uncertain write. The maintenance watchdog requires a constant
operation deadline within each request, so the readback does not receive a
separate restarted deadline. The maintenance watchdog may renew only from completed storage
work and completed USB exchanges under its actual lease. Host filesystem stalls
must fail within that lease; they cannot cause indefinite feeding.

## Uncertainty retains ownership

An incomplete write, failed status, mismatched readback, invalid sequence or USB
loss permanently faults the session. The transport loss latch is checked before
each later platform access and core step. A FIFO burst counts as one access and
holds up to one sector (128 words). Once admitted, the burst reaches the FIFO in
full even when a loss is latched during it. The CMD24 write has already started
by then, so that write is uncertain in either case. The service retains its source bytes,
range and platform resources. It does not retry, release ownership on timeout,
or reset a card into an assumed cold state.

The host also stops after any transport, validation or filesystem failure. A
durable intent without a durable result denotes an uncertain write. Replaying
that request is forbidden.

Restoration is an explicit new operation. A fresh qualified session must
reidentify the card and establish its controller/card startup state. The host
first backs up the current uncertain range, then prepares the protected original
bytes as the candidate for the same bounded write and readback path. Restoration
needs the same recovery qualification as an update. Neither operation starts
automatically after a failed session.

## Integration and evidence

The service lives in `drivers/storage/a733_storage_update.c`. Its concrete
platform writer is separate from the default read-only entry points. The USB
mailbox has one producer and one worker. Its response copy-out returns mailbox
storage to the producer; actual USB IN completion is a separate transport event.
The native implementation in `platform/a733-storage-update` uses three static
FreeRTOS tasks for the shared PMIC provider, storage operations and USB. One
permanent bootstrap object contains their common clock, mutex, pin and rail
managers. The storage task owns the service; the USB task owns framing and actual
IN completion. The maintenance lease binds these same objects and task handles.
The complete [maintenance image and launcher](storage-maintenance-launch.md)
compose these tasks with immutable boot admission and permanent fault revocation.
Their focused instruction and host checks pass; physical operation remains unqualified.

USB preparation acquires real shared supply leases and checks copied snapshots
from the common PMIC provider. The immutable boot record supplies the qualified
voltage targets. Supported selector values alone do not establish safe board
voltages or resolve the schematic's conflicting supply summaries. Provider
faults block subsequent USB register access; transient metadata contention waits
within the existing watchdog bound. A completed mailbox response cannot renew
the watchdog until its entire USB IN transfer has completed.

The host implementation is `tools/storage-update.py`. Its `prepare` command
constructs artifacts only. A physical launcher must establish actual target,
queue, watchdog and boot authority before using its connected session API.
The artifact constructor used by the host model creates no qualification record.

The focused check is `checks/a733-storage-update.py`. It connects the host code
to the C service and the actual MMC platform, PMIC, rail, clock and pin managers.
Only card media, register access, time and transport are modeled. It reuses the
existing card model without executing its protocol suites. New cases cover
backup files, write/readback, explicit restoration, ownership refusals, candidate
commitment, uncertain writes, transport loss and host durability failures. A
card model that stays in programming state for 500 ms after each CMD24 completes
a two-sector update; one that stays busy for one second retains the first write
as uncertain.
The card model holds only the granted range of an SD card whose capacity
equals the grant's. Planning cases build a 64 MiB card image with a partition
table, boot0, a nonzero backup package unit and a TOC1, then plan a slot A
install with a synthetic container. They check the four planned sessions and
their sectors, run a connected backup and update for each, and require slot
A to verify with every other sector unchanged. Planning again on the result
skips every range. Further cases refuse a changed card, a write outside its
region, a card record for another kind, and preparation against another
session's range.
Software results do not qualify the physical board.

The connected core/host batch passes under AddressSanitizer and
UndefinedBehaviorSanitizer. Eight streaming SHA-256 boundary vectors match the
host reference. The affected native units and existing embedding fixtures
compile; their old protocol workloads were not rerun. Evidence is preserved in
`~/.cache/sunstoneos/evidence/storage-update-20261017/batch1`. Native task, mutex,
USB and watchdog execution remain outside that result. The
[watchdog composition](maintenance-watchdog.md) has separate focused evidence;
it does not establish complete image execution.

The additive check `checks/a733-storage-native.py` covers the new supply
profiles, native C write-admission guards and USB framing/provider guards. It
models CPU privilege, task ownership, register access and completed USB
preparation. It compiles the original AArch64 adapter and native task units.
It does not execute bootstrap or establish physical recovery. The full host
backup, update and restoration workload is reused from the connected batch.

The additive cases pass under AddressSanitizer and UndefinedBehaviorSanitizer.
Evidence in `~/.cache/sunstoneos/evidence/storage-update-20261017/native1`
contains the successful PMIC and MMC guard checks. The successful USB guard
check and four original AArch64 compilations are preserved in `native2`.
The USB fixture uses aborting stubs for unexecuted task entry points retained by
sanitizer metadata. The merged PMIC source includes
the display supply profile alongside the two new USB supply profiles.
