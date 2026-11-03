/* The loader of large application regions (docs/designs/large-app-regions.md): one EL1 task that fills each
 * admitted region from the card, a FAT32 file or a raw extent, through two buffers while it copies and hashes
 * the other, and publishes READY only when the SHA-256 equals the declaration. It writes the region through the
 * kernel's table, so the application's read-only mapping does not matter, and it runs before the application's
 * main task is released (the application waits in ss_large_wait). */
#include "FreeRTOS.h"
#include "task.h"
#include "app_large.h"
#include "a733_storage_digest.h"
#include "a733_smp_placement.h"
#include <string.h>

#define PRIORITY 2
static StaticTask_t task_state;
static StackType_t task_stack[2048];
static struct fat32 volume;
static struct storage_digest digest;
static uint64_t loaded_offset;
static unsigned current;
/* Microseconds from the architectural counter (24 MHz on the board), which the loader's EL1 can read. The FreeRTOS tick
 * has a 1 ms step and produced loads that agreed to the microsecond. */
static uint64_t counter_ticks(void) { uint64_t v; __asm__ volatile("isb\n mrs %0, cntvct_el0" : "=r"(v) :: "memory"); return v; }
static uint64_t counter_hz(void) { uint64_t v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
static uint64_t ticks_us(uint64_t ticks) { uint64_t hz = counter_hz(); return hz ? ticks / hz * 1000000u + ticks % hz * 1000000u / hz : 0; }
static uint64_t now_us(void) { return ticks_us(counter_ticks()); }
static uint64_t copy_ticks, digest_ticks;
static int hashing;                 /* the current region declares a digest */

static int sink_consume(void *unused, const uint8_t *data, uint64_t bytes, uint64_t offset)
{
    (void)unused;
    struct large_slot *s = os_large_slot_mutable(current);
    if (offset != loaded_offset || offset + bytes > s->bytes) return 0;
    uint64_t *dst = (uint64_t *)(uintptr_t)(s->base + offset);
    const uint64_t *src = (const uint64_t *)(const void *)data;
    uint64_t t0 = counter_ticks();
    for (uint64_t i = 0; i < bytes / 8; ++i) dst[i] = src[i];
    for (uint64_t i = bytes & ~7ull; i < bytes; ++i) ((uint8_t *)dst)[i] = data[i];
    uint64_t t1 = counter_ticks();
    if (hashing) storage_digest_add_extensions(&digest, data, bytes);
    uint64_t t2 = counter_ticks();
    copy_ticks += t1 - t0; digest_ticks += t2 - t1;
    loaded_offset += bytes;
    __atomic_store_n(&s->loaded, loaded_offset, __ATOMIC_RELAXED);
    return 1;
}

/* A raw extent in 128 KiB requests, double buffered as fat32_stream does. */
static int stream_extent(const struct os_large_device *d, uint64_t first, uint64_t bytes, const struct fat32_sink *sink)
{
    uint8_t *buffer[2] = {d->buffers, d->buffers + OS_LARGE_BUFFER_BYTES};
    uint64_t sector = first, remaining = bytes, offset = 0, want, got;
    uint32_t count;
    unsigned k = 0;
    if (!remaining) return FAT32_OK;
    count = remaining / 512 > 256 ? 256 : (uint32_t)((remaining + 511) / 512);
    got = remaining > (uint64_t)count * 512 ? (uint64_t)count * 512 : remaining;
    if (d->async.submit(d->async.context, sector, count, buffer[0]) != 1) return FAT32_IO;
    for (;;) {
        int p;
        while ((p = d->async.poll(d->async.context)) == 0) {}
        if (p < 0) return FAT32_IO;
        uint64_t done = got;
        sector += count; remaining -= done;
        int more = remaining > 0;
        if (more) {
            count = remaining / 512 > 256 ? 256 : (uint32_t)((remaining + 511) / 512);
            want = (uint64_t)count * 512;
            got = remaining > want ? want : remaining;
            if (d->async.submit(d->async.context, sector, count, buffer[k ^ 1]) != 1) return FAT32_IO;
        }
        if (sink->consume(sink->context, buffer[k], done, offset) != 1) return FAT32_IO;
        offset += done;
        if (!more) return FAT32_OK;
        k ^= 1;
    }
}

/* The first FAT32 partition of the MBR, when the device does not name one. */
static int partition(const struct os_large_device *d, uint64_t *first, uint64_t *sectors)
{
    if (d->partition_sectors) { *first = d->partition_first; *sectors = d->partition_sectors; return 1; }
    uint8_t sector[512];
    if (d->sync.read(d->sync.context, 0, 1, sector) != 1 || sector[510] != 0x55 || sector[511] != 0xaa) return 0;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *e = sector + 446 + 16 * i;
        uint32_t start = e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24, count = e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24;
        if ((e[4] == 0x0b || e[4] == 0x0c) && count) { *first = start; *sectors = count; return 1; }
    }
    return 0;
}

