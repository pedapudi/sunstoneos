/* Read-only FatFs media interface. Source anchors: docs/filesystem.md. */
#include "fatfs_port.h"
#include "diskio.h"
#include "a733_range.h"
#include <string.h>

/* FatFs drive 0. Bound once; the owner task is the only caller. */
static struct fatfs_port *bound;

static struct fatfs_port *drive(BYTE pdrv)
{ return pdrv == 0 && bound && bound->state == FATFS_PORT_READY ? bound : 0; }

int fatfs_port_bind(struct fatfs_port *p, const struct fatfs_port_device *d)
{
    if (bound || !p || !d || !d->read || !d->sectors || p->state != FATFS_PORT_UNBOUND)
        return 0;
    p->device = *d; p->state = FATFS_PORT_READY; bound = p;
    return 1;
}

DSTATUS disk_status(BYTE pdrv) { return drive(pdrv) ? STA_PROTECT : STA_NOINIT; }
DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

/* Requests outside the device fail without a device call. A failed read that
 * may have started keeps the bounce buffer borrowed and ends the port. */
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    struct fatfs_port *p = drive(pdrv);
    if (!p) return RES_NOTRDY;
    uint64_t next = sector, left = count;
    if (!buff || !left || next >= p->device.sectors || left > p->device.sectors - next)
        return RES_PARERR;
    while (left) {
        uint32_t n = left < FATFS_PORT_MAX_BLOCKS ? (uint32_t)left : FATFS_PORT_MAX_BLOCKS;
        ++p->reads;
        int r = p->device.read(p->device.context, next, n, p->bounce, p->deadline_us);
        if (r == 0) { ++p->refusals; return RES_NOTRDY; }
        if (r != 1) { p->state = FATFS_PORT_RETAINED; return RES_ERROR; }
        memcpy(buff, p->bounce, n * FATFS_PORT_SECTOR_BYTES);
        buff += n * FATFS_PORT_SECTOR_BYTES; next += n; left -= n;
    }
    return RES_OK;
}

FRESULT fatfs_port_mount(struct fatfs_port *p, uint64_t deadline_us)
{
    if (!p || p != bound) return FR_INVALID_PARAMETER;
    if (p->state != FATFS_PORT_READY) return FR_NOT_READY;
    p->deadline_us = deadline_us;
    return f_mount(&p->volume, "", 1);
}

FRESULT fatfs_port_load(struct fatfs_port *p, const char *path, void *buffer,
                        size_t capacity, size_t *bytes, uint64_t deadline_us)
{
    if (!p || p != bound || !path || !bytes ||
        a733_extents_overlap_prechecked((uintptr_t)bytes, sizeof(*bytes), (uintptr_t)p, sizeof(*p)) ||
        (capacity && (!a733_span_excl(buffer, capacity) ||
                      a733_extents_overlap_prechecked((uintptr_t)buffer, capacity, (uintptr_t)p, sizeof(*p)) ||
                      a733_extents_overlap_prechecked((uintptr_t)buffer, capacity,
                                                      (uintptr_t)bytes, sizeof(*bytes)))))
        return FR_INVALID_PARAMETER;
    if (p->state != FATFS_PORT_READY) return FR_NOT_READY;
    p->deadline_us = deadline_us;
    FRESULT r = f_open(&p->file, path, FA_READ);
    if (r != FR_OK) return r;
    FSIZE_t size = f_size(&p->file);
    *bytes = (size_t)size;
    if (size > capacity) r = FR_INVALID_PARAMETER;
    else if (size) {
        UINT got = 0;
        r = f_read(&p->file, buffer, (UINT)size, &got);
        if (r == FR_OK && got != size) r = FR_INT_ERR;
    }
    FRESULT closed = f_close(&p->file);
    return r != FR_OK ? r : closed;
}

/* The core borrows the port's bounce buffer through DONE or FAULT. Only a
 * refusal before the read starts leaves it unborrowed. */
static int mmc_read(void *context, uint64_t first, uint32_t blocks, uint8_t *buffer,
                    uint64_t deadline_us)
{
    struct fatfs_port_mmc *m = context;
    struct sunxi_mmc *core = m->core;
    size_t bytes = (size_t)blocks * SUNXI_MMC_BLOCK_BYTES;
    if (!sunxi_mmc_read(core, first, blocks, buffer, bytes, deadline_us)) return 0;
    while (core->state == SUNXI_MMC_READING) {
        sunxi_mmc_poll(core);
        if (core->state == SUNXI_MMC_READING && m->idle) m->idle(m->idle_context);
    }
    if (!sunxi_mmc_take(core, &m->result) || core->state != SUNXI_MMC_READY ||
        m->result.error != SUNXI_MMC_OK || m->result.received != bytes) return -1;
    return 1;
}

int fatfs_port_mmc_device(struct fatfs_port_mmc *m, struct fatfs_port_device *d)
{
    if (!m || !d || !m->core || m->core->state != SUNXI_MMC_READY || !m->core->card.sectors)
        return 0;
    *d = (struct fatfs_port_device){m, mmc_read, m->core->card.sectors};
    return 1;
}
