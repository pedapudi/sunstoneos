# General storage write service for the microSD card

Applications write files on a FAT32 volume of the microSD card through a RAM
write-back buffer. The service flushes in large writes that end on the card's
allocation-unit boundaries, changes the FAT and the directory only at sync
points. It orders every sync so that a power cut at any sector write leaves a
volume that recovers to a state `fsck.fat -n` reports clean, with every
synced byte intact. POSIX-like calls over the service let storage code written
for FatFs and newlib stdio (for example mimus on ESP32 parts) run unchanged.

**Status.** The write service, the POSIX-like layer, the newlib system calls and the card adapter are implemented. The card adapter is the SD owner task of the OS image (`apps/os/sd_owner.c`, `apps/os/large_sd.c`) over CMD25 with descriptor DMA ([multi-block write](mmc-multiblock-write.md)), the SD Status read (ACMD13) and the runner's admission of bounded writes. The service is **qualified on the board** for bounded writes on the 62.5 GB card at 3.3 V and 50 MHz, including one interruption and recovery; see [Qualified scope](#qualified-scope). Writes at 1.8 V or 150 MHz are not qualified ([adoption design](sd-uhs-adoption.md)).

## Why the design avoids small and repeated writes

A microSD card has its own controller that maps logical blocks to flash and
levels wear, so the operating system does none of that. What wears the card and
stalls it is write amplification: a write smaller than, or not aligned to, the
card's allocation unit (AU) makes the controller read, modify and rewrite a
whole erase unit. The SD Physical Layer Simplified Specification, version 9.10,
defines the AU in the SD Status register (section 4.10.2, Table 4-44, bits
431:428 `AU_SIZE`). Section 4.10.2.4, Table 4-47 lists 16 KB to 64 MB, and Table
4-48 gives the maximum for each capacity, 4 MB up to 32 GB and 64 MB above. The host
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
   largest divisor of the AU that fits half the buffer. An AU is a multiple of
   its divisors, so a write that ends on the divisor never straddles an AU in
   the middle. A non-final flush writes only whole sectors up to the last
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

The ordering follows Linux `fs/fat` (stable tree `linux-6.12.y`) with three deliberate differences. The dirty flag is FAT[1] bit 27, not the boot sector's state byte, because the boot sector is outside the writable regions and `fsck.fat` reads FAT[1]; it is set lazily, so a session that writes nothing writes nothing. The FAT chain is written before the directory entry, so that an entry never names a chain the FAT does not hold, where Linux writes the entry first; recovery handles a chain longer than its entry by truncation. FSInfo is written at checkpoint and unmount, as Linux writes it with the superblock. Where they agree: the FAT copies are written in order, a new directory cluster is zeroed before it is linked, and a deleted file marks its short and long-name entries deleted. `entry_delete` deletes long entries in other sectors first, so a cut leaves a valid file under its short name. The record `storage-write-host-evidence` holds the function-level comparison.

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

The writer runs as a low-priority task. The application never waits for the card on a real-time path: `ss_write_nowait`
copies into the buffer and returns, and the writer task calls `ss_fs_service` to flush.

**One owner, loader first.** The native adapter admits register access only from the task that opened the card, so the
write-capable OS image runs one SD owner task (`apps/os/sd_owner.c`). It serves two single-slot mailboxes: the loader's
reads, then the writer's reads and write pieces. A write piece is at most 256 sectors (128 KiB), and the owner looks at the
loader's mailbox between pieces, so a model-loading read waits for at most the piece in flight. Callers sleep on a task
notification until the owner finishes; the owner sleeps on one until a request arrives.

**The owner runs at EL1.** The owner reads the CCU, configures the pins and drives SMHC0, none of which an EL0 task may touch. A new
task takes the privilege level its creator passes to the scheduler's hook, and the application's EL0 task creates the owner on its first
`ss_storage_enable` call (the loader that creates it in a read-only image never runs for an application without large regions). The
scheduler therefore asks `board_application_task_privileged`, which reaches `os_sd_owner_task_privileged` (`apps/os/sd_owner.c`) through
`os_application_task_privileged`. Any image that lets an application create the owner must keep that path: the image link check
(`checks/sunstone-image-link.py`) disassembles the hook in every image that links `ss_storage_enable` and refuses one that does not reach
the owner's predicate.

