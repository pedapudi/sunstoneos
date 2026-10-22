#ifndef SUNSTONE_FATFS_PORT_H
#define SUNSTONE_FATFS_PORT_H
#include <stddef.h>
#include <stdint.h>
/* Select this configuration before ff.h, ahead of any ffconf.h beside it. */
#include "ffconf.h"
#include "ff.h"
#include "sunxi_mmc.h"

/* Read-only FatFs volume over whole 512-byte card sectors. One task owns the
 * port, its device and every FatFs object; there is no locking. The port reads
 * into its private bounce buffer and copies to FatFs only after success. */
#define FATFS_PORT_SECTOR_BYTES 512u
#define FATFS_PORT_MAX_BLOCKS SUNXI_MMC_MAX_BLOCKS

enum fatfs_port_state { FATFS_PORT_UNBOUND, FATFS_PORT_READY, FATFS_PORT_RETAINED };

struct fatfs_port_device {
    void *context;
    /* Read 1..FATFS_PORT_MAX_BLOCKS sectors into buffer before the absolute
     * deadline. Return 1 when complete; 0 when refused before anything started,
     * leaving buffer unborrowed; any other value when the read may have started
     * and failed. The buffer then stays borrowed and the port never reuses it. */
    int (*read)(void *, uint64_t first_sector, uint32_t blocks, uint8_t *buffer,
                uint64_t deadline_us);
    uint64_t sectors; /* Device capacity; every request lies inside it. */
};

struct fatfs_port {
    struct fatfs_port_device device;
    enum fatfs_port_state state;
    uint64_t deadline_us; /* Absolute bound passed to each device read. */
    uint64_t reads, refusals; /* Diagnostic device-request counts. */
    FATFS volume;
    FIL file; /* Used by fatfs_port_load. */
    uint8_t bounce[FATFS_PORT_MAX_BLOCKS * FATFS_PORT_SECTOR_BYTES];
};

/* Bind FatFs drive 0 to this zeroed port once for the owner's lifetime. */
int fatfs_port_bind(struct fatfs_port *, const struct fatfs_port_device *);
/* Mount the unpartitioned volume or the first FAT/exFAT MBR partition. */
FRESULT fatfs_port_mount(struct fatfs_port *, uint64_t deadline_us);
/* Read one whole regular file. *bytes receives the file size whenever the file
 * opens. A file larger than capacity returns FR_INVALID_PARAMETER without
 * reading data. On any error the buffer contents are unspecified. */
FRESULT fatfs_port_load(struct fatfs_port *, const char *path, void *buffer,
                        size_t capacity, size_t *bytes, uint64_t deadline_us);

/* Synchronous device over the polled SD/eMMC core, owned by the same task.
 * idle, when set, runs between polls of an active read, for example to yield. */
struct fatfs_port_mmc {
    struct sunxi_mmc *core;
    void (*idle)(void *);
    void *idle_context;
    struct sunxi_mmc_result result; /* Last collected core result. */
};
/* Requires an identified READY core; fills device for fatfs_port_bind. */
int fatfs_port_mmc_device(struct fatfs_port_mmc *, struct fatfs_port_device *);
#endif
