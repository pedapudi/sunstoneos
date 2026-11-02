# General storage write service for the microSD card

Applications write files on a FAT32 volume of the microSD card through a RAM
write-back buffer. The service flushes in large writes that end on the card's
allocation-unit boundaries, changes the FAT and the directory only at sync
points, and orders every sync so that a power cut at any sector write leaves a
volume that recovers to a state `fsck.fat -n` reports clean, with every
synced byte intact. POSIX-like calls over the service let storage code written
for FatFs and newlib stdio (for example mimus on ESP32 parts) run unchanged.

**Status.** The write service, the POSIX-like layer and the newlib system calls
are implemented and host-checked (section [Host evidence](#host-evidence)). The
card adapter is not implemented: it needs multi-block writes with descriptor
DMA (CMD25) and the SD Status read (ACMD13) in the SD core, a read-priority
arbiter, and the runner admission for bounded writes (section
[Remaining work](#remaining-work)). No physical card has been written through
this service.

## Why the design avoids small and repeated writes

A microSD card has its own controller that maps logical blocks to flash and
levels wear, so the operating system does none of that. What wears the card and
stalls it is write amplification: a write smaller than, or not aligned to, the
card's allocation unit (AU) makes the controller read, modify and rewrite a
whole erase unit. The SD Physical Layer Simplified Specification, version 9.10,
defines the AU in the SD Status register (section 4.10.2, Table 4-44, bits
431:428 `AU_SIZE`; section 4.10.2.4, Table 4-47 lists 16 KB to 64 MB; Table 4-48
gives the maximum for each capacity, 4 MB up to 32 GB and 64 MB above). The host
reads SD Status with ACMD13. A card in a UHS-I bus mode reports the AU for that
mode in `UHS_AU_SIZE` (section 4.10.2.9), and the host uses that field instead.
Section 4.3.4 states that a multiple-block write (CMD25, WRITE_MULTIPLE_BLOCK)
must be used rather than a sequence of single-block writes for speed, and section
4.3.4 also requires a 512-byte block length for block writes.

Three kinds of metadata are rewritten often by a naive writer and each maps to
the same few sectors: the FAT entries of the file being extended, the file's
directory entry (size and modification time), and the FSInfo sector (free
count and next-free hint). The service writes each of them only at a sync point
and writes FSInfo only at a checkpoint or unmount.

The card sits on a robot that can lose power at any moment, so the ordering of
the writes matters as much as their number.

## Layers

| Layer | File | Role |
| --- | --- | --- |
| Device | `struct fwr_device` in `include/fat32_rw.h` | Sector read, multi-sector write, optional yield hook, AU, largest single write |
| Write service | `drivers/storage/fat32_rw.c` | Mount and recovery, directories, reservations, buffering, flush, sync, replace, rename, unlink, truncate, wear counters |
| POSIX-like calls | `drivers/storage/sunstone_fs.c`, `include/sunstone_fs.h` | Descriptor table, errno values, `open`, `write`, `fsync`, `rename`, `ftruncate`, `posix_fallocate` |
| newlib system calls | `drivers/storage/sunstone_fs_syscalls.c` | `_open`, `_write`, `_read`, `_lseek`, `_fstat`, `_stat`, `_unlink`, `_rename`, `fsync`, `ftruncate`, `posix_fallocate` |

The service has no dependency on the SD controller. A subsystem core never
assumes one card: the AU, the largest single write and the sector count come
from the device description, which the card adapter fills from the actual card.
The service does not edit `fat32_ro.c`; it parses the volume itself because the
read-only reader refuses a volume whose dirty flag is set, which is the state
recovery must handle.

## Volume layout and the writable regions

The service parses the boot sector, the FAT size and copies, the FSInfo sector
and the root cluster following the Microsoft *FAT32 File System Specification*
(2000-12-06). It supports 512-byte sectors, one or two FAT copies, and refuses
FAT12 and FAT16 by cluster count.

Every device write passes `guard`: the range must lie wholly inside the first
FAT copy, the second FAT copy, the FSInfo sector, or the data area at or after
an optional floor sector. The boot sector, the reserved sectors, sectors before
the floor, any range that spans two regions and any range that wraps are
refused with `FWR_REFUSED` before the device is called. `fwr_write_allowed`
exposes the same test. The floor lets the board runner confine a first write to
the directory and clusters it named.

## Write path

A file object owns a RAM buffer supplied by the caller.

1. `fwr_write` copies bytes into the buffer and returns the count accepted. It
   never touches the card and never allocates. A full buffer returns a short
   count; a real-time producer treats zero as would-block.
2. `fwr_service`, called by the writer task, flushes when at least one unit is
   buffered. The unit is the AU, or when the buffer is smaller than two AUs the
   largest divisor of the AU that fits half the buffer (an AU is a multiple of
   its divisors, so a write that ends on the divisor never straddles an AU in
   the middle). A non-final flush writes only whole sectors up to the last
   boundary that is a multiple of the unit in absolute card sectors, so every
   write ends on an AU boundary. The remainder (less than one unit) moves to the
   front of the buffer.
3. The first write after a sync that left a partial AU starts at the first
   unwritten sector, mid-AU, and ends at the next boundary. That sector is
   rewritten with its earlier bytes, which the buffer keeps for this purpose
   (the buffer always begins at a sector start).
4. A write larger than the card's single-write limit splits into pieces; when
   the limit is at least one AU the split falls on AU boundaries. The device
   `yield` hook runs before each piece (see [Real time](#real-time)).

Buffer sizing: two AUs or more keeps flushes aligned. A smaller buffer still
works and flushes on a divisor of the AU. A buffer smaller than the distance to
the next boundary flushes unaligned once when it is full, so the writer never
stalls.

## Preallocated, append-only files

A file reserves clusters in memory, not in the FAT. A reservation is a short
list of runs (up to eight) held by the open file; the allocator treats those
clusters as occupied. `fwr_open` with a preallocation size, `fwr_preallocate`
and the growth step (16 MiB by default) reserve contiguous runs where the FAT
has them: first by extending the file's last cluster, then by scanning the FAT
from the next-free hint for a run of the wanted length, then the longest run.

Because the FAT does not change at reservation, an interruption leaves nothing
to undo, and releasing the unused clusters at close is dropping the in-memory
list. A reservation does not survive a power cut and does not shrink the free
space the card reports.

Files grow by appending at their end. There is no seek into written data; the
POSIX layer refuses a seek on a writable descriptor and refuses to reopen a
non-empty file for in-place overwrite.

## Sync order

`fwr_sync` makes everything written so far durable. The order, with the
write each step issues:

1. The volume dirty flag. Before the first metadata write of a session the
   service clears the clean-shutdown bit (bit 27, `ClnShutBitMask`) of FAT[1]
   in FAT copy 1, then copy 2. FAT32 defines this bit in the FAT Data Structure
   section of the FAT32 specification, and `fsck.fat` reports "Dirty bit is set"
   when it is clear (checked on the host against dosfstools 4.2).
2. Data: the buffered bytes, padded to a sector, to the reserved data clusters.
   These clusters are still free in the FAT, so a cut here loses only unsynced
   bytes.
3. The FAT chain for the clusters that now hold data: first the new clusters'
   entries (copy 1, then copy 2), then the link from the file's old last
   cluster to the first new cluster (copy 1, then copy 2). The new chain is not
   reachable until the link is written, and the link is written after the
   chain is complete.
4. The directory entry: size, first cluster and times, in one sector write. A
   new file's entry is created here, at its first sync; until then the file has
   no directory entry. The entry is placed in a run of free slots that fits one
   sector, so a name with long-name entries never straddles two sectors. When
   the directory has no room the service first writes a zeroed cluster, then
   its FAT entry, then links it to the directory, then writes the entry.
5. FSInfo: at `fwr_checkpoint` and `fwr_unmount` only, because recovery
   recounts it, never at a file sync. Then, when no open file has unsynced
   state, the clean bit is set again in FAT copy 2 first, then copy 1. Mount
   reads the flag from copy 1, so copy 1 is cleared last and set first.

The ordering follows the reference behaviour of Linux `fs/fat`: file data is
written back before the inode and FAT blocks that name it when a file is
synced (`fat_file_fsync` in `fs/fat/file.c`); the dirty state is recorded at mount and cleared at unmount (`fat_set_state` in
`fs/fat/inode.c`, which records it in the boot sector's state byte; this
service records it in FAT[1], the field that `fsck.fat` and Windows also read);
and FSInfo is written at sync and unmount (`fat_clusters_flush` in
`fs/fat/misc.c`). A deleted entry marks the long-name entries and then the short
entry (`fat_remove_entries`), and so does `entry_delete` here. The Linux
sources are not in the local cache; this paragraph cites function names only.

The cost of a sync is one write for the partial AU, one or two FAT sector runs
(two copies), one directory sector, and on the first metadata write of a session
two FAT[1] writes. A sync that changed nothing writes nothing.

## Recovery at mount

A volume whose FAT[1] clean bit is clear is recovered by `fwr_mount`, which needs
a bitmap of `(clusters + 2) / 8` bytes (about 240 KB for the 62 GB card's 1.9
million clusters). Recovery:

1. walks every directory from the root and claims each file's chain in the
   bitmap, up to the number of clusters its size requires;
2. truncates a chain that is longer than its size (the chain was written before
   the entry, so the extra clusters hold unsynced bytes) and shortens the size
   of an entry whose chain is shorter than the size; a chain that reaches a free,
   bad, out-of-range or already claimed cluster ends there; a directory whose
   first cluster is invalid is deleted;
3. resolves an interrupted rename (see below);
4. frees every cluster with a nonzero FAT entry that no entry claims;
5. rewrites any FAT 2 sector that differs from FAT 1;
6. recounts the free clusters, sets the next-free hint, rewrites FSInfo, and
   sets the clean bit last.

Every step is idempotent and the dirty flag stays set until the last write, so a
cut during recovery is recovered again.

Limits: recovery keeps at most 256 pending directories on its stack and returns
`FWR_UNSUPPORTED` beyond that.

## Atomic replace and rename

`FWR_O_REPLACE` writes a new body that replaces a path. The new file has no
directory entry. The first `fwr_sync` (and so `fwr_close`) writes its data and
chain, then switches the path in one directory-sector write: the existing
entry's first cluster, size and times change in place, or a new entry is
created when the path did not exist. After the switch the old chain is freed. A
cut before the switch leaves the old content, a cut after leaves the new
content. Between the data and the switch the new clusters are chained but
unreferenced, and recovery frees them.

`fwr_rename(from, to)` works inside one directory (a rename across directories
returns `FWR_UNSUPPORTED`; the POSIX layer returns `EXDEV`). When `to` exists
it is replaced:

1. tag the source entry (bit 7 of the entry's reserved byte at offset 12, which
   the entry never otherwise uses);
2. switch the destination entry to the source chain and size in one sector
   write (the commit point), or create the destination entry when it did not
   exist;
3. delete the source entry (long-name entries first, then the short entry);
4. free the old destination chain.

A cut between steps 2 and 3 leaves two entries naming one chain. Recovery finds
the tagged entry, sees an untagged entry with the same first cluster, and
deletes the tagged one; a tagged entry without a twin only has its tag removed.
Rename of an empty file is not tagged and may leave both names if cut between
the steps. Rename of a directory is not supported.

## Real time

The writer runs as a low-priority task. The application never waits for the
card on a real-time path: `ss_write_nowait` copies into the buffer and returns,
and the writer task calls `ss_fs_service` to flush.

The service holds no interrupt-masking spans and takes no locks of its own; it
is owned by one task. The device `yield` hook runs before each write piece,
at most `max_write_sectors` (256 sectors, 128 KiB, with descriptor DMA) apart.
The card adapter binds it to a simple read-over-write priority: when a model
loading read is pending the writer waits for it before submitting the next
piece, so a read waits at most for one piece already in flight. A flush is
therefore never longer than one piece on the read side. Interrupt-masked spans
over 5 microseconds are defects (the scheduler stream's target); the service
adds none, and the adapter's interrupt path remains the SD core's.

The card's own busy time after a write is not bounded by the host; a read that
arrives while the card programs must wait for the card to return to the
transfer state.

## Wear reporting

`fwr_wear` returns counters that applications read: bytes written (data and
metadata separately), device write calls, flushes, syncs, writes that start and
end on AU boundaries, other writes, a histogram of write sizes (bucket `i`
counts writes of `2^(i-1)+1` to `2^i` sectors; bucket 0 is one sector), and the
recoveries run and the clusters they freed. The service reads no vendor command.
The SD Status register has no standard wear-level field; a card health status
is reported only if a standard command provides it, and none does in the
specification version used here.

## POSIX-like calls and the FatFs and ESP-IDF mapping

`include/sunstone_fs.h` provides the calls below. Each returns a non-negative
value or a negated `errno`. The newlib system calls wrap them, so `fopen`,
`fwrite`, `fflush`, `fclose`, `rename` and `remove` work. Descriptors 0 to 2
go to a console hook.

The comparison column is the ESP-IDF v6.1 VFS over FatFs R0.16 at the revision
pinned in `lib/fatfs/source.json`
(`components/fatfs/vfs/vfs_fat.c`).

| Call | SunstoneOS behaviour | ESP-IDF / FatFs behaviour | Difference to know |
| --- | --- | --- | --- |
| `open(O_WRONLY\|O_CREAT\|O_APPEND)` | Append, buffered | `FA_OPEN_ALWAYS`, seek to end before each write | Same result |
| `open(O_WRONLY\|O_CREAT\|O_EXCL)` | Create new; `EEXIST` if present | `FA_CREATE_NEW` | Same |
| `open(O_WRONLY\|O_CREAT\|O_TRUNC)` | Replace: the old content stays until the first `fsync` or `close`, then the new body replaces it in one sector write | `FA_CREATE_ALWAYS`: truncates at open | Power loss before the first sync leaves the old file here and an empty file on FatFs |
| `open(O_WRONLY\|O_CREAT)` on a non-empty file | `ENOTSUP`: no overwrite in place | Opens at offset 0 and overwrites | Open with `O_APPEND` or `O_TRUNC` for portable code |
| `lseek` on a writable file | Only a query of the current position | Seeks | A writer that seeks backwards must stage in memory |
| `write` | Buffers; the blocking form flushes when the buffer is full | Cached by FatFs in one sector window, flushed at `f_sync` or when the sector changes | Flush size is an AU here, a sector there |
| `fsync` | Durability point: data, FAT, entry | `f_sync` (data sector, entry) | Same contract |
| `close` | Syncs, then releases unused reservation | `f_close` syncs | Same |
| `rename(a, b)` with `b` present | Atomic replace in one directory | `f_rename` fails with `EEXIST` | Portable code: `rename`; on `EEXIST` call `unlink(b)` then `rename` again. The second path is not atomic |
| `rename` across directories | `EXDEV` | Works | Keep the temporary file in the destination directory |
| `ftruncate` | Shrink: entry first, then the FAT. Grow: zero fill | `f_lseek` / `f_truncate`; grow writes zeros | Same |
| `posix_fallocate(fd, off, len)` | Reserves clusters and extends the size with zeros | None; `esp_vfs_fat_create_contiguous_file` calls `f_expand` | The extension writes zeros and wears the card; prefer `ss_fallocate_keep` |
| `ss_fallocate_keep(fd, len)` | Reserves clusters, size unchanged, lost at close or power cut | `f_expand` with allocation (persistent) | Reservation is not persistent here |
| `fstat`, `stat` | Size and directory flag only | Adds times and mode | Times are not exposed |
| `unlink`, `mkdir` | Supported; `unlink` also removes an empty directory | Same | Same |
| Reading a file open for writing | Syncs first, then reads durable bytes | Reads what `f_read` sees | Costs a sync |
| Open descriptors | 8 (`SS_FS_MAX_FILES`), one buffer each | The `max_files` mount option | Set by the pool the application supplies |

A portable storage layer therefore writes a configuration file as: open a
temporary file in the same directory with `O_CREAT|O_EXCL`, write, `fsync`,
`close`, then `rename` over the target, retrying with `unlink` when `rename`
returns `EEXIST`. On SunstoneOS the first `rename` is atomic; on the ESP32 parts
the fallback is not.

## Limits

- Sectors are 512 bytes; the FAT is FAT32.
- Files are append-only; directories are not renamed; a rename stays in one
  directory.
- The persistence model assumes a sector write is atomic and that a write call's
  sectors reach the card in ascending order. The host checks also persist a random
  subset of every multi-sector call. A card can tear a sector write on power
  loss; no host check can show that it does not, and the board interruption test
  measures it on the real card.
- File sizes are below 4 GiB (the directory entry's 32-bit size).
- Only ASCII names are created. Existing non-ASCII long names match only
  by their ASCII characters.

## Host evidence

`checks/fat32-rw.py` builds a `mkfs.fat` FAT32 volume with 512-byte clusters at
sector 32768 of a card image, and runs a scenario through the service while the
model logs every sector write. The scenario creates a directory and a
long-named, preallocated file with two syncs and a close, replaces a
configuration file, renames a file over another, unlinks and truncates.

For every cut point, a power cut leaves the first k logged sectors. The check
then runs mount-time recovery, runs `fsck.fat -n` (exit status 0 required),
reads the volume with an independent Python FAT32 reader and compares each file
with the constraints the scenario recorded, and mounts again to confirm that
nothing is written. Results of the committed run: 1333 cuts (one per sector
write of the scenario), 2468 random subsets of multi-sector calls, 138 cuts in
the recovery itself, all clean with synced bytes intact.

The sweep found one defect during development. The recovery's last write
cleared the clean bit in FAT copy 1 before copy 2, so a cut between them left
FAT 1 clean and FAT 2 different, and the volume mounted without recovery. The
flag now clears in copy 2 first and sets in copy 1 first. The sweep fails on the
earlier ordering. Skipping the freeing of unclaimed clusters (a mutation of the
recovery) makes the sweep fail with `fsck.fat` reporting lost clusters.

Other checks in the same file: write-size accounting (20000 appends of 100 bytes
become 30 writes of one AU each, none unaligned, each preceded by a yield),
the write guard at every region boundary, and the POSIX-like calls through the
newlib system calls with a descriptor limit, would-block writes that touch
nothing, truncate-replace visibility, rename over an existing file, `ftruncate`
and `posix_fallocate`.

## Remaining work

1. SD core: CMD25 with descriptor DMA, CMD12 stop handling, ACMD13 read of the
   SD Status (and `UHS_AU_SIZE` in UHS modes), and an adapter that fills
   `struct fwr_device`. The core refuses every command outside its allow-list
   today, including CMD25.
2. A read-over-write arbiter at the adapter.
3. Runner admission of bounded writes: allowed sector ranges and the hash of
   the metadata backup, checked before the first write.
4. An OS application at the top operating points that measures sustained rate,
   flush latency and the interruption test, with the measured CPU clock in every
   evidence record, and the first bounded write of the large-region test file.
5. Board measurement of the sector-tearing assumption above.
