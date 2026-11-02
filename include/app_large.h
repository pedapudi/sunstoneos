#ifndef SUNSTONE_APP_LARGE_H
#define SUNSTONE_APP_LARGE_H
#include "app_region.h"
#include "sunstone_large.h"
/* One admitted large region of an application. */
struct large_slot {
    uint64_t base, bytes;
    uint32_t index, flags, state, error;
    uint64_t loaded, total, started_us, finished_us;
    uint64_t copy_us, digest_us;      /* time the loader spent copying into the region and hashing; the rest of the load is the card */
};
/* Take every region of the application's records from the ledger, all or nothing. out has out_capacity
 * (at least count) entries and is indexed by the record's index. Returns an app_region_status; on failure
 * nothing is held. */
int large_admit(struct app_region_ledger *, uint16_t owner, const struct ss_large_region *records, unsigned count,
                struct large_slot *out, unsigned out_capacity);

/* The block device the loader reads, supplied by the board; the default is none (the regions then fail
 * as unloadable and the application learns it). */
#include "fat32_ro.h"
struct os_large_device {
    struct fat32_device sync;          /* absolute sectors; used for the volume's metadata between requests */
    struct fat32_async async;          /* one request in flight, into the buffers below */
    uint64_t partition_first, partition_sectors;   /* zero sectors: use the first FAT32 partition of the MBR */
    uint8_t *buffers;                  /* three 128 KiB buffers the device may fill: two for the stream, one for metadata */
};
#define OS_LARGE_BUFFER_BYTES (256u * 512u)
const struct os_large_device *os_large_device(void);
/* Called by the loader task after its last region: release the device (the SD binding restores clock, pins and rail). */
void os_large_device_close(void);
/* The loader's access to the admitted regions (apps/os/large.c). */
unsigned os_large_count(void);
struct large_slot *os_large_slot_mutable(unsigned index);
const struct ss_large_region *os_large_record(unsigned index);
void os_large_finish(unsigned index, unsigned state, uint32_t error);   /* final state, wakes the waiters */
#endif
