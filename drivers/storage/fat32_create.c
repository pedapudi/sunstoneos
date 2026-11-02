#include "fat32_create.h"
#include "a733_storage_digest.h"
#include <string.h>

#define SECTOR 512u
#define FAT_MASK UINT32_C(0x0fffffff)
#define FAT_EOC UINT32_C(0x0fffffff)

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int rd(struct fat32 *f, uint64_t sector, uint32_t count, uint8_t *buffer)
{
    if (!count || sector > f->device.sectors || count > f->device.sectors - sector) return 0;
    return f->device.read(f->device.context, sector, count, buffer) == 1;
}

/* Bounds from the volume geometry and the plan's cluster numbers only. */
int fat32_create_bounds(const struct fat32 *f, const struct fat32_create_plan *p, struct fat32_create_bounds *b)
{
    if (!f || !p || !b || !f->clusters || p->clusters == 0 || p->first_cluster < 3 ||
        p->first_cluster > f->clusters + 1 || p->clusters > f->clusters + 2 - p->first_cluster) return 0;
    uint64_t last = (uint64_t)p->first_cluster + p->clusters - 1;
    memset(b, 0, sizeof *b);
    b->data_first = f->data_start + (uint64_t)(p->first_cluster - 2) * f->sectors_per_cluster;
    b->data_sectors = (uint64_t)p->clusters * f->sectors_per_cluster;
    uint64_t first_index = (uint64_t)p->first_cluster * 4 / SECTOR, last_index = last * 4 / SECTOR;
    b->fats = f->fats;
    b->fat_sectors = last_index - first_index + 1;
    for (uint32_t i = 0; i < f->fats && i < 2; ++i)
        b->fat_first[i] = f->base + f->reserved + (uint64_t)i * f->fat_sectors + first_index;
    b->fsinfo = f->fsinfo_sector;
    b->root_first = fat32_cluster_sector(f, f->root_cluster);
    b->root_sectors = f->sectors_per_cluster;
    return 1;
}

int fat32_create_allowed(const struct fat32 *f, const struct fat32_create_plan *p, uint64_t sector)
{
    struct fat32_create_bounds b;
    if (!fat32_create_bounds(f, p, &b)) return 0;
    if (sector >= b.data_first && sector - b.data_first < b.data_sectors) return 1;
    for (uint32_t i = 0; i < b.fats && i < 2; ++i)
        if (sector >= b.fat_first[i] && sector - b.fat_first[i] < b.fat_sectors) return 1;
    if (b.fsinfo && sector == b.fsinfo) return 1;
    return sector >= b.root_first && sector - b.root_first < b.root_sectors;
}

static uint8_t checksum(const uint8_t *name11)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; ++i) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

/* Short name from the long one: up to six characters of the stem, "~n", the first three of the extension. */
static int short_name_candidate(const char *name, unsigned n, char out[11])
{
    static const char ok[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_";
    size_t len = strlen(name), dot = len;
    for (size_t i = 0; i < len; ++i) if (name[i] == '.') dot = i;
    memset(out, ' ', 11);
    unsigned stem = 0, ext = 0;
    for (size_t i = 0; i < dot && stem < 6; ++i) {
        char c = name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 32) : name[i];
        if (strchr(ok, c) && c) out[stem++] = c;
    }
    if (!stem) return 0;
    char tail[8];
    unsigned t = 0;
    tail[t++] = '~';
    if (n >= 100) tail[t++] = (char)('0' + n / 100 % 10);
    if (n >= 10) tail[t++] = (char)('0' + n / 10 % 10);
    tail[t++] = (char)('0' + n % 10);
    if (stem + t > 8) stem = 8 - t;
    memcpy(out + stem, tail, t);
    if (dot < len) for (size_t i = dot + 1; i < len && ext < 3; ++i) {
        char c = name[i] >= 'a' && name[i] <= 'z' ? (char)(name[i] - 32) : name[i];
        if (strchr(ok, c) && c) out[8 + ext++] = c;
    }
    return 1;
}

