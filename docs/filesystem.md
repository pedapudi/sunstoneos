# Read-only FAT and exFAT files on the microSD card

The [FatFs port](../drivers/storage/fatfs_port.c) reads files from a FAT12,
FAT16, FAT32 or exFAT volume on the microSD card. A resident image can then load
mimus models and configuration by path from an ordinary card partition, in place
of fixed sector ranges. The port reads through the polled
[SD/eMMC core](mmc.md) and never writes to the card. Card writes remain limited
to the backup-first [storage update service](storage-update.md).

Status: implemented and host-modeled. Host checks cover FAT16, FAT32 and exFAT
card images, read through a RAM device and through the actual polled core with a
modeled SD controller. The native AArch64 objects compile. No image links the
port yet, and no physical card has been read through it.

## Source and license

The filesystem code is FatFs R0.16 by ChaN, as distributed in ESP-IDF v6.1 at
commit `fff9895c82d744c7237be8847347bdd1b07c6643`
([components/fatfs/src](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643/components/fatfs/src)).
The [source manifest](../lib/fatfs/source.json) pins each file by SHA-256. Only
`ff.c` and `ffunicode.c` are compiled; `ff.h` and `diskio.h` are headers. The
FatFs license is a one-clause BSD-style license: redistributions of source must
keep the copyright notice, condition and disclaimer at the top of `ff.c` and
`ff.h`. The files are compiled without modification from the external source
cache.

ESP-IDF's copy is used in place of ChaN's release archive because it adds input
validation that a removable card needs. Its `sbom.yml` records fixes for
CVE-2026-6682 (32-bit overflow while mounting FAT32), CVE-2026-6683 (exFAT
division by zero on an empty cluster heap), CVE-2026-6685 (unsigned wrap in the
cached-sector refill check) and CVE-2026-6687 (exFAT label length). ESP-IDF also
adds a heap-buffer option, `FF_USE_DYN_BUFFER`, which this configuration sets
to zero, and declares FatFs's `UINT` as `size_t`.

## Configuration

[ffconf.h](../lib/fatfs/ffconf.h) selects these options:

| Option | Setting | Effect |
| --- | --- | --- |
| Write support | Read-only | `f_write`, `f_mkfs`, `f_unlink` and every other writer are absent from the object |
| Formats | FAT12/16/32 and exFAT | exFAT covers SDXC cards formatted at the factory |
| Names | Long names, UTF-8 paths, code page 437 short names | Paths such as `models/mimus overlay model.bin` work |
| Volumes | One, on drive 0 | An unpartitioned card, or the first of the four MBR partitions that holds a FAT or exFAT volume |
| Addressing | 32-bit sector numbers | Volumes up to 2 TiB; GPT is not recognized |
| Buffers | One 512-byte window per volume and per open file | No allocation; long-name buffers are static |
| Concurrency | None | One task owns the port and every FatFs object |

The SunstoneOS [card layout](boot-slots.md) keeps an MBR. Boot0, the slot headers
and the boot package occupy the first 20 MiB, outside any partition. FatFs reads
sector 0 and each partition's first sector to find the volume. A card with only
a GPT protective entry has no recognized volume, and mounting returns
`FR_NO_FILESYSTEM`.

`ff.h` includes `ffconf.h` from its own directory first, and ESP-IDF ships its
own `ffconf.h` there. Builds therefore force this configuration with
`-include lib/fatfs/ffconf.h` on the FatFs sources, and `fatfs_port.h` includes
it before `ff.h`. The configuration also renames FatFs's `disk_*` media
functions to `fatfs_port_disk_*`.

## The port reads whole sectors through a private buffer

The [public interface](../include/fatfs_port.h) has three parts:

- A **device** reads one to eight 512-byte sectors before an absolute deadline.
  It returns 1 on success and 0 when it refused the request before starting
  anything. Any other result means the read may have started and failed.
- The **port** binds FatFs drive 0 to one device for the owner's lifetime,
  mounts the volume and loads whole files.
- The **SD binding** turns an identified, ready `sunxi_mmc` core into a device.

FatFs may ask for many sectors in one call, for example a whole cluster. The
port first checks the complete range against the device capacity and refuses it
without a device call if any sector lies outside. It then issues requests of at
most eight sectors, each into the port's own 4 KiB buffer. Only a successful
request is copied into the FatFs buffer. Bytes from a failed transfer therefore
never reach FatFs or the caller.

A refusal fails only the current FatFs operation; a later one may succeed. A
failure that may have started is terminal. The port keeps its buffer borrowed,
reports the drive as not initialized and refuses every later mount and load
without calling the device. This matches the core's own rule: a faulted read
keeps its buffer and every platform lease until an external owner recovers the
board.

`fatfs_port_mount` and `fatfs_port_load` take an absolute deadline in
monotonic microseconds, which the port passes to every device read in that
operation. `fatfs_port_load` opens a regular file, reports its size, refuses a
file larger than the caller's buffer before reading data, reads the file and
closes it. A caller that uses other FatFs functions sets `deadline_us` in the
port first.

