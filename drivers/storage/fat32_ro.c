#include "fat32_ro.h"
#include <string.h>

#define SECTOR 512u
#define FAT_MASK UINT32_C(0x0fffffff)
#define FAT_BAD UINT32_C(0x0ffffff7)
#define FAT_EOC UINT32_C(0x0ffffff8)
#define LFN_MAX_SEQ 20u
#define DIR_CLUSTERS_MAX UINT32_C(65536)

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

static int rd(struct fat32 *f, uint64_t sector, uint32_t count, uint8_t *buffer)
{
    if (!count || sector > f->device.sectors || count > f->device.sectors - sector) return 0;
    return f->device.read(f->device.context, sector, count, buffer) == 1;
}

int fat32_mount(struct fat32 *f, const struct fat32_device *d, uint64_t base, uint64_t sectors)
{
    if (!f || !d || !d->read || base > d->sectors || sectors > d->sectors - base || sectors < 2) return FAT32_INVALID;
    memset(f, 0, sizeof *f);
    f->device = *d; f->base = base; f->sectors = sectors;
    f->fat_cached = f->dir_cached = UINT64_MAX;
    f->fsinfo_free = f->fsinfo_next = UINT32_MAX;
    uint8_t b[SECTOR];
    if (!rd(f, base, 1, b)) return FAT32_IO;
    if (b[510] != 0x55 || b[511] != 0xaa) return FAT32_BAD_BOOT;
    uint32_t bytes = le16(b + 11), spc = b[13], reserved = le16(b + 14), fats = b[16];
    if (bytes != SECTOR) return (bytes >= SECTOR && bytes <= 4096 && !(bytes & (bytes - 1))) ? FAT32_UNSUPPORTED : FAT32_BAD_BOOT;
    if (!spc || (spc & (spc - 1)) || spc > 128 || reserved < 2 || fats < 1 || fats > 2) return FAT32_BAD_BOOT;
    if (le16(b + 17) || le16(b + 19) || le16(b + 22)) return FAT32_UNSUPPORTED;    /* FAT12 or FAT16 fields */
    uint32_t total = le32(b + 32), fat_sectors = le32(b + 36), root = le32(b + 44);
    if (!total || !fat_sectors || le16(b + 42)) return FAT32_BAD_BOOT;              /* nonzero filesystem version */
    if (total > sectors) return FAT32_BAD_BOOT;
    uint64_t data = (uint64_t)reserved + (uint64_t)fats * fat_sectors;
    if (data >= total) return FAT32_BAD_BOOT;
    uint32_t clusters = (uint32_t)(((uint64_t)total - data) / spc);
    if (clusters < 65525) return FAT32_UNSUPPORTED;                                 /* FAT12 or FAT16 by cluster count */
    if (clusters > FAT_BAD - 3 || (uint64_t)fat_sectors * (SECTOR / 4) < (uint64_t)clusters + 2) return FAT32_BAD_BOOT;
    if (root < 2 || root >= clusters + 2) return FAT32_BAD_BOOT;
    f->sectors_per_cluster = spc; f->reserved = reserved; f->fats = fats; f->fat_sectors = fat_sectors;
    f->root_cluster = root; f->clusters = clusters; f->data_start = base + data; f->volume_id = le32(b + 67);
    uint32_t media = b[21];
    /* FAT entries 0 and 1 carry the media byte and the end-of-chain mark. */
    if (!rd(f, base + reserved, 1, f->fat_cache)) return FAT32_IO;
    f->fat_cached = base + reserved;
    if ((le32(f->fat_cache) & 0xff) != media || (le32(f->fat_cache + 4) & FAT_MASK) < FAT_EOC) return FAT32_BAD_BOOT;
    uint32_t info = le16(b + 48);
    if (info && info != 0xffff && info < reserved) {
        uint8_t s[SECTOR];
        if (!rd(f, base + info, 1, s)) return FAT32_IO;
        if (le32(s) == 0x41615252 && le32(s + 484) == 0x61417272 && le32(s + 508) == 0xaa550000) {
            uint32_t free = le32(s + 488), next = le32(s + 492);
            f->fsinfo_sector = base + info;
            if (free <= clusters) f->fsinfo_free = free;
            if (next >= 2 && next < clusters + 2) f->fsinfo_next = next;
        }
    }
    return FAT32_OK;
}