**One device's clock fault must not fail the shared provider (open defect).** A faulted SD0 binding takes the thermal service down with it. The
call path is `a733_mmc_platform.c fail()` (lines 57 to 64), which calls `a733_clock_retain` on the SD0 token. `a733_clock_leases.c` then latches
`fail(m)`, which sets the registry-wide `failed` flag and the power authority's CLOCK_FAULT bit, so that `enter()` returns FAULT to every later caller
of any token. The thermal service reads that flag in `live()` and in every register access (`apps/os/thermal.c`), so it ends the session within a second
or two, before the owner can report the SD fault. The platform's own retention of the register lock has the same reach (the thermal service enters the
same lock). A single device's token fault should retain that token and its pins and rails and nothing else; whether that holds for the registry
is a change to shared OS code that the coordinator decides. Until then the owner leaves its stage as the first-failure note, so the record names the
SD0 binding when it came first.

**Why an open or an enable stopped.** `ss_storage_diagnostic` returns the stage and a detail (`apps/os/large_sd.c` lists the stages); the
application copies it into its result, so a refused enable can be read without another instrumented image.

**Completion by interrupt and one-shot timer.** No transfer polls the controller in a loop. The core arms the SMHC0 interrupt
mask for each DMA command as the board kernel does (`sunxi_mmc_exe_cmd`, `sunxi-mmc.c:2754`: the mask is written before
the argument and command). The mask is the error bits plus the automatic-stop-done bit for a CMD25 or CMD18 that carries a stop, else the
data-over bit (`sunxi_mmc_parse_cmd`, `sunxi-mmc.c:2638`). The handler for GIC SPI 161 (INTID 193, level high, board kernel
`sun60iw2p1.dtsi`, `sdc0`) follows `sunxi_mmc_irq` (`sunxi-mmc.c:1271`): it reads the masked status, accumulates the bits,
writes the same bits back to the raw status to acknowledge them, and wakes the waiter. The core ORs the accumulated bits into
the raw status it reads (`sunxi_mmc_set_irq`), so a bit the handler acknowledged is never lost, and it disarms the mask when the
command ends. The IDMAC interrupts stay off, as in the board kernel's write path, and IDST remains with the core.

After a poll that did not finish, the core says what to wait for (`sunxi_mmc_wait_hint`): the interrupt, for the command and
data phase; a one-shot timer, for DAT0 busy and for retry spacing; or nothing, for register conditions of a few microseconds
(a reset bit clearing). `storage_wait_run` sleeps accordingly on `sunstone_deadline_wait_until`, the one-shot deadline timer,
bounded by the operation deadline and, for an interrupt wait, by a 5 ms recheck so a lost interrupt cannot hang an operation.
Every wake is a hint: the next poll reads the controller. DAT0 busy has no interrupt on this controller: the board kernel's
v5p3x path does not set `sunxi_mmc_hw_wbusy_wait`, so it never enables the busy-clear interrupt and polls card busy in its bottom
half. The manual lists one (RINTSTS bit 17, `R1B_BUSY_CLR`, and bit 13 `DSE_BC` for the end of busy after the last write block,
enabled by `THLD.BCIG`), but nothing in the vendor driver exercises it on this chip, so using it is left to a board experiment. The
busy timer doubles from 100 microseconds to 2 milliseconds, so a busy of several hundred milliseconds costs a few hundred wakes.
`checks/sunxi-mmc-write-dma.c` runs a 128 KiB write with 4 ms of DMA and 300 ms of busy: 172 polls and 154 sleeps, one of them
on the interrupt. It also covers an interrupt that arrives before the sleep, one that is lost, and stale and spurious wakes. The
owner's busy fraction during a flush is a board measurement (`ss_storage_owner_stats`).

**Allocation unit.** `AU_SIZE` applies to the bus modes the image runs: profile 2 is high speed at 3.3 V, which is not a UHS-I
mode. The unit is read from `UHS_AU_SIZE` instead when the card signals at 1.8 V, the condition for a UHS-I bus speed mode
(SD Physical Layer 9.10, section 4.10.2.9); the image passes `card.signal_1v8`.

