/* Host driver for the FAT32 file-create module: a file-backed device that logs every write.
 *   plan   IMAGE BASE NAME SIZE                  print the plan and the bounded sets
 *   create IMAGE BASE NAME SIZE LOG [tamper]     create, write the image, log "sector count" per write
 *   allowed IMAGE BASE NAME SIZE                 print the merged ranges of allowed sectors of the whole image
 *   check  IMAGE BASE                            print the consistency report */
#include "fat32_create.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *image, *log_file;
static uint64_t device_sectors;
static unsigned writes_before_failure = UINT32_MAX;

static int device_read(void *c, uint64_t sector, uint32_t count, uint8_t *buffer)
{
    (void)c;
    if (sector + count > device_sectors) { fprintf(stderr, "read outside the device\n"); exit(3); }
    fseeko(image, (off_t)(sector * 512), SEEK_SET);
    return fread(buffer, 512, count, image) == count;
}
static int device_write(void *c, uint64_t sector, uint32_t count, const uint8_t *data)
{
    (void)c;
    if (sector + count > device_sectors) { fprintf(stderr, "write outside the device\n"); exit(3); }
    if (!writes_before_failure--) return 0;
    if (log_file) fprintf(log_file, "%llu %u\n", (unsigned long long)sector, count);
    fseeko(image, (off_t)(sector * 512), SEEK_SET);
    return fwrite(data, 512, count, image) == count;
}
static const char *name_of(int s)
{
    switch (s) {
    case FAT32_CREATE_OK: return "ok"; case FAT32_CREATE_IO: return "io"; case FAT32_CREATE_NO_SPACE: return "no-space";
    case FAT32_CREATE_EXISTS: return "exists"; case FAT32_CREATE_NO_SLOT: return "no-slot";
    case FAT32_CREATE_OUT_OF_BOUNDS: return "out-of-bounds"; case FAT32_CREATE_INVALID: return "invalid";
    case FAT32_CREATE_VOLUME: return "volume"; case FAT32_CREATE_NOT_FREE: return "not-free"; default: return "?";
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) return 2;
    image = fopen(argv[2], "r+b");
    if (!image) return 2;
    fseeko(image, 0, SEEK_END);
    device_sectors = (uint64_t)ftello(image) / 512;
    uint64_t base = strtoull(argv[3], 0, 0);
    static struct fat32 f;
    struct fat32_device dev = {0, device_read, device_sectors};
    int s = fat32_mount(&f, &dev, base, device_sectors - base);
    if (s) { printf("mount %d\n", s); return 0; }
    static uint8_t scratch[256 * 512], scratch2[256 * 512], buffer[256 * 512];
    if (!strcmp(argv[1], "check")) {
        struct fat32_consistency c;
        int r = fat32_check_volume(&f, scratch, scratch2, 256, &c);
        printf("status %d fats_equal %u fsinfo_valid %u fsinfo_free %u counted_free %u fsinfo_next %u differing %llu\n", r,
               c.fats_equal, c.fsinfo_valid, c.fsinfo_free, c.counted_free, c.fsinfo_next, (unsigned long long)c.differing_fat_sectors);
        return 0;
    }
    if (argc < 6) return 2;
    uint64_t size = strtoull(argv[5], 0, 0);
    struct fat32_create_plan plan;
    int r = fat32_create_plan(&f, argv[4], size, 0x5d62, 0, scratch, 256, &plan);
    if (r) { printf("plan %s\n", name_of(r)); return 0; }
    struct fat32_create_bounds b;
    if (!strcmp(argv[1], "plan") || !strcmp(argv[1], "create")) {
        if (argc > 7 && !strcmp(argv[7], "tamper-root")) plan.first_cluster = 2;
        else if (argc > 7 && !strcmp(argv[7], "tamper-beyond")) plan.first_cluster = f.clusters + 1;
        else if (argc > 7 && !strcmp(argv[7], "tamper-count")) plan.clusters = f.clusters;
        else if (argc > 7 && !strcmp(argv[7], "tamper-slots")) plan.entry_index = 100000;
        else if (argc > 7 && !strcmp(argv[7], "stale-fat")) {          /* the volume changes between plan and execute */
            uint32_t cluster = plan.first_cluster + 5;
            uint8_t entry[4] = {0xf8, 0xff, 0xff, 0x0f};
            for (uint32_t copy = 0; copy < f.fats; ++copy) {
                off_t at = (off_t)((f.base + f.reserved + (uint64_t)copy * f.fat_sectors) * 512 + cluster * 4);
                fseeko(image, at, SEEK_SET); fwrite(entry, 1, 4, image); fflush(image);
            }
            f.fat_cached = UINT64_MAX;
        } else if (argc > 7 && !strcmp(argv[7], "stale-slot")) {
            uint8_t entry[32] = {'X', 'X', 'X', ' ', ' ', ' ', ' ', ' ', 'D', 'A', 'T', 0x20};
            off_t at = (off_t)(fat32_cluster_sector(&f, f.root_cluster) * 512 + 32 * plan.entry_index);
            fseeko(image, at, SEEK_SET); fwrite(entry, 1, 32, image); fflush(image);
            f.dir_cached = UINT64_MAX;
        }
        else if (argc > 7 && !strcmp(argv[7], "fail-3")) writes_before_failure = 3;
        int ok = fat32_create_bounds(&f, &plan, &b);
        printf("plan ok first_cluster %u clusters %u entry_index %u entries %u short %.11s free_before %u bounds %d\n", plan.first_cluster,
               plan.clusters, plan.entry_index, plan.entry_count, plan.short_name, plan.free_before, ok);
        if (ok)
            printf("data %llu %llu fat0 %llu fat1 %llu fat_sectors %llu fsinfo %llu root %llu %llu\n", (unsigned long long)b.data_first,
                   (unsigned long long)b.data_sectors, (unsigned long long)b.fat_first[0], (unsigned long long)b.fat_first[1],
                   (unsigned long long)b.fat_sectors, (unsigned long long)b.fsinfo, (unsigned long long)b.root_first,
                   (unsigned long long)b.root_sectors);
        if (!strcmp(argv[1], "create")) {
            log_file = fopen(argv[6], "w");
            struct fat32_create_io io = {0, device_write, region_test_fill};
            if (argc > 8 && !strcmp(argv[8], "phases")) {      /* data in three slices, then each later phase */
                uint64_t total = (uint64_t)plan.clusters * f.sectors_per_cluster, third = total / 3;
                struct fat32_create_run runs[] = {{FAT32_CREATE_DATA, 0, third}, {FAT32_CREATE_DATA, third, third},
                    {FAT32_CREATE_DATA, 2 * third, UINT64_MAX}, {FAT32_CREATE_FAT, 0, 0}, {FAT32_CREATE_DIR, 0, 0}, {FAT32_CREATE_INFO, 0, 0}};
                r = 0;
                for (unsigned i = 0; i < 6 && !r; ++i) r = fat32_create_execute(&f, &plan, &io, buffer, 256, &runs[i]);
            } else
            r = fat32_create_execute(&f, &plan, &io, buffer, 256, NULL);
            if (log_file) fclose(log_file);
            printf("create %s\n", name_of(r));
        }
    } else if (!strcmp(argv[1], "allowed")) {
        uint64_t start = UINT64_MAX;
        for (uint64_t sct = 0; sct <= device_sectors; ++sct) {
            int a = sct < device_sectors && fat32_create_allowed(&f, &plan, sct);
            if (a && start == UINT64_MAX) start = sct;
            if (!a && start != UINT64_MAX) { printf("%llu %llu\n", (unsigned long long)start, (unsigned long long)(sct - start)); start = UINT64_MAX; }
        }
    }
    return 0;
}