uint64_t fat32_cluster_sector(const struct fat32 *f, uint32_t cluster)
{ return f->data_start + (uint64_t)(cluster - 2) * f->sectors_per_cluster; }

int fat32_next(struct fat32 *f, uint32_t cluster, uint32_t *next)
{
    if (!f || !next || cluster < 2 || cluster >= f->clusters + 2) return FAT32_BAD_CHAIN;
    uint64_t sector = f->base + f->reserved + (uint64_t)cluster * 4 / SECTOR;
    if (f->fat_cached != sector) {
        if (!rd(f, sector, 1, f->fat_cache)) { f->fat_cached = UINT64_MAX; return FAT32_IO; }
        f->fat_cached = sector;
    }
    uint32_t v = le32(f->fat_cache + cluster * 4 % SECTOR) & FAT_MASK;
    if (v >= FAT_EOC) { *next = 0; return FAT32_OK; }
    if (v < 2 || v == FAT_BAD || v >= f->clusters + 2) return FAT32_BAD_CHAIN;     /* free, reserved, bad or outside */
    *next = v;
    return FAT32_OK;
}

int fat32_count_free(struct fat32 *f, uint8_t *scratch, uint32_t scratch_sectors, uint32_t *out)
{
    if (!f || !scratch || !scratch_sectors || !out) return FAT32_INVALID;
    uint32_t free = 0, total = f->clusters + 2;
    for (uint32_t done = 0; done < f->fat_sectors && (uint64_t)done * (SECTOR / 4) < total;) {
        uint32_t n = f->fat_sectors - done < scratch_sectors ? f->fat_sectors - done : scratch_sectors;
        if (!rd(f, f->base + f->reserved + done, n, scratch)) return FAT32_IO;
        for (uint32_t i = 0; i < n * (SECTOR / 4); ++i) {
            uint64_t cluster = (uint64_t)done * (SECTOR / 4) + i;
            if (cluster >= 2 && cluster < total && !(le32(scratch + 4 * i) & FAT_MASK)) ++free;
        }
        done += n;
    }
    f->fat_cached = UINT64_MAX;
    *out = free;
    return FAT32_OK;
}

void fat32_dir_open(struct fat32 *f, struct fat32_dir *d, uint32_t first)
{
    (void)f;
    *d = (struct fat32_dir){first, first, 0, 0};
}

static uint8_t short_checksum(const uint8_t *name11)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; ++i) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

