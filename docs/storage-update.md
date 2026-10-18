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
Persistent boot image layout, boot selection, power-loss durability and wider
storage modes remain required work.

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
any write command. The last chunk must terminate the chain.

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
session cutoff. The maintenance watchdog may renew only from completed storage
work and completed USB exchanges under its actual lease. Host filesystem stalls
must fail within that lease; they cannot cause indefinite feeding.

## Uncertainty retains ownership

An incomplete write, failed status, mismatched readback, invalid sequence or USB
loss permanently faults the session. The transport loss latch is checked before
subsequent platform accesses and core I/O. The service retains its source bytes,
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
The native bootstrap, MMC worker and USB task bind actual PMIC, platform and
transport ownership. USB completion is reported to the maintenance watchdog.
Their focused validation and the complete maintenance image remain pending.

The host implementation is `tools/storage-update.py`. Its `prepare` command
constructs artifacts only. A physical launcher must establish actual target,
queue, watchdog and boot authority before using its connected session API.
The artifact constructor used by the host model creates no qualification record.

The focused check is `checks/a733-storage-update.py`. It connects the host code
to the C service and the actual MMC platform, PMIC, rail, clock and pin managers.
Only card media, register access, time and transport are modeled. It reuses the
existing card model without executing its protocol suites. New cases cover
backup files, write/readback, explicit restoration, ownership refusals, candidate
commitment, uncertain writes, transport loss and host durability failures.
Software results do not qualify the physical board.

The connected core/host batch passes under AddressSanitizer and
UndefinedBehaviorSanitizer. Eight streaming SHA-256 boundary vectors match the
host reference. The affected native units and existing embedding fixtures
compile; their old protocol workloads were not rerun. Evidence is preserved in
`~/.cache/sunstoneos/evidence/storage-update-20261017/batch1`. Native task, mutex and
USB execution remain outside that result. The [watchdog composition](maintenance-watchdog.md)
has separate focused evidence; it does not establish complete image execution.