## The SD binding drives the polled core synchronously

`fatfs_port_mmc_device` requires a core in the READY state, after
identification, and takes the device capacity from the card's CSD. Each read
starts `sunxi_mmc_read` on the port's buffer and polls the core until it leaves
the READING state, calling an optional idle function between polls. A resident
task can use that function to yield or to record watchdog progress. The read
succeeds only when the core returns to READY with no error and exactly the
requested byte count.

The core checks the deadline, the platform's retained ownership and card
presence on every poll. A CRC error, a removed card, lost ownership or an
expired deadline faults the core; the binding then reports a failure and the
port stops. If the core refuses to start, because it is not READY or the
deadline has already passed, the binding reports a refusal and nothing is
borrowed. The data path is programmed I/O, so no DMA mapping or cache
maintenance is involved.

## Memory and bounds

The native objects for FatFs and the port take about 15.3 KB of code and
1,138 bytes of static data. `struct fatfs_port` is 5,312 bytes: the 576-byte
volume, a 584-byte file object, the 4 KiB buffer and counters. The largest
native stack frames are 144 bytes, in `f_open` and its path parser.

Every FatFs operation reads a bounded number of sectors. File reads stop at the
recorded file size. A cluster chain that leaves the volume or reaches a reserved
value returns `FR_INT_ERR`, as does an exFAT entry set whose checksum does not
match. FatFs stops scanning a FAT directory after 2 MiB and an exFAT directory
after 256 MiB. The caller's deadline bounds the time of each operation.

## Checks

```bash
PYTHONDONTWRITEBYTECODE=1 python3 checks/fatfs-port.py \
  --fatfs /path/to/pinned/fatfs/src \
  --evidence /absolute/new/evidence-directory
```

The [check script](../checks/fatfs-port.py) verifies the pinned FatFs files,
then builds three card images. `mkfs.fat` and `mkfs.exfat` format each volume,
and the script adds files with its own FAT and exFAT writers, which share no
code with FatFs. The FAT32 and exFAT images have an MBR partition at sector
40960 behind random boot0 and slot-header contents; the FAT16 image has no
partition table. Each image holds a short configuration file, an empty file, a
fragmented 300 KiB model, a fragmented long-name model, a contiguous model and
a file in the last clusters of the volume. The [host model](../checks/fatfs-port.c)
runs with AddressSanitizer and UndefinedBehaviorSanitizer:

- Every file in each image loads with its exact size and contents through a
  RAM device. The exFAT image exercises eight-sector requests.
- The FAT32 and exFAT images also load through the polled core, which
  identifies a modeled SDHC card and reads the image with CMD17 and CMD18.
- Invalid bindings, buffers that overlap the port, a buffer one byte too small,
  a missing file and a directory path are refused; the too-small buffer receives
  no data.
- A refused device request fails one load, and the next load succeeds.
- A failed device request stops the port. The failed bytes appear in neither
  the caller's buffer nor the FatFs window, and later calls reach no device.
- A CRC error, card removal and an expired deadline fault the actual core. The
  core keeps the port's buffer borrowed and no register access follows.
- A device shorter than the volume receives no request beyond its end.
- Corrupt FAT32 and exFAT chains and a corrupt exFAT checksum return
  `FR_INT_ERR`. A blank partition and a GPT-only card return
  `FR_NO_FILESYSTEM`.

FAT12 is enabled in FatFs but has no image in these checks.

The script also compiles the port, `ff.c` and `ffunicode.c` for AArch64 with the
image compile options and links them into one relocatable object. Its only
external references are `memcpy`, `memset`, `memcmp`, `strchr` and the three
core functions `sunxi_mmc_read`, `sunxi_mmc_poll` and `sunxi_mmc_take`. No
FatFs write function is defined.

[fatfs.cmake](../lib/fatfs/fatfs.cmake) adds the volume to an image target
with `sunstone_enable_fatfs(<target> <fatfs-dir>)`. It refuses to configure when
any pinned file differs from the manifest. A scratch AArch64 library configured
with it builds, and a modified `ff.c` stops configuration.

The run that passed twenty host cases is under
`~/.cache/sunstoneos/build/audit-storage-filesystem/fatfs-check-final/`. The
script regenerates the card images and does not keep them.

## Remaining work

- Link the port into the resident OS image or the supervised read service, and
  choose the owning task, its deadlines and its idle function.
- Read a physical card. Every physical prerequisite of the [SD core](mmc.md) and
  its [platform binding](mmc-platform.md) still applies, and no SD command has
  run on either board.
- GPT cards, volumes above 2 TiB, more than one volume, concurrent access from
  several tasks and any write remain unsupported.
- An unnoticed card replacement between reads is not detected, as for every
  other SD client.