int fat32_dir_next(struct fat32 *f, struct fat32_dir *d, struct fat32_entry *out)
{
    if (!f || !d || !out) return FAT32_INVALID;
    uint16_t chars[LFN_MAX_SEQ * 13];
    unsigned active = 0, expected = 0;
    uint8_t checksum = 0;
    uint32_t per_cluster = f->sectors_per_cluster * (SECTOR / 32);
    for (;;) {
        if (!d->cluster) return 0;
        if (d->cluster < 2 || d->cluster >= f->clusters + 2) return FAT32_BAD_DIR;
        uint64_t sector = fat32_cluster_sector(f, d->cluster) + d->index / (SECTOR / 32);
        if (f->dir_cached != sector) {
            if (!rd(f, sector, 1, f->dir_cache)) { f->dir_cached = UINT64_MAX; return FAT32_IO; }
            f->dir_cached = sector;
        }
        const uint8_t *e = f->dir_cache + 32 * (d->index % (SECTOR / 32));
        if (++d->index == per_cluster) {
            uint32_t next;
            int r = fat32_next(f, d->cluster, &next);
            if (r < 0) return r == FAT32_BAD_CHAIN ? FAT32_BAD_DIR : r;
            if (next && ++d->walked >= DIR_CLUSTERS_MAX) return FAT32_BAD_DIR;       /* a looping directory */
            d->cluster = next; d->index = 0;
        }
        if (e[0] == 0x00) { d->cluster = 0; return 0; }
        if (e[0] == 0xe5) { active = 0; continue; }
        uint8_t attr = e[11];
        if ((attr & 0x3f) == 0x0f) {                                               /* long name entry */
            unsigned ord = e[0], seq = ord & 0x3f;
            if (ord & 0x40) {
                active = seq >= 1 && seq <= LFN_MAX_SEQ;
                expected = seq; checksum = e[13];
                if (active) memset(chars, 0xff, sizeof chars);
            } else if (!active || seq != expected - 1 || e[13] != checksum) active = 0;
            if (!active) continue;
            expected = seq;
            static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            for (unsigned i = 0; i < 13; ++i) chars[(seq - 1) * 13 + i] = (uint16_t)le16(e + at[i]);
            continue;
        }
        if ((attr & (FAT32_ATTR_DIRECTORY | 0x08)) == 0x08) { active = 0; continue; }   /* volume label */
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) { active = 0; continue; }
        memset(out, 0, sizeof *out);
        out->attributes = attr;
        out->first_cluster = le16(e + 20) << 16 | le16(e + 26);
        out->size = le32(e + 28);
        out->directory_cluster = d->first;
        if ((attr & FAT32_ATTR_DIRECTORY) && out->size) return FAT32_BAD_DIR;
        if (out->first_cluster && (out->first_cluster < 2 || out->first_cluster >= f->clusters + 2)) return FAT32_BAD_DIR;
        if (active && expected == 1 && short_checksum(e) == checksum) {
            unsigned n = 0;
            for (unsigned i = 0; i < LFN_MAX_SEQ * 13 && chars[i] != 0x0000 && chars[i] != 0xffff; ++i) {
                if (n == FAT32_NAME_MAX) return FAT32_BAD_DIR;
                out->name[n++] = chars[i] < 0x80 ? (char)chars[i] : '?';
            }
            out->long_name = 1;
        } else {
            unsigned n = 0;
            uint8_t lower_name = e[12] & 0x08, lower_ext = e[12] & 0x10;
            for (unsigned i = 0; i < 8 && e[i] != ' '; ++i) {
                char c = (char)(i == 0 && e[0] == 0x05 ? 0xe5 : e[i]);
                out->name[n++] = lower_name && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
            }
            if (e[8] != ' ') {
                out->name[n++] = '.';
                for (unsigned i = 8; i < 11 && e[i] != ' '; ++i) {
                    char c = (char)e[i];
                    out->name[n++] = lower_ext && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
                }
            }
        }
        return 1;
    }
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int same_name(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (lower(a[i]) != lower(b[i])) return 0;
    return !b[n];
}

int fat32_lookup(struct fat32 *f, const char *path, struct fat32_entry *out)
{
    if (!f || !path || !out) return FAT32_INVALID;
    uint32_t dir = f->root_cluster;
    while (*path == '/') ++path;
    if (!*path) return FAT32_INVALID;
    for (;;) {
        const char *end = path;
        while (*end && *end != '/') ++end;
        size_t n = (size_t)(end - path);
        if (!n || n > FAT32_NAME_MAX) return FAT32_INVALID;
        struct fat32_dir it;
        fat32_dir_open(f, &it, dir);
        struct fat32_entry e;
        int r, found = 0;
        while ((r = fat32_dir_next(f, &it, &e)) == 1)
            if (same_name(path, e.name, n)) { found = 1; break; }
        if (r < 0) return r;
        if (!found) return FAT32_NOT_FOUND;
        while (*end == '/') ++end;
        if (!*end) { *out = e; return FAT32_OK; }
        if (!(e.attributes & FAT32_ATTR_DIRECTORY)) return FAT32_NOT_FOUND;
        dir = e.first_cluster ? e.first_cluster : f->root_cluster;
        path = end;
    }
}

