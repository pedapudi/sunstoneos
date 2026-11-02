#ifndef SUNSTONE_STORAGE_H
#define SUNSTONE_STORAGE_H
/* Card writes for an application, in an image built with SUNSTONE_A733_OS_SD_WRITE (docs/designs/storage-write.md). The
 * application links the FAT32 service (fat32_rw.h) and the POSIX-like calls (sunstone_fs.h) into itself; these calls give it
 * the block device. Callable at EL0 from the application's task; the card is owned by the OS's SD owner task. */
#include <stddef.h>
#include <stdint.h>

struct fwr_device;
/* Verify the runner-delivered bounds (the application's first input, its blob and length) against the card and open the
 * writes: 1 on success. Nothing can be written before this succeeds, and only inside the bounds. */
int ss_storage_enable(const void *bounds, size_t bytes);
/* Open the card for reading only: the owner opens it and reads the allocation unit, and no bounds are verified or bound, so the controller
 * refuses every write command. For the read-back modes, which must run on a card whose backup sectors have changed since the blob was made.
 * 1 on success; ss_storage_region and ss_storage_device follow as for ss_storage_enable. */
int ss_storage_open_read(void);
/* Register the flush buffer (64-byte aligned, in a WORK region, a multiple of 512 bytes); the FAT32 service then writes from
 * it with no copy. Once, before ss_storage_device. */
int ss_storage_region(void *base, size_t bytes);
/* Fill the block device (read, write, sector count, allocation unit, largest single write) for fwr_mount. */
int ss_storage_device(struct fwr_device *);
/* Restore the card's clock, pins and rail leases, as a returning image must before it finishes. Call after the volume is
 * unmounted; every later request fails. */
int ss_storage_close(void);
/* CPU time of the SD owner task: wall time since the first call, time spent asleep (waiting for the interrupt, a timer or
 * a request), and the number of wakes. Busy time is the difference; preemption by other tasks counts as busy. */
/* irq_cores, owner_cores and caller_cores are bit masks of the cores that took the SD0 interrupt, ran the owner task, and
 * posted requests to it. */
/* irq_entries counts, per core, the entries of the SD0 interrupt handler (INTID 193); irq_stray those that found the owner not ready
 * (the handler then masks the source); irq_recheck counts the interrupt waits that ended on the recheck timer, not the interrupt. */
struct ss_storage_owner_stats { uint64_t elapsed_us, asleep_us, wakes; uint32_t irq_cores, owner_cores, caller_cores, irq_entries[8], irq_stray, irq_recheck; };
int ss_storage_owner_stats(struct ss_storage_owner_stats *);
/* Where the last open or enable stopped: [0] stage, [1] detail, [2] first failed bounds read (sector + 1), [3] bounds reads made
 * (apps/os/large_sd.c lists the stages). All zero when nothing failed. */
int ss_storage_diagnostic(uint64_t out[4]);
#endif
