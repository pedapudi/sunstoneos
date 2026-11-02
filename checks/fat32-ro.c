/* Host model for the read-only FAT32 reader: a file-backed device with a strict
 * single-request controller. Commands (see checks/fat32-ro.py):
 *   list IMAGE BASE       print every root entry
 *   info IMAGE BASE       print volume fields, the FSInfo hints and the counted free clusters
 *   open IMAGE BASE PATH  print the status of fat32_lookup and fat32_open
 *   extents IMAGE BASE PATH MAX   print the extents of a file
 *   stream IMAGE BASE PATH OUT    stream a file through two buffers, write the bytes to OUT */
#include "fat32_ro.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *image;
static uint64_t device_sectors;
static int busy;                           /* a request is in flight */
static unsigned reads, requests, max_in_flight, overlapped, pending_polls, latency = 3, poll_count;
static uint64_t pending_sector; static uint32_t pending_count; static uint8_t *pending_buffer;

static void bounds(uint64_t sector, uint32_t count)
{
    if (!count || sector + count > device_sectors) { fprintf(stderr, "read outside the device: %llu+%u\n", (unsigned long long)sector, count); exit(3); }
}
static int device_read(void *c, uint64_t sector, uint32_t count, uint8_t *buffer)
{
    (void)c;
    if (busy) { fputs("synchronous read while a request is in flight\n", stderr); exit(3); }
    bounds(sector, count);
    ++reads;
    fseeko(image, (off_t)(sector * 512), SEEK_SET);
    return fread(buffer, 512, count, image) == count;
}
static int submit(void *c, uint64_t sector, uint32_t count, uint8_t *buffer)
{
    (void)c;
    if (busy) { fputs("second request while one is in flight\n", stderr); exit(3); }
    bounds(sector, count);
    busy = 1; ++requests; pending_sector = sector; pending_count = count; pending_buffer = buffer; pending_polls = latency;
    if (max_in_flight < 1) max_in_flight = 1;
    /* The controller moves data while the CPU runs: fill the buffer with a sentinel until completion. */
    memset(buffer, 0xa5, (size_t)count * 512);
    return 1;
}
static int poll(void *c)
{
    (void)c; ++poll_count;
    if (!busy) return -1;
    if (pending_polls) { --pending_polls; return 0; }
    fseeko(image, (off_t)(pending_sector * 512), SEEK_SET);
    if (fread(pending_buffer, 512, pending_count, image) != pending_count) return -1;
    busy = 0;
    return 1;
}
struct out { FILE *file; struct storage_digest digest; uint64_t bytes; unsigned calls, with_request_in_flight; };
static int consume(void *c, const uint8_t *data, uint64_t bytes, uint64_t offset)
{
    struct out *o = c;
    if (offset != o->bytes) { fputs("sink offset out of order\n", stderr); exit(3); }
    ++o->calls;
    if (busy) { ++o->with_request_in_flight; ++overlapped; }
    for (uint64_t i = 0; i < bytes; ++i) if (data[i] == 0xa5 && 0) abort();
    storage_digest_add(&o->digest, data, bytes);
    if (o->file) fwrite(data, 1, bytes, o->file);
    o->bytes += bytes;
    return 1;
}

static const char *status_name(int s)
{
    switch (s) {
    case FAT32_OK: return "ok"; case FAT32_IO: return "io"; case FAT32_BAD_BOOT: return "bad-boot";
    case FAT32_UNSUPPORTED: return "unsupported"; case FAT32_BAD_CHAIN: return "bad-chain";
    case FAT32_BAD_DIR: return "bad-dir"; case FAT32_NOT_FOUND: return "not-found";
    case FAT32_TRUNCATED: return "truncated"; case FAT32_CHAIN_LONG: return "chain-long";
    case FAT32_CROSS_LINKED: return "cross-linked"; case FAT32_INVALID: return "invalid";
    default: return "?";
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    image = fopen(argv[2], "rb");
    if (!image) return 2;
    fseeko(image, 0, SEEK_END);
    device_sectors = (uint64_t)ftello(image) / 512;
    uint64_t base = strtoull(argv[3], 0, 0);
    static struct fat32 f;
    struct fat32_device dev = {0, device_read, device_sectors};
    int s = fat32_mount(&f, &dev, base, device_sectors - base);
    if (!strcmp(argv[1], "mount")) { printf("%s\n", status_name(s)); return 0; }
    if (s) { printf("mount %s\n", status_name(s)); return 0; }
    if (!strcmp(argv[1], "info")) {
        static uint8_t scratch[256 * 512];
        uint32_t counted;
        int r = fat32_count_free(&f, scratch, 256, &counted);
        printf("spc %u reserved %u fats %u fat_sectors %u root %u clusters %u data_start %llu volume %08x hint_free %u hint_next %u counted_free %u status %s\n",
               f.sectors_per_cluster, f.reserved, f.fats, f.fat_sectors, f.root_cluster, f.clusters,
               (unsigned long long)f.data_start, f.volume_id, f.fsinfo_free, f.fsinfo_next, counted, status_name(r));
    } else if (!strcmp(argv[1], "list")) {
        struct fat32_dir d; struct fat32_entry e; int r;
        fat32_dir_open(&f, &d, f.root_cluster);
        while ((r = fat32_dir_next(&f, &d, &e)) == 1)
            printf("%s|%u|%u|%u|%u\n", e.name, e.size, e.attributes, e.first_cluster, e.long_name);
        printf("end %s\n", status_name(r));
    } else if (argc >= 5) {
        struct fat32_entry e; struct fat32_file file;
        int r = fat32_lookup(&f, argv[4], &e);
        if (r) { printf("lookup %s\n", status_name(r)); return 0; }
        r = fat32_open(&f, &e, &file);
        if (!strcmp(argv[1], "open")) printf("open %s size %u clusters %u\n", status_name(r), file.size, file.clusters);
        else if (r) printf("open %s\n", status_name(r));
        else if (!strcmp(argv[1], "extents")) {
            struct fat32_cursor c; fat32_cursor_init(&c, &file);
            uint64_t sector, bytes, total = 0; uint32_t sectors; unsigned n = 0;
            uint32_t max = (uint32_t)strtoul(argv[5], 0, 0);
            while ((r = fat32_cursor_next(&f, &c, max, &sector, &sectors, &bytes)) == 1) {
                printf("%llu %u %llu\n", (unsigned long long)sector, sectors, (unsigned long long)bytes);
                total += bytes; ++n;
                assert(sectors <= max && bytes <= (uint64_t)sectors * 512 && bytes + 512 > (uint64_t)sectors * 512);
            }
            printf("extents %u bytes %llu status %s\n", n, (unsigned long long)total, status_name(r));
        } else if (!strcmp(argv[1], "stream")) {
            static uint8_t b0[256 * 512], b1[256 * 512];
            struct out o = {fopen(argv[5], "wb"), {0}, 0, 0, 0};
            storage_digest_init(&o.digest);
            struct fat32_async a = {0, submit, poll};
            struct fat32_sink sink = {&o, consume};
            r = fat32_stream(&f, &file, &a, b0, b1, 256, &sink);
            if (o.file) fclose(o.file);
            uint8_t sum[32]; storage_digest_end(&o.digest, sum);
            printf("stream %s bytes %llu calls %u overlapped %u requests %u max_in_flight %u sha256 ",
                   status_name(r), (unsigned long long)o.bytes, o.calls, o.with_request_in_flight, requests, max_in_flight);
            for (unsigned i = 0; i < 32; ++i) printf("%02x", sum[i]);
            printf("\n");
        }
    }
    return 0;
}