int fat32_open(struct fat32 *f, const struct fat32_entry *e, struct fat32_file *out)
{
    if (!f || !e || !out || (e->attributes & (FAT32_ATTR_DIRECTORY | 0x08))) return FAT32_INVALID;
    uint64_t cluster_bytes = (uint64_t)f->sectors_per_cluster * SECTOR;
    *out = (struct fat32_file){e->first_cluster, e->size, 0, e->directory_cluster};
    if (!e->size) return e->first_cluster ? FAT32_CHAIN_LONG : FAT32_OK;           /* an empty file owns no cluster */
    if (!e->first_cluster) return FAT32_TRUNCATED;
    uint64_t need = ((uint64_t)e->size + cluster_bytes - 1) / cluster_bytes;
    uint32_t directory[FAT32_DIR_CLUSTERS_CHECKED], dir_count = 0;
    for (uint32_t c = e->directory_cluster; c && dir_count < FAT32_DIR_CLUSTERS_CHECKED;) {
        directory[dir_count++] = c;
        uint32_t next;
        int r = fat32_next(f, c, &next);
        if (r < 0) return r == FAT32_BAD_CHAIN ? FAT32_BAD_DIR : r;
        c = next;
    }
    uint32_t c = e->first_cluster;
    for (uint64_t n = 1;; ++n) {
        for (uint32_t i = 0; i < dir_count; ++i) if (directory[i] == c) return FAT32_CROSS_LINKED;
        uint32_t next;
        int r = fat32_next(f, c, &next);
        if (r < 0) return r;
        if (!next) {
            if (n < need) return FAT32_TRUNCATED;
            out->clusters = (uint32_t)n;
            return FAT32_OK;
        }
        if (n >= need) return FAT32_CHAIN_LONG;                                    /* includes a looping chain */
        c = next;
    }
}

void fat32_cursor_init(struct fat32_cursor *c, const struct fat32_file *file)
{ *c = (struct fat32_cursor){file->first_cluster, file->size, 0, 0}; }

int fat32_cursor_next(struct fat32 *f, struct fat32_cursor *c, uint32_t max_sectors,
                      uint64_t *sector, uint32_t *sectors, uint64_t *bytes)
{
    if (!f || !c || !max_sectors || !sector || !sectors || !bytes) return FAT32_INVALID;
    if (!c->run_sectors) {
        if (!c->remaining) return 0;
        if (!c->cluster) return FAT32_TRUNCATED;
        uint32_t cl = c->cluster, spc = f->sectors_per_cluster;
        uint64_t cap = (c->remaining + SECTOR - 1) / SECTOR;
        c->run_sector = fat32_cluster_sector(f, cl);
        c->run_sectors = spc;
        for (;;) {
            uint32_t next;
            int r = fat32_next(f, cl, &next);
            if (r < 0) return r;
            if (c->run_sectors >= cap || c->run_sectors >= max_sectors || !next || next != cl + 1) { c->cluster = next; break; }
            c->run_sectors += spc; cl = next;
        }
        if (c->run_sectors > cap) c->run_sectors = (uint32_t)cap;
    }
    uint32_t n = c->run_sectors < max_sectors ? c->run_sectors : max_sectors;
    uint64_t available = (uint64_t)n * SECTOR;
    *sector = c->run_sector; *sectors = n;
    *bytes = available < c->remaining ? available : c->remaining;
    c->run_sector += n; c->run_sectors -= n; c->remaining -= *bytes;
    return 1;
}

int fat32_stream(struct fat32 *f, const struct fat32_file *file, const struct fat32_async *a,
                 uint8_t *buffer0, uint8_t *buffer1, uint32_t buffer_sectors, const struct fat32_sink *sink)
{
    if (!f || !file || !a || !a->submit || !a->poll || !buffer0 || !buffer1 || buffer0 == buffer1 ||
        !buffer_sectors || !sink || !sink->consume) return FAT32_INVALID;
    uint8_t *buffers[2] = {buffer0, buffer1};
    struct fat32_cursor cursor;
    fat32_cursor_init(&cursor, file);
    uint64_t sector, bytes, offset = 0;
    uint32_t sectors;
    int r = fat32_cursor_next(f, &cursor, buffer_sectors, &sector, &sectors, &bytes);
    if (r <= 0) return r;
    unsigned k = 0;
    if (a->submit(a->context, sector, sectors, buffers[0]) != 1) return FAT32_IO;
    for (;;) {
        int p;
        while ((p = a->poll(a->context)) == 0) {}
        if (p < 0) return FAT32_IO;
        uint64_t done_bytes = bytes;
        /* The device is idle: look up the next extent, then start it before consuming this buffer. */
        r = fat32_cursor_next(f, &cursor, buffer_sectors, &sector, &sectors, &bytes);
        if (r < 0) return r;
        if (r == 1 && a->submit(a->context, sector, sectors, buffers[k ^ 1]) != 1) return FAT32_IO;
        if (sink->consume(sink->context, buffers[k], done_bytes, offset) != 1) return FAT32_IO;
        offset += done_bytes;
        if (r == 0) return offset == file->size ? FAT32_OK : FAT32_TRUNCATED;
        k ^= 1;
    }
}
