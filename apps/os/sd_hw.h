#ifndef SUNSTONE_OS_SD_HW_H
#define SUNSTONE_OS_SD_HW_H
/* The SD0 hardware operations of a write-capable OS image (OS_SD_WRITE). apps/os/large_sd.c builds the platform once
 * and owns its state; apps/os/sd_owner.c runs the single owner task that calls these. Every function must run in the
 * task that called os_sd_open_owner(), which the native adapter requires. */
#include <stddef.h>
#include <stdint.h>
#include "app_large.h"

struct a733_storage_bounds;
#define OS_SD_WRITE_PIECE_SECTORS 256u
#define OS_SD_WRITE_BYTES (OS_SD_WRITE_PIECE_SECTORS * 512u)

struct os_sd_hw {
    const struct os_large_device *device;       /* the read path of the loader: sync metadata reads and pipelined requests */
    uint64_t sectors;                           /* card capacity */
    unsigned sector_addressed;
    /* ACMD13: the allocation unit in sectors (0 when the card defines none). */
    int (*allocation_unit)(uint32_t *sectors);
    /* Parse the runner-delivered bounds, re-read the listed backup sectors and compare the hash, then bind the writer. */
    int (*enable_writes)(const uint8_t *blob, size_t bytes);
    /* One CMD25 write of 1 to OS_SD_WRITE_PIECE_SECTORS sectors from src, to completion. Returns 1. */
    int (*write_piece)(uint64_t sector, uint32_t count, const uint8_t *src);
    int (*live)(void);
};
/* Open the card in the calling task (it becomes the owner) and return the hardware operations; NULL on failure. */
const struct os_sd_hw *os_sd_open_owner(void);
void os_sd_close_owner(void);
/* The read-only open and close under their write-image names (large_sd.c). */
const struct os_large_device *os_sd_open_owner_device(void);
void os_sd_close_owner_device(void);
/* sd_owner.c: the writer's entry points; both run in a task other than the owner. */
struct fwr_device;
int os_sd_writes_enable(const uint8_t *blob, size_t bytes);
int os_sd_writer_device(struct fwr_device *);
#endif