**The bounds blob.** The runner's frozen first input is an INPUT region of the application, mapped read-write at EL0 like all
kernel data and application regions in this OS (`docs/memory-protection.md`: only the text and read-only data block is
read-only), so no mapping can keep it unchanged. The image copies it once into memory taken from the OS heap at enable time (sized from the input's length; the only refusal is that the heap has no room), parses the copy. `a733_storage_bounds_allows` re-hashes it on every use and refuses a blob that has changed since parse. This detects a
change; it does not prevent one made between the hash and the use, which would take a second EL0 task on another core racing
a microsecond window. A read-only mapping needs page-table support the OS does not have yet; the wider gap, kernel and driver data reachable from EL0, is a backlog item in `docs/memory-protection.md`.

**One crossing of memory.** The application registers a WORK region as the FAT32 service's flush buffer
(`ss_storage_region`). A write piece that lies inside it goes to the controller from there, with the cache clean the core
already performs before the transfer. Only metadata writes (the FAT window, directory sectors) are copied into the image's
128 KiB bounce buffer.

The service itself holds no interrupt-masking spans and takes no locks; it is owned by one task. Interrupt-masked spans over
5 microseconds are defects (the scheduler stream's target); the handler masks nothing.

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

`checks/fat32-rw.py` runs a scenario (directory creation, a long-named preallocated file with two syncs and a close, a replace, a rename over another file, unlink and truncate) on a `mkfs.fat` volume while the model logs every sector write. A power cut after each logged sector (1,339 cuts), at 2,482 random subsets of multi-sector calls and at 133 points inside recovery leaves a volume that mount-time recovery, `fsck.fat -n` and an independent FAT32 reader accept, with every synced byte intact. The sweep fails if the recovery clears FAT copy 1 before copy 2 or skips freeing unclaimed clusters. The results, the other checks and the comparison with Linux `fs/fat` are in the record `storage-write-host-evidence` in [the evidence file](../evidence/storage-write.jsonl).

## Card adapter and driver contract

The adapter fills `struct fwr_device` from the SD core:

- **Multi-block write.** CMD25 with the data moved by the controller's descriptor chain, up to the single-write limit the device reports (256 sectors, `OS_SD_WRITE_PIECE_SECTORS`), terminated by the automatic CMD12; the driver then waits for the card to leave the programming state by the DAT0 busy line and CMD13 ([multi-block write](mmc-multiblock-write.md)). SD specification section 4.3.4 asks for multiple-block writes for speed.
- **Allocation unit.** ACMD13 (CMD55, then CMD13 with a 64-byte data block) returns SD Status. `sunxi_mmc_au_sectors` takes `AU_SIZE` (bits 431:428) and, in a UHS-I mode, `UHS_AU_SIZE` (section 4.10.2.9), converts it to sectors with Table 4-47 and passes it as `au_sectors`; `Not Defined` selects the 128-sector default.
- **Priority.** A pending loader read runs before the next write piece (the owner's mailboxes, above). The card core has one owner, so the arbiter is a mailbox check between pieces; no tick-based polling is involved.

## Bounded first write on the 62.5 GB card

The card holds one FAT32 partition at sector 32,768 with 122,105,856 sectors, 64 sectors per cluster and a 4 MiB allocation unit (8,192 sectors). The host run `fat32-rw plan` records every write of creating `/sunstone-test/sunstone-region-test.bin` (536,870,912 bytes) on a volume of this geometry: one FSInfo sector, FAT indexes 0 to 128 in both copies, the root and directory clusters (62,656 to 62,783) and the data (clusters 47 to 16,430, the first on an allocation unit boundary). The service writes 267 metadata sectors and the data in 4,110 calls (4,096 pieces of 256 sectors and 14 metadata writes). The boot sector, its backup, the backup FSInfo and every other FAT sector are never written. The backup is 395 sectors in five ranges (`[0,1] [32768,8] [32832,129] [47744,129] [62656,128]`). The full table is in the record `storage-write-first-write-plan`; the plan is regenerated for a different card.

### Each write session needs a fresh capture, blob and record

The blob's backup hash covers the card's state before the session, and the writer's enable re-reads those sectors and refuses to bind when the hash differs. A session changes the very sectors the backup holds (FSInfo, the FAT starts, the root and directory clusters), so the blob of one session does not verify the card the next session finds. For every session that writes (modes 0, 1 and 5):

1. read the backup ranges again with the read-only backup probe, so the capture is the card as it is now;
2. encode a new blob from the same ranges and the new capture's hash (`tools/storage_bounds.py` `encode_input`);
3. the coordinator checks the blob against the capture and writes a new approval record for the blob and the image, retiring the old one.

The first backup (`backup-395-sectors.bin`, the card before any write) stays as the restore point and is never replaced. The read-back modes (2, 3 and 6) open the card for reading (`ss_storage_open_read`), verify no bounds and bind none, so the controller refuses every write command, and they run on a card whose backup has changed.

The runner admits a bounded-write image only with `--storage-write` and `--approved-by coordinator`; its input carries the allowed sector ranges and the SHA-256 of the metadata backup (`tools/board.py`, `tools/board_images.py`).

### Scratch cards

A card that the coordinator lists as scratch (it holds nothing of value; `scratch-cards.json` in the coordinator's directory) is admitted on its CID and its FAT32 partition. The blob (`SSBW0004`, `tools/storage_bounds.py encode_scratch`) holds one write range, the partition, with no backup range and no digest. The enable checks the card's CID and that every write lies inside the partition; nothing is read back from the card. The coordinator keeps one record per card (`storage-card-<cid>.json`), not one per image.

Mode 9 of `storage-write-test` restores a scratch card by reformatting the partition in place: both FATs and the root cluster are zeroed, FAT entries 0 to 2, the FSInfo sector and the boot sector's label are rewritten, and the geometry stays. `checks/storage-write-guard.py` checks it on the host (`fsck.fat -n` clean, the region file created again afterwards). A card that is not listed as scratch keeps the strict path above: a blob (`SSBW0003`) naming the backup's SHA-256, a restore point per CID (`restore-points.json`, never replaced) and a record per blob.

## Qualified scope

Board-qualified on the 62.5 GB card (Class 10, U3, V30, A1; profile 2), through the OS image's SD owner task:

| Result | Value | Record |
| --- | --- | --- |
| 512 MiB file created, contiguous chain (clusters 47 to 16430), read back | 0 mismatches; chain whole, FAT copies equal, FAT[1] clean | `storage.jsonl` `storage-write-qualification` |
| Append of 144 MiB in a later session | full size, FSInfo, entry and both FATs consistent | `storage.jsonl` `1793632378497932347-1618d4a8` |
| Cut in the middle of a flush, then recovery | recovery wrote 1,536 bytes of metadata; the 40 MiB synced file reads back whole | `storage.jsonl` `1793636210564944400-a065537e` |
| Read rate | 21.7 MB/s (6.0 ms per 128 KiB) | `storage.jsonl` `1793632286777262813-62b0f3bb` |
| Write rate, flushed | 14.7 MB/s; flush latency p50 256 ms, p99 803 ms | `storage.jsonl` `1793632378497932347-1618d4a8` |

Throughput figures taken with the controller interrupt disabled are not valid rates ([record](../evidence/storage.jsonl) `storage-interrupt-disabled-throughput`).

## Limitations and open work

1. Not qualified: more than one interruption point (the host sweep covers every sector write; the board has one cut), other cards, writes outside `/sunstone-test/`, directories other than the existing one, concurrent writers, and writes at 1.8 V or 150 MHz.
2. Board measurement of the sector-tearing assumption under [Limits](#limits).
3. Reads through the file API use a cursor (`fwr_read_cursor`); the application's verify has not been switched to it and run on the board (record `storage-fat32-read-cursor`).
4. The busy-clear interrupt in place of the DAT0 timer, and a comparison of flush latency and owner CPU time, once the write path has run again.
5. Images that link the SD core without `SUNXI_MMC_READ_ONLY` are write-capable to the runner and need a rebuild with that definition, or a coordinator approval record, before they run.
6. Mapping kernel and driver data EL1-only ([memory protection](../memory-protection.md#backlog-kernel-and-driver-data-are-reachable-from-el0)).