static int same_ci(const char *a, const char *b)
{
    for (;; ++a, ++b) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a, y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

int fat32_create_plan(struct fat32 *f, const char *name, uint64_t size, uint16_t date, uint16_t time,
                      uint8_t *scratch, uint32_t scratch_sectors, struct fat32_create_plan *p)
{
    if (!f || !name || !p || !scratch || !scratch_sectors || !size || size > UINT32_MAX || !f->clusters) return FAT32_CREATE_INVALID;
    size_t len = strlen(name);
    if (!len || len > FAT32_NAME_MAX) return FAT32_CREATE_INVALID;
    for (size_t i = 0; i < len; ++i) if ((uint8_t)name[i] < 0x20 || (uint8_t)name[i] >= 0x7f || strchr("\\/:*?\"<>|", name[i]))
        return FAT32_CREATE_INVALID;
    memset(p, 0, sizeof *p);
    uint64_t cluster_bytes = (uint64_t)f->sectors_per_cluster * SECTOR;
    p->size = size;
    p->clusters = (uint32_t)((size + cluster_bytes - 1) / cluster_bytes);
    p->date = date; p->time = time;
    p->free_before = f->fsinfo_free; p->next_before = f->fsinfo_next;
    if (!f->fsinfo_sector || f->fsinfo_free == UINT32_MAX) return FAT32_CREATE_VOLUME;
    /* The name must not exist, and the short name must not collide. */
    struct fat32_dir d; struct fat32_entry e; int r;
    char shorts[11];
    unsigned attempt = 1;
    fat32_dir_open(f, &d, f->root_cluster);
    while ((r = fat32_dir_next(f, &d, &e)) == 1) {
        if (same_ci(e.name, name)) return FAT32_CREATE_EXISTS;
    }
    if (r < 0) return FAT32_CREATE_VOLUME;
    /* Raw scan of the root cluster for the short names and a free run of slots. */
    uint32_t spc = f->sectors_per_cluster, slots = spc * (SECTOR / 32);
    unsigned long_entries = (unsigned)((len + 12) / 13), need = long_entries + 1;
    if (need > FAT32_CREATE_MAX_ENTRIES) return FAT32_CREATE_INVALID;
    uint8_t sec[SECTOR];
    uint32_t run_start = 0, run_len = 0, found = UINT32_MAX;
    uint8_t names[256][11];
    unsigned name_count = 0;
    for (uint32_t i = 0; i < slots; ++i) {
        if (i % (SECTOR / 32) == 0 && !rd(f, fat32_cluster_sector(f, f->root_cluster) + i / (SECTOR / 32), 1, sec)) return FAT32_CREATE_IO;
        const uint8_t *en = sec + 32 * (i % (SECTOR / 32));
        int free_slot = en[0] == 0x00 || en[0] == 0xe5;
        if (!free_slot && (en[11] & 0x3f) != 0x0f && name_count < 256) memcpy(names[name_count++], en, 11);
        if (free_slot) { if (!run_len) run_start = i; if (++run_len == need && found == UINT32_MAX) found = run_start; }
        else run_len = 0;
        if (en[0] == 0x00 && found != UINT32_MAX) break;
    }
    if (found == UINT32_MAX) return FAT32_CREATE_NO_SLOT;
    for (;; ++attempt) {
        if (attempt > 99 || !short_name_candidate(name, attempt, shorts)) return FAT32_CREATE_INVALID;
        int clash = 0;
        for (unsigned i = 0; i < name_count; ++i) if (!memcmp(names[i], shorts, 11)) clash = 1;
        if (!clash) break;
    }
    memcpy(p->short_name, shorts, 11); p->short_name[11] = 0;
    p->entry_index = found; p->entry_count = need;
    /* First free run of the file's size, scanning the FAT from cluster 3. */
    uint32_t total = f->clusters + 2, run = 0, start = 0;
    p->first_cluster = 0;
    for (uint32_t base = 0; base < f->fat_sectors && (uint64_t)base * (SECTOR / 4) < total && !p->first_cluster;) {
        uint32_t n = f->fat_sectors - base < scratch_sectors ? f->fat_sectors - base : scratch_sectors;
        if (!rd(f, f->base + f->reserved + base, n, scratch)) return FAT32_CREATE_IO;
        for (uint32_t i = 0; i < n * (SECTOR / 4) && !p->first_cluster; ++i) {
            uint64_t c = (uint64_t)base * (SECTOR / 4) + i;
            if (c < 3 || c >= total) { run = 0; continue; }
            if (get32(scratch + 4 * i) & FAT_MASK) { run = 0; continue; }
            if (!run) start = (uint32_t)c;
            if (++run == p->clusters) p->first_cluster = start;
        }
        base += n;
    }
    f->fat_cached = UINT64_MAX;
    if (!p->first_cluster) return FAT32_CREATE_NO_SPACE;
    /* The directory entries: long entries from the last, then the short entry. */
    uint8_t sum = checksum((const uint8_t *)shorts);
    for (unsigned k = 0; k < long_entries; ++k) {
        uint8_t *en = p->entries[k];
        unsigned seq = long_entries - k;
        memset(en, 0xff, 32);
        en[0] = (uint8_t)(seq | (seq == long_entries ? 0x40 : 0));
        en[11] = 0x0f; en[12] = 0; en[13] = sum; put16(en + 26, 0);
        static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (unsigned i = 0; i < 13; ++i) {
            size_t idx = (size_t)(seq - 1) * 13 + i;
            uint32_t ch = idx < len ? (uint8_t)name[idx] : idx == len ? 0x0000 : 0xffff;
            put16(en + at[i], ch);
        }
    }
    uint8_t *sfn = p->entries[long_entries];
    memset(sfn, 0, 32);
    memcpy(sfn, shorts, 11);
    sfn[11] = 0x20;
    put16(sfn + 14, time); put16(sfn + 16, date); put16(sfn + 18, date);
    put16(sfn + 22, time); put16(sfn + 24, date);
    put16(sfn + 20, p->first_cluster >> 16); put16(sfn + 26, p->first_cluster & 0xffff);
    put32(sfn + 28, (uint32_t)size);
    return FAT32_CREATE_OK;
}

static int guarded_write(struct fat32 *f, const struct fat32_create_plan *p, const struct fat32_create_io *io,
                         uint64_t sector, uint32_t count, const uint8_t *data)
{
    for (uint32_t i = 0; i < count; ++i)
        if (!fat32_create_allowed(f, p, sector + i)) return FAT32_CREATE_OUT_OF_BOUNDS;
    return io->write(io->context, sector, count, data) == 1 ? FAT32_CREATE_OK : FAT32_CREATE_IO;
}

int fat32_create_execute(struct fat32 *f, const struct fat32_create_plan *p, const struct fat32_create_io *io,
                         uint8_t *buffer, uint32_t buffer_sectors, const struct fat32_create_run *run)
{
    struct fat32_create_bounds b;
    const struct fat32_create_run whole = {FAT32_CREATE_DATA | FAT32_CREATE_FAT | FAT32_CREATE_DIR | FAT32_CREATE_INFO, 0, UINT64_MAX};
    if (!run) run = &whole;
    if (!f || !p || !io || !io->write || !io->fill || !buffer || !buffer_sectors ||
        !fat32_create_bounds(f, p, &b) || !b.fsinfo || p->entry_count == 0 || p->entry_count > FAT32_CREATE_MAX_ENTRIES ||
        p->entry_index + p->entry_count > f->sectors_per_cluster * (SECTOR / 32)) return FAT32_CREATE_INVALID;
    int r;
    uint64_t first_index = (uint64_t)p->first_cluster * 4 / SECTOR;
    uint32_t last = p->first_cluster + p->clusters - 1;
    /* 0. Nothing is written until the volume still shows what the plan saw: every chain entry free in every
     * FAT copy, and the directory slots free. */
    if (run->phases & ~15u) return FAT32_CREATE_INVALID;
    if (run->phases & (FAT32_CREATE_DATA | FAT32_CREATE_FAT))
    for (uint32_t copy = 0; copy < f->fats && copy < 2; ++copy)
        for (uint64_t s = 0; s < b.fat_sectors; ++s) {
            uint8_t sec[SECTOR];
            if (!rd(f, b.fat_first[copy] + s, 1, sec)) return FAT32_CREATE_IO;
            for (uint32_t i = 0; i < SECTOR / 4; ++i) {
                uint64_t c = (first_index + s) * (SECTOR / 4) + i;
                if (c >= p->first_cluster && c <= last && (get32(sec + 4 * i) & FAT_MASK)) return FAT32_CREATE_NOT_FREE;
            }
        }
    if (run->phases & (FAT32_CREATE_DATA | FAT32_CREATE_DIR))
    for (uint32_t k = 0; k < p->entry_count; ++k) {
        uint32_t slot = p->entry_index + k;
        uint8_t sec[SECTOR];
        if (!rd(f, b.root_first + slot / (SECTOR / 32), 1, sec)) return FAT32_CREATE_IO;
        uint8_t first = sec[32 * (slot % (SECTOR / 32))];
        if (first != 0x00 && first != 0xe5) return FAT32_CREATE_NOT_FREE;
    }
    /* 1. Data clusters, in chunks, the last sector padded with zeros. */
    uint64_t done = run->data_from, total_sectors = b.data_sectors;
    if (run->data_sectors != UINT64_MAX && run->data_sectors < total_sectors - done) total_sectors = done + run->data_sectors;
    if (done > b.data_sectors) return FAT32_CREATE_INVALID;
    if (!(run->phases & FAT32_CREATE_DATA)) done = total_sectors;
    while (done < total_sectors) {
        uint32_t n = total_sectors - done < buffer_sectors ? (uint32_t)(total_sectors - done) : buffer_sectors;
        uint64_t offset = done * SECTOR, want = p->size > offset ? p->size - offset : 0;
        uint32_t bytes = want > (uint64_t)n * SECTOR ? n * SECTOR : (uint32_t)want;
        if (bytes && io->fill(io->context, offset, buffer, bytes) != 1) return FAT32_CREATE_IO;
        memset(buffer + bytes, 0, (size_t)n * SECTOR - bytes);
        if ((r = guarded_write(f, p, io, b.data_first + done, n, buffer))) return r;
        done += n;
    }
    /* 2. The chain, one FAT copy after the other. Each entry the plan chains must read free. */
    if (run->phases & FAT32_CREATE_FAT)
    for (uint32_t copy = 0; copy < f->fats && copy < 2; ++copy)
        for (uint64_t s = 0; s < b.fat_sectors; ++s) {
            uint8_t sec[SECTOR];
            if (!rd(f, b.fat_first[copy] + s, 1, sec)) return FAT32_CREATE_IO;
            uint64_t base_cluster = (first_index + s) * (SECTOR / 4);
            for (uint32_t i = 0; i < SECTOR / 4; ++i) {
                uint64_t c = base_cluster + i;
                if (c < p->first_cluster || c > last) continue;
                if (get32(sec + 4 * i) & FAT_MASK) return FAT32_CREATE_NOT_FREE;
                uint32_t v = c == last ? FAT_EOC : (uint32_t)c + 1;
                put32(sec + 4 * i, (get32(sec + 4 * i) & ~FAT_MASK) | v);
            }
            if ((r = guarded_write(f, p, io, b.fat_first[copy] + s, 1, sec))) return r;
        }
    /* 3. The directory entries, the sector with the long entries first. */
    uint32_t first_sector = p->entry_index / (SECTOR / 32), last_sector = (p->entry_index + p->entry_count - 1) / (SECTOR / 32);
    if (run->phases & FAT32_CREATE_DIR)
    for (uint32_t s = first_sector; s <= last_sector; ++s) {
        uint8_t sec[SECTOR];
        if (!rd(f, b.root_first + s, 1, sec)) return FAT32_CREATE_IO;
        for (uint32_t k = 0; k < p->entry_count; ++k) {
            uint32_t slot = p->entry_index + k;
            if (slot / (SECTOR / 32) != s) continue;
            uint8_t *en = sec + 32 * (slot % (SECTOR / 32));
            if (en[0] != 0x00 && en[0] != 0xe5) return FAT32_CREATE_NOT_FREE;
            memcpy(en, p->entries[k], 32);
        }
        if ((r = guarded_write(f, p, io, b.root_first + s, 1, sec))) return r;
    }
    /* 4. FSInfo. */
    if (!(run->phases & FAT32_CREATE_INFO)) return FAT32_CREATE_OK;
    uint8_t info[SECTOR];
    if (!rd(f, b.fsinfo, 1, info)) return FAT32_CREATE_IO;
    if (get32(info) != 0x41615252 || get32(info + 484) != 0x61417272 || get32(info + 508) != 0xaa550000) return FAT32_CREATE_VOLUME;
    uint32_t next = p->first_cluster + p->clusters;
    put32(info + 488, p->free_before - p->clusters);
    put32(info + 492, next < f->clusters + 2 ? next : 2);
    return guarded_write(f, p, io, b.fsinfo, 1, info);
}

int fat32_check_volume(struct fat32 *f, uint8_t *a, uint8_t *b, uint32_t scratch_sectors, struct fat32_consistency *out)
{
    if (!f || !a || !b || !scratch_sectors || !out) return FAT32_INVALID;
    memset(out, 0, sizeof *out);
    out->first_difference = UINT64_MAX;
    out->fats_equal = 1;
    uint32_t total = f->clusters + 2, counted = 0;
    for (uint32_t base = 0; base < f->fat_sectors;) {
        uint32_t n = f->fat_sectors - base < scratch_sectors ? f->fat_sectors - base : scratch_sectors;
        if (!rd(f, f->base + f->reserved + base, n, a)) return FAT32_IO;
        if (f->fats > 1) {
            if (!rd(f, f->base + f->reserved + f->fat_sectors + base, n, b)) return FAT32_IO;
            for (uint32_t s = 0; s < n; ++s)
                if (memcmp(a + s * SECTOR, b + s * SECTOR, SECTOR)) {
                    out->fats_equal = 0; ++out->differing_fat_sectors;
                    if (out->first_difference == UINT64_MAX) out->first_difference = base + s;
                }
        }
        for (uint32_t i = 0; i < n * (SECTOR / 4); ++i) {
            uint64_t c = (uint64_t)base * (SECTOR / 4) + i;
            if (c >= 2 && c < total && !(get32(a + 4 * i) & FAT_MASK)) ++counted;
        }
        base += n;
    }
    f->fat_cached = UINT64_MAX;
    out->counted_free = counted;
    out->fsinfo_valid = f->fsinfo_sector != 0 && f->fsinfo_free != UINT32_MAX;
    out->fsinfo_free = f->fsinfo_free; out->fsinfo_next = f->fsinfo_next;
    return FAT32_OK;
}

/* Counter-mode SHA-256 stream: block i is SHA-256("sunstone-region-test-v1" || i, 8 bytes little endian). */
int region_test_fill(void *unused, uint64_t offset, uint8_t *out, uint32_t bytes)
{
    (void)unused;
    static const char prefix[] = "sunstone-region-test-v1";
    uint64_t block = offset / 32;
    uint32_t skip = (uint32_t)(offset % 32), done = 0;
    while (done < bytes) {
        uint8_t message[sizeof prefix - 1 + 8], digest[32];
        memcpy(message, prefix, sizeof prefix - 1);
        for (unsigned i = 0; i < 8; ++i) message[sizeof prefix - 1 + i] = (uint8_t)(block >> (8 * i));
        storage_digest_bytes(message, sizeof message, digest);
        uint32_t n = 32 - skip < bytes - done ? 32 - skip : bytes - done;
        memcpy(out + done, digest + skip, n);
        done += n; skip = 0; ++block;
    }
    return 1;
}