static void load(unsigned index, const struct os_large_device *d, int mounted)
{
    struct large_slot *s = os_large_slot_mutable(index);
    const struct ss_large_region *r = os_large_record(index);
    copy_ticks = digest_ticks = 0;
    s->started_us = now_us();
    s->total = r->source_bytes;
    __atomic_store_n(&s->state, SS_LARGE_LOADING, __ATOMIC_RELEASE);
    current = index; loaded_offset = 0;
    storage_digest_init(&digest);
    hashing = 0;
    for (unsigned k = 0; k < 32; ++k) if (r->sha256[k]) hashing = 1;
    struct fat32_sink sink = {NULL, sink_consume};
    int status;
    if (r->flags & SS_LARGE_EXTENT) status = stream_extent(d, r->first_sector, r->source_bytes, &sink);
    else {
        struct fat32_entry entry; struct fat32_file file;
        status = mounted ? fat32_lookup(&volume, r->path, &entry) : FAT32_BAD_BOOT;
        if (status == FAT32_OK) status = fat32_open(&volume, &entry, &file);
        if (status == FAT32_OK && file.size != r->source_bytes) status = FAT32_INVALID;
        if (status == FAT32_OK)
            status = fat32_stream(&volume, &file, &d->async, d->buffers, d->buffers + OS_LARGE_BUFFER_BYTES, 256, &sink);
    }
    uint8_t sum[32];
    if (status == FAT32_OK && hashing) {
        storage_digest_end(&digest, sum);
        if (memcmp(sum, r->sha256, 32)) status = FAT32_INVALID - 1;     /* digest mismatch */
    }
    s->finished_us = now_us();
    s->copy_us = ticks_us(copy_ticks); s->digest_us = ticks_us(digest_ticks);
    os_large_finish(index, status == FAT32_OK ? SS_LARGE_READY : SS_LARGE_FAILED, (uint32_t)status);
}

/* No source: ready at once, contents as the memory held them. */
static void unfilled(unsigned index)
{
    struct large_slot *s = os_large_slot_mutable(index);
    s->started_us = s->finished_us = now_us();
    os_large_finish(index, SS_LARGE_READY, 0);
}

static void loader(void *unused)
{
    (void)unused;
    /* The loader runs on one core for good, so that core's own ID_AA64ISAR0_EL1 decides: SHA2 is bits 15:12. */
    uint64_t isar0;
    __asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    (void)storage_digest_use_extensions(((isar0 >> 12) & 15u) != 0);
    /* A region with no source needs no card: when every region is like that the device is never opened. */
    unsigned needing = 0;
    for (unsigned i = 0; i < os_large_count(); ++i) if (!(os_large_record(i)->flags & SS_LARGE_UNFILLED)) ++needing;
    const struct os_large_device *d = needing ? os_large_device() : 0;
    int mounted = 0;
    if (d) {
        uint64_t first, sectors;
        if (partition(d, &first, &sectors)) {
            struct fat32_device dev = d->sync;
            mounted = fat32_mount(&volume, &dev, first, sectors) == FAT32_OK;
        }
    }
    for (unsigned i = 0; i < os_large_count(); ++i) {
        if (os_large_record(i)->flags & SS_LARGE_HEAP) continue;          /* ready at admission */
        if (os_large_record(i)->flags & SS_LARGE_UNFILLED) { unfilled(i); continue; }
        if (!d) { os_large_finish(i, SS_LARGE_FAILED, (uint32_t)FAT32_IO); continue; }
        load(i, d, mounted);
    }
    if (needing) os_large_device_close();
    vTaskDelete(NULL);
}

/* The loader runs at EL1: it writes regions that the application sees read-only and drives the device. */
int os_large_task_privileged(void (*entry)(void *), void *context) { (void)context; return entry == loader; }
/* The SD binding authorizes the pins its card needs (apps/os/large_sd.c); a board without one authorizes none. */
__attribute__((weak)) int os_large_sd_pins(struct a733_pin_handoff *pins) { (void)pins; return 1; }
int os_large_pins(struct a733_pin_handoff *pins) { return os_large_sd_pins(pins); }
int os_large_start(void)
{
    unsigned loadable = 0;
    for (unsigned i = 0; i < os_large_count(); ++i) loadable += !(os_large_record(i)->flags & SS_LARGE_HEAP);
    if (!loadable) return 1;                                              /* a heap alone needs no card */
    return xTaskCreateStaticAffinitySet(loader, "large-load", sizeof task_stack / sizeof task_stack[0], NULL, PRIORITY,
                                        task_stack, &task_state, (UBaseType_t)1 << 0) != NULL;
}

__attribute__((weak)) const struct os_large_device *os_large_device(void) { return 0; }
__attribute__((weak)) void os_large_device_close(void) {}
