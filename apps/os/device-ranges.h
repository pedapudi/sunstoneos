#ifndef SUNSTONE_OS_DEVICE_RANGES_H
#define SUNSTONE_OS_DEVICE_RANGES_H
/* The DRAM ranges that a device reads or writes, published by the OS when it allocates them from the ledger. The CPU return
 * (apps/os/cpu-return/terminal.patch) validates every published range at EL3 (bounds, 64-byte lines, a write-back mapping for
 * each block) before the board goes back to the ROM. The dirty lines themselves reach DRAM by the set/way sweep of each core's
 * caches and of the shared L3, whose cost depends on the cache sizes and not on the size of any range. The ranges are chosen at
 * run time: nothing about them is known at link time, and the EL3 tables map all DRAM that the boot's decode check passed.
 *
 * The record is a list of pages. A page is 4 KiB of DRAM owned by the kernel in the ledger: the address of the next page, the
 * number of ranges, then {base, bytes} pairs. os_device_ranges_head, a word of image RAM, holds the first page (zero when
 * nothing is published). A range is published before any device uses it. The count of a page is stored with release order
 * after its pair, so the reader at EL3 (which runs after every device is quiet) sees complete pairs only.
 *
 * Each range starts and ends on a 64-byte line and lies in the DRAM above the OS image. The number of ranges is bounded by
 * memory: a page holds 255 and the list grows by taking pages from the ledger. */
#include <stdint.h>

#define OS_DEVICE_RANGES_PER_PAGE 255u
struct os_device_range { uint64_t base, bytes; };
struct os_device_range_page {
    uint64_t next, count;
    struct os_device_range range[OS_DEVICE_RANGES_PER_PAGE];
};
extern uint64_t os_device_ranges_head;
/* Publish [base, base + bytes); 1 on success, 0 when the range is malformed or the ledger cannot give a page. */
int os_device_range_publish(uint64_t base, uint64_t bytes);
#endif
