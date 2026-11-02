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
struct ss_storage_owner_stats { uint64_t elapsed_us, asleep_us, wakes; };
int ss_storage_owner_stats(struct ss_storage_owner_stats *);
#endif
