#include "fat32_rw.h"
#include <string.h>

#define S FWR_SECTOR
#define MASK UINT32_C(0x0fffffff)
#define BAD UINT32_C(0x0ffffff7)
#define EOC UINT32_C(0x0fffffff)
#define EOC_MIN UINT32_C(0x0ffffff8)
#define CLEAN_BIT UINT32_C(0x08000000)
#define DIR_MAX_CLUSTERS 65536u
#define ENTRIES (S / 32)

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

/* ---------------------------------------------------------------- device access */

static uint64_t cluster_sector(const struct fwr_volume *v, uint32_t c)
{ return v->data_start + (uint64_t)(c - 2) * v->spc; }

static int dev_read(struct fwr_volume *v, uint64_t sector, uint32_t count, uint8_t *buffer)
{
    const struct fwr_device *d = &v->cfg.device;
    if (!count || sector < v->cfg.base || sector + count > v->total_end) return FWR_REFUSED;
    return d->read(d->context, sector, count, buffer) == 1 ? FWR_OK : FWR_IO;
}

/* The only writable regions: both FATs, the FSInfo sector, and the data area at or after the floor. */
static int guard(const struct fwr_volume *v, uint64_t s, uint32_t n)
{
    uint64_t e = s + n;
    if (!n || e < s) return 0;
    if (s >= v->fat1 && e <= v->fat1 + v->fat_sectors) return 1;
    if (v->fats == 2 && s >= v->fat2 && e <= v->fat2 + v->fat_sectors) return 1;
    if (v->fsinfo_valid && s == v->fsinfo && n == 1) return 1;
    uint64_t lo = v->data_start, hi = v->data_start + (uint64_t)v->clusters * v->spc;
    if (v->cfg.write_floor > lo) lo = v->cfg.write_floor;
    return s >= lo && e <= hi;
}

int fwr_write_allowed(const struct fwr_volume *v, uint64_t sector, uint32_t count) { return guard(v, sector, count); }

static unsigned bucket(uint32_t count)
{
    unsigned b = 0;
    while (b < FWR_HIST - 1 && ((uint32_t)1 << b) < count) ++b;
    return b;
}

static int dev_write(struct fwr_volume *v, uint64_t sector, uint32_t count, const uint8_t *data, int is_data)
{
    const struct fwr_device *d = &v->cfg.device;
    if (!guard(v, sector, count)) return FWR_REFUSED;
    while (count) {
        /* Pieces start and end on multiples of the card's write limit, so the card sees aligned
         * pieces whatever the first sector is; with a limit of one unit or more they also end on
         * allocation unit boundaries. */
        uint32_t n = v->max_write - (uint32_t)(sector % v->max_write);
        if (n > count) n = count;
        if (n < count && v->max_write >= v->au) {
            uint64_t end = sector + n;
            uint64_t cut = end - end % v->au;
            if (cut > sector) n = (uint32_t)(cut - sector);
        }
        if (d->yield) d->yield(d->context);
        unsigned phase = FWR_PHASE_DATA;
        if (!is_data) {
            if (sector >= v->fat1 && sector < v->fat1 + v->fat_sectors)
                phase = v->flag_write == 1 ? FWR_PHASE_DIRTY1 : v->flag_write == 2 ? FWR_PHASE_CLEAN1 : FWR_PHASE_FAT1;
            else if (v->fats == 2 && sector >= v->fat2 && sector < v->fat2 + v->fat_sectors)
                phase = v->flag_write == 1 ? FWR_PHASE_DIRTY2 : v->flag_write == 2 ? FWR_PHASE_CLEAN2 : FWR_PHASE_FAT2;
            else phase = sector == v->fsinfo ? FWR_PHASE_FSINFO : FWR_PHASE_DIRECTORY;
        }
        int ok = d->write_classified ? d->write_classified(d->context, sector, n, data, phase, v->transaction_token)
                                    : d->write(d->context, sector, n, data);
        if (ok != 1) return FWR_IO;
        struct fwr_wear *w = &v->wear;
        w->writes++; w->bytes_written += (uint64_t)n * S;
        if (is_data) w->data_bytes += (uint64_t)n * S; else w->metadata_bytes += (uint64_t)n * S;
        w->size_histogram[bucket(n)]++;
        uint32_t g = v->au < v->max_write ? v->au : v->max_write;
        if (sector % g == 0 && (sector + n) % g == 0) w->au_aligned_writes++; else w->unaligned_writes++;
        sector += n; count -= n; data += (size_t)n * S;
    }
    return FWR_OK;
}

/* ---------------------------------------------------------------- FAT window */

/* Dirty sectors go to FAT 1 and then FAT 2, or the other way round when reverse is set. */
static int fat_flush_order(struct fwr_volume *v, int reverse)
{
    if (v->window_first == UINT32_MAX) return FWR_OK;
    for (unsigned step = 0; step < v->fats; ++step) {
        unsigned copy = reverse ? v->fats - 1 - step : step;
        uint64_t base = copy ? v->fat2 : v->fat1;
        for (uint32_t i = 0; i < v->window_valid_sectors;) {
            if (!(v->window_dirty[i / 8] >> (i % 8) & 1)) { ++i; continue; }
            uint32_t j = i;
            while (j < v->window_valid_sectors && (v->window_dirty[j / 8] >> (j % 8) & 1)) ++j;
            int r = dev_write(v, base + v->window_first + i, j - i, v->cfg.fat_window + (size_t)i * S, 0);
            if (r) return r;
            i = j;
        }
    }
    memset(v->window_dirty, 0, sizeof v->window_dirty);
    return FWR_OK;
}

static int fat_flush(struct fwr_volume *v) { return fat_flush_order(v, 0); }

static int window_for(struct fwr_volume *v, uint32_t sector_index)
{
    if (v->window_first != UINT32_MAX && sector_index >= v->window_first &&
        sector_index < v->window_first + v->window_valid_sectors) return FWR_OK;
    int r = fat_flush(v);
    if (r) return r;
    uint32_t first = sector_index - sector_index % FWR_FAT_WINDOW_SECTORS;
    uint32_t n = v->fat_sectors - first < FWR_FAT_WINDOW_SECTORS ? v->fat_sectors - first : FWR_FAT_WINDOW_SECTORS;
    v->window_first = UINT32_MAX;
    r = dev_read(v, v->fat1 + first, n, v->cfg.fat_window);
    if (r) return r;
    v->window_first = first; v->window_valid_sectors = n;
    return FWR_OK;
}

static int fat_get_raw(struct fwr_volume *v, uint32_t c, uint32_t *out)
{
    if (c >= (uint64_t)v->fat_sectors * (S / 4)) return FWR_BAD_VOLUME;
    int r = window_for(v, c / (S / 4));
    if (r) return r;
    *out = le32(v->cfg.fat_window + ((c / (S / 4)) - v->window_first) * S + c % (S / 4) * 4);
    return FWR_OK;
}

static int fat_get(struct fwr_volume *v, uint32_t c, uint32_t *out)
{
    uint32_t raw;
    int r = fat_get_raw(v, c, &raw);
    if (!r) *out = raw & MASK;
    return r;
}

static int fat_set_raw(struct fwr_volume *v, uint32_t c, uint32_t value)
{
    if (c >= (uint64_t)v->fat_sectors * (S / 4)) return FWR_BAD_VOLUME;
    int r = window_for(v, c / (S / 4));
    if (r) return r;
    uint32_t i = c / (S / 4) - v->window_first;
    put32(v->cfg.fat_window + (size_t)i * S + c % (S / 4) * 4, value);
    v->window_dirty[i / 8] |= (uint8_t)(1u << (i % 8));
    return FWR_OK;
}

/* The top four bits of an entry are preserved. */
static int fat_set(struct fwr_volume *v, uint32_t c, uint32_t value)
{
    uint32_t raw;
    int r = fat_get_raw(v, c, &raw);
    if (r) return r;
    return fat_set_raw(v, c, (raw & ~MASK) | (value & MASK));
}

/* FAT[1] bit 27 clear means the volume was not cleanly unmounted (FAT32 specification, FAT[1]). */
static int set_flag(struct fwr_volume *v, int clean)
{
    uint32_t raw;
    int r = fat_get_raw(v, 1, &raw);
    if (r) return r;
    raw = clean ? raw | CLEAN_BIT : raw & ~CLEAN_BIT;
    r = fat_set_raw(v, 1, raw);
    if (r) return r;
    /* Mount reads the flag from FAT 1: set it there first, clear it there last. */
    v->flag_write = clean ? 2u : 1u;
    r = fat_flush_order(v, clean);
    v->flag_write = 0;
    if (r) return r;
    v->dirty_set = !clean;
    return FWR_OK;
}

static int begin_meta(struct fwr_volume *v)
{
    if (v->dirty_set || v->in_recovery) return FWR_OK;
    return set_flag(v, 0);
}

/* ---------------------------------------------------------------- mount */

static int fsinfo_write(struct fwr_volume *v)
{
    if (!v->fsinfo_valid) return FWR_OK;
    uint8_t *b = v->dir_sector;                 /* scratch; the directory cache is invalidated */
    v->dir_cached = UINT64_MAX;
    int r = dev_read(v, v->fsinfo, 1, b);
    if (r) return r;
    if (le32(b) != 0x41615252 || le32(b + 484) != 0x61417272) return FWR_BAD_VOLUME;
    if (le32(b + 488) == v->free_clusters && le32(b + 492) == v->next_hint) return FWR_OK;
    put32(b + 488, v->free_clusters); put32(b + 492, v->next_hint);
    return dev_write(v, v->fsinfo, 1, b, 0);
}

static int recover(struct fwr_volume *v);

static int mount_transaction(struct fwr_volume *v, const struct fwr_config *cfg)
{
    if (!v || !cfg || !cfg->device.read || !cfg->device.write || !cfg->cluster_buffer || !cfg->fat_window) return FWR_INVALID;
    v->window_first = UINT32_MAX; v->dir_cached = UINT64_MAX;
    v->free_clusters = v->next_hint = UINT32_MAX;
    v->au = cfg->device.au_sectors ? cfg->device.au_sectors : FWR_DEFAULT_AU_SECTORS;
    v->max_write = cfg->device.max_write_sectors ? cfg->device.max_write_sectors : 256u;
    if (!cfg->device.sectors || cfg->base > cfg->device.sectors || cfg->sectors > cfg->device.sectors - cfg->base || cfg->sectors < 2) return FWR_INVALID;
    v->total_end = cfg->base + cfg->sectors;
    uint8_t *b = v->dir_sector;
    int r = dev_read(v, cfg->base, 1, b);
    if (r) return r;
    if (b[510] != 0x55 || b[511] != 0xaa) return FWR_BAD_VOLUME;
    uint32_t bytes = le16(b + 11), spc = b[13], reserved = le16(b + 14), fats = b[16];
    if (bytes != S) return FWR_UNSUPPORTED;
    if (!spc || (spc & (spc - 1)) || spc > 128 || reserved < 2 || fats < 1 || fats > 2) return FWR_BAD_VOLUME;
    if (le16(b + 17) || le16(b + 19) || le16(b + 22)) return FWR_UNSUPPORTED;
    uint32_t total = le32(b + 32), fsz = le32(b + 36), root = le32(b + 44);
    if (!total || !fsz || le16(b + 42) || total > cfg->sectors) return FWR_BAD_VOLUME;
    uint64_t data = (uint64_t)reserved + (uint64_t)fats * fsz;
    if (data >= total) return FWR_BAD_VOLUME;
    uint32_t clusters = (uint32_t)(((uint64_t)total - data) / spc);
    if (clusters < 65525) return FWR_UNSUPPORTED;
    if (clusters > BAD - 3 || (uint64_t)fsz * (S / 4) < (uint64_t)clusters + 2) return FWR_BAD_VOLUME;
    if (root < 2 || root >= clusters + 2) return FWR_BAD_VOLUME;
    if (spc * S > cfg->cluster_buffer_bytes) return FWR_INVALID;
    v->spc = spc; v->csize = spc * S; v->reserved = reserved; v->fats = (uint8_t)fats; v->fat_sectors = fsz;
    v->root = root; v->clusters = clusters;
    v->fat1 = cfg->base + reserved; v->fat2 = v->fat1 + fsz; v->data_start = cfg->base + data;
    uint32_t info = le16(b + 48);
    if (info && info != 0xffff && info < reserved) {
        uint8_t save[S];
        memcpy(save, b, S);
        if (dev_read(v, cfg->base + info, 1, b)) return FWR_IO;
        if (le32(b) == 0x41615252 && le32(b + 484) == 0x61417272 && le32(b + 508) == 0xaa550000) {
            v->fsinfo_valid = 1; v->fsinfo = cfg->base + info;
            uint32_t fr = le32(b + 488), nx = le32(b + 492);
            if (fr <= clusters) v->free_clusters = fr;
            if (nx >= 2 && nx < clusters + 2) v->next_hint = nx;
        }
        memcpy(b, save, S);
    }
    uint32_t f0, f1;
    if ((r = fat_get_raw(v, 0, &f0)) || (r = fat_get_raw(v, 1, &f1))) return r;
    if ((f0 & 0xff) != b[21] || (f1 & MASK) < (EOC_MIN & ~CLEAN_BIT)) return FWR_BAD_VOLUME;
    if (!(f1 & CLEAN_BIT)) {
        v->dirty_set = 1;
        if (!cfg->recovery_bitmap) return FWR_NEEDS_RECOVERY;
        r = recover(v);
        if (r) return r;
    }
    return FWR_OK;
}

static int file_quiescent(const struct fwr_file *f)
{
    if (f->failed) return 0;
    if (f->size != f->synced_size) return 0;
    if (f->chained && (!f->has_entry || f->replace_pending)) return 0;
    return 1;
}

static int checkpoint_transaction(struct fwr_volume *v)
{
    int r = fat_flush(v);
    if (r) return r;
    if (v->free_clusters != UINT32_MAX) { r = fsinfo_write(v); if (r) return r; }
    if (!v->dirty_set) return FWR_OK;
    for (struct fwr_file *f = v->files; f; f = f->next) if (!file_quiescent(f)) return FWR_OK;
    return set_flag(v, 1);
}

int fwr_unmount(struct fwr_volume *v)
{
    if (!v) return FWR_INVALID;
    if (v->files) return FWR_BUSY;
    return fwr_checkpoint(v);
}

const struct fwr_wear *fwr_wear(const struct fwr_volume *v) { return &v->wear; }

/* ---------------------------------------------------------------- directories */

struct fwr_diter {
    uint32_t first, cluster;       /* cluster 0: finished */
    uint32_t index;                /* entry ordinal within the cluster */
    uint32_t clusters;
};

static int dir_load(struct fwr_volume *v, uint64_t sector)
{
    if (v->dir_cached == sector) return FWR_OK;
    v->dir_cached = UINT64_MAX;
    int r = dev_read(v, sector, 1, v->dir_sector);
    if (!r) v->dir_cached = sector;
    return r;
}

static int dir_store(struct fwr_volume *v, uint64_t sector)
{
    int r = begin_meta(v);
    if (r) return r;
    ++v->dir_generation;
    r = dev_write(v, sector, 1, v->dir_sector, 0);
    if (r) v->dir_cached = UINT64_MAX; else v->dir_cached = sector;
    return r;
}

static uint8_t sfn_checksum(const uint8_t *n)
{
    uint8_t sum = 0;
    for (unsigned i = 0; i < 11; ++i) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + n[i]);
    return sum;
}

static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }

static int name_equal(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) if (upper(*a) != upper(*b)) return 0;
    return !*a && !*b;
}

/* The entry at position `index` of a directory sector run, in directory order. */
struct scan {
    const char *name;              /* match by long or short name */
    const uint8_t *sfn;            /* match by 11-byte short name */
    unsigned need;                 /* free slots wanted in one sector; 0: none */
    int found;
    struct fwr_loc loc;
    int plan;                      /* 0 none, 1 in place, 2 next sector, 3 new cluster */
    struct fwr_slot slot;
    uint64_t tomb_sector; uint8_t tomb_from;   /* entries tomb_from..31 of tomb_sector become deleted */
    uint32_t last_cluster;
    unsigned count;                /* live entries seen (for the empty-directory check) */
};

static void short_display(const uint8_t *e, char *out)
{
    unsigned n = 0, lower_base = e[12] & 0x08, lower_ext = e[12] & 0x10;
    for (unsigned i = 0; i < 8 && e[i] != ' '; ++i) { char c = (char)e[i]; out[n++] = lower_base && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (unsigned i = 8; i < 11 && e[i] != ' '; ++i) { char c = (char)e[i]; out[n++] = lower_ext && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
    }
    out[n] = 0;
}

static int scan_dir(struct fwr_volume *v, uint32_t dir, struct scan *sc)
{
    uint16_t chars[20 * 13];
    unsigned lfn_expected = 0, lfn_total = 0; uint8_t lfn_sum = 0;
    struct fwr_slot lfn_pos[20]; unsigned lfn_n = 0;
    uint32_t cluster = dir, walked = 0;
    sc->found = 0; sc->plan = 0; sc->count = 0; sc->last_cluster = dir; sc->tomb_from = ENTRIES;
    while (cluster) {
        sc->last_cluster = cluster;
        for (uint32_t sec = 0; sec < v->spc; ++sec) {
            uint64_t abs = cluster_sector(v, cluster) + sec;
            int r = dir_load(v, abs);
            if (r) return r;
            unsigned run = 0;
            for (unsigned i = 0; i < ENTRIES; ++i) {
                const uint8_t *e = v->dir_sector + 32 * i;
                if (e[0] == 0x00) {
                    /* End of the directory: every later slot is free. */
                    if (sc->need && !sc->plan) {
                        unsigned room = ENTRIES - i;
                        if (room >= sc->need) { sc->plan = 1; sc->slot = (struct fwr_slot){abs, (uint8_t)i}; }
                        else {
                            sc->tomb_sector = abs; sc->tomb_from = (uint8_t)i;
                            if (sec + 1 < v->spc) { sc->plan = 2; sc->slot = (struct fwr_slot){abs + 1, 0}; }
                            else sc->plan = 3;
                        }
                    }
                    return FWR_OK;
                }
                if (e[0] == 0xe5) {
                    ++run; lfn_expected = 0; lfn_total = 0; lfn_n = 0;
                    if (sc->need && !sc->plan && run >= sc->need) {
                        sc->plan = 1; sc->slot = (struct fwr_slot){abs, (uint8_t)(i + 1 - run)};
                    }
                    continue;
                }
                run = 0;
                uint8_t attr = e[11];
                if ((attr & 0x3f) == 0x0f) {
                    unsigned ord = e[0], seq = ord & 0x3f;
                    if (ord & 0x40) { lfn_total = seq; lfn_expected = seq; lfn_sum = e[13]; lfn_n = 0; }
                    if (!lfn_expected || seq != lfn_expected || e[13] != lfn_sum || seq < 1 || seq > 20) { lfn_expected = 0; continue; }
                    static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                    for (unsigned k = 0; k < 13; ++k) chars[(seq - 1) * 13 + k] = (uint16_t)le16(e + pos[k]);
                    lfn_pos[lfn_n++] = (struct fwr_slot){abs, (uint8_t)i};
                    --lfn_expected;
                    continue;
                }
                if (attr & 0x08) { lfn_expected = 0; continue; }          /* volume label */
                if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) { lfn_expected = 0; continue; }
                ++sc->count;
                int long_ok = lfn_total && lfn_expected == 0 && lfn_n == lfn_total && lfn_sum == sfn_checksum(e);
                int match = 0;
                if (sc->sfn) match = memcmp(sc->sfn, e, 11) == 0;
                else if (sc->name) {
                    char shown[16], longname[FWR_NAME_MAX + 1];
                    short_display(e, shown);
                    match = name_equal(sc->name, shown);
                    if (!match && long_ok) {
                        unsigned n = 0;
                        for (unsigned k = 0; k < lfn_total * 13u && n < FWR_NAME_MAX; ++k) {
                            uint16_t ch = chars[k];
                            if (ch == 0) break;
                            longname[n++] = ch < 0x80 ? (char)ch : '?';
                        }
                        longname[n] = 0;
                        match = name_equal(sc->name, longname);
                    }
                }
                if (match) {
                    sc->found = 1;
                    sc->loc.sfn = (struct fwr_slot){abs, (uint8_t)i};
                    memcpy(sc->loc.entry, e, 32);
                    sc->loc.directory = dir;
                    sc->loc.lfn_count = 0;
                    if (long_ok) {
                        sc->loc.lfn_count = (uint8_t)lfn_n;
                        for (unsigned k = 0; k < lfn_n; ++k) sc->loc.lfn[k] = lfn_pos[k];
                    }
                    return FWR_OK;
                }
                lfn_expected = 0; lfn_total = 0; lfn_n = 0;
            }
        }
        uint32_t next;
        int r = fat_get(v, cluster, &next);
        if (r) return r;
        if (next >= EOC_MIN) { cluster = 0; break; }
        if (next < 2 || next == BAD || next >= v->clusters + 2 || ++walked >= DIR_MAX_CLUSTERS) return FWR_CORRUPT;
        cluster = next;
    }
    if (sc->need && !sc->plan) sc->plan = 3;
    return FWR_OK;
}

/* ---------------------------------------------------------------- names */

static int name_valid(const char *n)
{
    size_t len = strlen(n);
    if (!len || len > FWR_NAME_MAX || n[len - 1] == ' ' || n[len - 1] == '.' || n[0] == ' ') return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)n[i];
        if (c < 0x20 || c > 0x7e || strchr("\"*/:<>?\\|", c)) return 0;
    }
    return 1;
}

static int short_char(char c)
{ return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr("!#$%&'()-@^_`{}~", c)); }

/* 1 when the name is an 8.3 name that needs no long name entries; fills the 11 bytes and case flags. */
static int make_short(const char *name, uint8_t sfn[11], uint8_t *nt)
{
    const char *dot = strchr(name, '.');
    if (dot && strchr(dot + 1, '.')) return 0;
    size_t stem = dot ? (size_t)(dot - name) : strlen(name), ext = dot ? strlen(dot + 1) : 0;
    if (!stem || stem > 8 || ext > 3 || (dot && !ext)) return 0;
    int lo_stem = 0, up_stem = 0, lo_ext = 0, up_ext = 0;
    memset(sfn, ' ', 11);
    for (size_t i = 0; i < stem + ext + (dot ? 1u : 0u); ++i) {
        if (dot && i == stem) continue;
        char c = name[i];
        int in_ext = dot && i > stem;
        if (c >= 'a' && c <= 'z') { if (in_ext) lo_ext = 1; else lo_stem = 1; c = upper(c); }
        else if (c >= 'A' && c <= 'Z') { if (in_ext) up_ext = 1; else up_stem = 1; }
        if (!short_char(c)) return 0;
        if (in_ext) sfn[8 + (i - stem - 1)] = (uint8_t)c; else sfn[i] = (uint8_t)c;
    }
    if ((lo_stem && up_stem) || (lo_ext && up_ext)) return 0;
    *nt = (uint8_t)((lo_stem ? 0x08 : 0) | (lo_ext ? 0x10 : 0));
    return 1;
}

static void make_basis(const char *name, uint8_t sfn[11])
{
    const char *dot = NULL;                                   /* the last dot; no libc call, so the freestanding boards build it */
    for (const char *q = name; *q; ++q) if (*q == '.') dot = q;
    while (*name == '.' || *name == ' ') ++name;
    if (dot && dot < name) dot = NULL;
    size_t stem_len = dot ? (size_t)(dot - name) : strlen(name);
    memset(sfn, ' ', 11);
    unsigned n = 0;
    for (size_t i = 0; i < stem_len && n < 6; ++i) {
        char c = upper(name[i]);
        if (c == ' ' || c == '.') continue;
        sfn[n++] = (uint8_t)(short_char(c) ? c : '_');
    }
    if (!n) sfn[n++] = '_';
    n = 0;
    if (dot) for (const char *p = dot + 1; *p && n < 3; ++p) {
        char c = upper(*p);
        if (c == ' ') continue;
        sfn[8 + n++] = (uint8_t)(short_char(c) ? c : '_');
    }
}

static void stamp(const struct fwr_volume *v, uint32_t *date, uint32_t *time)
{
    uint32_t t = v->cfg.now ? v->cfg.now(v->cfg.now_context) : 0x00210000u;
    *date = t >> 16; *time = t & 0xffff;
}

struct newent { uint8_t e[21][32]; unsigned n; };

static void build_entries(const struct fwr_volume *v, const char *name, const uint8_t sfn[11], uint8_t nt, int lfn,
                          uint8_t attr, uint32_t first, uint32_t size, struct newent *out)
{
    memset(out, 0, sizeof *out);
    uint32_t date, time;
    stamp(v, &date, &time);
    unsigned n = 0;
    if (lfn) {
        size_t len = strlen(name);
        unsigned count = (unsigned)((len + 12) / 13);
        uint8_t sum = sfn_checksum(sfn);
        static const uint8_t pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
        for (unsigned seq = count; seq >= 1; --seq) {
            uint8_t *e = out->e[n++];
            e[0] = (uint8_t)(seq | (seq == count ? 0x40 : 0)); e[11] = 0x0f; e[13] = sum;
            for (unsigned k = 0; k < 13; ++k) {
                size_t idx = (size_t)(seq - 1) * 13 + k;
                uint16_t ch = idx < len ? (uint8_t)name[idx] : idx == len ? 0x0000 : 0xffff;
                put16(e + pos[k], ch);
            }
        }
    }
    uint8_t *e = out->e[n++];
    memcpy(e, sfn, 11); e[11] = attr; e[12] = nt;
    put16(e + 14, time); put16(e + 16, date); put16(e + 18, date); put16(e + 22, time); put16(e + 24, date);
    put16(e + 20, first >> 16); put16(e + 26, first); put32(e + 28, size);
    out->n = n;
}

/* ---------------------------------------------------------------- allocation */

static int is_reserved(const struct fwr_volume *v, uint32_t c)
{
    for (const struct fwr_file *f = v->files; f; f = f->next)
        for (unsigned i = 0; i < f->run_count; ++i)
            if (c >= f->runs[i].start && c - f->runs[i].start < f->runs[i].length) return 1;
    return 0;
}

/* A run for `want` clusters. With align set, the run starts at a cluster whose first sector is a
 * multiple of the allocation unit when the volume has such a run; otherwise any run. */
static int find_free_run_once(struct fwr_volume *v, uint32_t want, int align, struct fwr_run *out)
{
    uint32_t hint = v->next_hint >= 2 && v->next_hint < v->clusters + 2 ? v->next_hint : 2;
    uint32_t run_start = 0, run_len = 0, best_start = 0, best_len = 0;
    for (uint32_t k = 0; k < v->clusters; ++k) {
        uint32_t c = 2 + (hint - 2 + k) % v->clusters, val;
        if (c == 2) run_len = 0;
        int r = fat_get(v, c, &val);
        if (r) return r;
        if (val == 0 && !is_reserved(v, c) && (run_len || !align || cluster_sector(v, c) % v->au == 0)) {
            if (!run_len) run_start = c;
            if (++run_len > best_len) { best_len = run_len; best_start = run_start; }
            if (run_len >= want) break;
        } else run_len = 0;
    }
    if (!best_len) return FWR_FULL;
    out->start = best_start; out->length = best_len < want ? best_len : want;
    return FWR_OK;
}

static int find_free_run(struct fwr_volume *v, uint32_t want, int align, struct fwr_run *out)
{
    if (align && (uint64_t)want * v->csize >= (uint64_t)v->au * S) {
        int r = find_free_run_once(v, want, 1, out);
        if (r == FWR_OK && out->length == want) return r;
        if (r != FWR_OK && r != FWR_FULL) return r;
    }
    return find_free_run_once(v, want, 0, out);
}

static int alloc_one(struct fwr_volume *v, uint32_t *c)
{
    struct fwr_run r;
    int rc = find_free_run(v, 1, 0, &r);
    if (!rc) *c = r.start;
    return rc;
}

static int reserve(struct fwr_file *f, uint32_t count, uint32_t *got_out)
{
    struct fwr_volume *v = f->volume;
    uint32_t got = 0;
    while (got < count && f->run_count < FWR_MAX_RUNS) {
        struct fwr_run r = {0, 0};
        uint32_t last = f->run_count ? f->runs[f->run_count - 1].start + f->runs[f->run_count - 1].length - 1 : f->chained ? f->tail : 0;
        if (last && last + 1 < v->clusters + 2) {
            uint32_t l = 0, val;
            while (got + l < count && last + 1 + l < v->clusters + 2) {
                int rc = fat_get(v, last + 1 + l, &val);
                if (rc) return rc;
                if (val != 0 || is_reserved(v, last + 1 + l)) break;
                ++l;
            }
            if (l) r = (struct fwr_run){last + 1, l};
        }
        if (!r.length) {
            int rc = find_free_run(v, count - got, 1, &r);
            if (rc == FWR_FULL) break;
            if (rc) return rc;
        }
        got += r.length;
        v->next_hint = r.start + r.length < v->clusters + 2 ? r.start + r.length : 2;
        if (f->run_count && f->runs[f->run_count - 1].start + f->runs[f->run_count - 1].length == r.start)
            f->runs[f->run_count - 1].length += r.length;
        else f->runs[f->run_count++] = r;
    }
    *got_out = got;
    return FWR_OK;
}

static uint64_t reserved_clusters(const struct fwr_file *f)
{
    uint64_t n = f->chained;
    for (unsigned i = 0; i < f->run_count; ++i) n += f->runs[i].length;
    return n;
}

static int ensure_reserved(struct fwr_file *f, uint64_t total)
{
    uint64_t have = reserved_clusters(f);
    if (have >= total) return FWR_OK;
    struct fwr_volume *v = f->volume;
    uint64_t grow = (v->cfg.grow_bytes ? v->cfg.grow_bytes : (uint64_t)16 << 20) / v->csize;
    uint64_t want = total - have;
    if (want < grow) want = grow;
    if (want > 0x0fffffffu) want = 0x0fffffffu;
    uint32_t got = 0;
    int r = reserve(f, (uint32_t)want, &got);
    if (r) return r;
    return reserved_clusters(f) >= total ? FWR_OK : FWR_FULL;
}

static int contig_from(const struct fwr_file *f, uint64_t L, uint32_t *c, uint32_t *n)
{
    if (L < f->chained) {
        if (L != f->chained - 1) return FWR_INVALID;
        *c = f->tail; *n = 1;
        if (f->run_count && f->runs[0].start == f->tail + 1) *n += f->runs[0].length;
        return FWR_OK;
    }
    uint64_t idx = L - f->chained;
    for (unsigned i = 0; i < f->run_count; ++i) {
        if (idx < f->runs[i].length) { *c = f->runs[i].start + (uint32_t)idx; *n = f->runs[i].length - (uint32_t)idx; return FWR_OK; }
        idx -= f->runs[i].length;
    }
    return FWR_FULL;
}

static int free_chain(struct fwr_volume *v, uint32_t first)
{
    uint32_t c = first, count = 0;
    while (c >= 2 && c < v->clusters + 2) {
        uint32_t next;
        int r = fat_get(v, c, &next);
        if (r) return r;
        if (next == 0 || next == BAD) { next = EOC; }
        r = fat_set(v, c, 0);
        if (r) return r;
        ++count;
        if (next >= EOC_MIN || count > v->clusters) break;
        c = next;
    }
    int r = fat_flush(v);
    if (!r && v->free_clusters != UINT32_MAX) v->free_clusters += count;
    return r;
}

/* ---------------------------------------------------------------- paths and entries */

static uint32_t entry_cluster(const uint8_t *e) { return le16(e + 20) << 16 | le16(e + 26); }

static int resolve(struct fwr_volume *v, const char *path, uint32_t *dir, char leaf[FWR_NAME_MAX + 1])
{
    if (!path) return FWR_INVALID;
    uint32_t d = v->root;
    while (*path == '/') ++path;
    for (;;) {
        const char *end = path;
        while (*end && *end != '/') ++end;
        size_t len = (size_t)(end - path);
        if (!len || len > FWR_NAME_MAX) return FWR_INVALID;
        char part[FWR_NAME_MAX + 1];
        memcpy(part, path, len); part[len] = 0;
        const char *rest = end;
        while (*rest == '/') ++rest;
        if (!*rest) {
            if (*end == '/' || !name_valid(part)) return FWR_INVALID;
            memcpy(leaf, part, len + 1);
            *dir = d;
            return FWR_OK;
        }
        struct scan sc = {.name = part};
        int r = scan_dir(v, d, &sc);
        if (r) return r;
        if (!sc.found || !(sc.loc.entry[11] & 0x10)) return FWR_NOT_FOUND;
        d = entry_cluster(sc.loc.entry);
        if (!d) d = v->root;
        path = rest;
    }
}

static int slot_write(struct fwr_volume *v, struct scan *sc, const struct newent *ne, struct fwr_slot *sfn_slot)
{
    int r;
    if (sc->plan == 1 || sc->plan == 2) {
        if ((r = dir_load(v, sc->slot.sector))) return r;
        memcpy(v->dir_sector + 32 * sc->slot.index, ne->e, 32 * ne->n);
        if ((r = dir_store(v, sc->slot.sector))) return r;
        *sfn_slot = (struct fwr_slot){sc->slot.sector, (uint8_t)(sc->slot.index + ne->n - 1)};
    } else if (sc->plan == 3) {
        uint32_t c;
        if ((r = alloc_one(v, &c))) return r;
        memset(v->cfg.cluster_buffer, 0, v->csize);
        if ((r = dev_write(v, cluster_sector(v, c), v->spc, v->cfg.cluster_buffer, 1))) return r;
        if ((r = fat_set(v, c, EOC)) || (r = fat_flush(v))) return r;
        if ((r = fat_set(v, sc->last_cluster, c)) || (r = fat_flush(v))) return r;
        if (v->free_clusters != UINT32_MAX) --v->free_clusters;
        uint64_t abs = cluster_sector(v, c);
        memset(v->dir_sector, 0, S); v->dir_cached = abs;
        memcpy(v->dir_sector, ne->e, 32 * ne->n);
        if ((r = dir_store(v, abs))) return r;
        *sfn_slot = (struct fwr_slot){abs, (uint8_t)(ne->n - 1)};
    } else return FWR_INVALID;
    if (sc->plan >= 2 && sc->tomb_from < ENTRIES) {
        if ((r = dir_load(v, sc->tomb_sector))) return r;
        for (unsigned i = sc->tomb_from; i < ENTRIES; ++i) v->dir_sector[32 * i] = 0xe5;
        if ((r = dir_store(v, sc->tomb_sector))) return r;
    }
    return FWR_OK;
}

/* Create a directory entry for `name` in `dir`. Fails with FWR_EXISTS when the name exists. */
static int entry_create(struct fwr_volume *v, uint32_t dir, const char *name, uint8_t attr, uint32_t first,
                        uint32_t size, struct fwr_slot *sfn_slot)
{
    uint8_t sfn[11], nt = 0;
    int lfn = !make_short(name, sfn, &nt);
    unsigned need = 1;
    struct scan sc;
    int r;
    if (lfn) {
        need = (unsigned)((strlen(name) + 12) / 13) + 1;
        make_basis(name, sfn);
        int ok = 0;
        for (unsigned tail = 1; tail < 1000000 && !ok; ++tail) {
            uint8_t cand[11]; memcpy(cand, sfn, 11);
            char digits[8]; unsigned nd = 0;
            for (unsigned t = tail; t; t /= 10) digits[nd++] = (char)('0' + t % 10);
            unsigned stem = 0; while (stem < 8 && cand[stem] != ' ') ++stem;
            if (stem + 1 + nd > 8) stem = 8 - 1 - nd;
            memset(cand + stem, ' ', 8 - stem);
            cand[stem] = '~';
            for (unsigned k = 0; k < nd; ++k) cand[stem + 1 + k] = (uint8_t)digits[nd - 1 - k];
            sc = (struct scan){.sfn = cand};
            if ((r = scan_dir(v, dir, &sc))) return r;
            if (!sc.found) { memcpy(sfn, cand, 11); ok = 1; }
        }
        if (!ok) return FWR_FULL;
    }
    sc = (struct scan){.name = name, .need = need};
    if ((r = scan_dir(v, dir, &sc))) return r;
    if (sc.found) return FWR_EXISTS;
    struct newent ne;
    build_entries(v, name, sfn, nt, lfn, attr, first, size, &ne);
    if ((r = begin_meta(v))) return r;
    return slot_write(v, &sc, &ne, sfn_slot);
}

static int entry_update(struct fwr_volume *v, const struct fwr_slot *slot, uint32_t first, uint32_t size)
{
    int r = dir_load(v, slot->sector);
    if (r) return r;
    uint8_t *e = v->dir_sector + 32 * slot->index;
    if (e[0] == 0xe5 || e[0] == 0x00) return FWR_CORRUPT;
    uint32_t date, time;
    stamp(v, &date, &time);
    put16(e + 20, first >> 16); put16(e + 26, first); put32(e + 28, size);
    put16(e + 22, time); put16(e + 24, date); put16(e + 18, date);
    e[11] |= 0x20;
    e[12] &= (uint8_t)~0x80;
    return dir_store(v, slot->sector);
}

/* Delete an entry set: long entries first, the short entry last, the short entry's sector in one write. */
static int entry_delete(struct fwr_volume *v, const struct fwr_loc *loc)
{
    int r = begin_meta(v);
    if (r) return r;
    for (unsigned i = 0; i < loc->lfn_count; ++i) {
        if (loc->lfn[i].sector == loc->sfn.sector) continue;
        int seen = 0;
        for (unsigned j = 0; j < i; ++j) if (loc->lfn[j].sector == loc->lfn[i].sector) seen = 1;
        if (seen) continue;
        if ((r = dir_load(v, loc->lfn[i].sector))) return r;
        for (unsigned j = 0; j < loc->lfn_count; ++j)
            if (loc->lfn[j].sector == loc->lfn[i].sector) v->dir_sector[32 * loc->lfn[j].index] = 0xe5;
        if ((r = dir_store(v, loc->lfn[i].sector))) return r;
    }
    if ((r = dir_load(v, loc->sfn.sector))) return r;
    for (unsigned j = 0; j < loc->lfn_count; ++j)
        if (loc->lfn[j].sector == loc->sfn.sector) v->dir_sector[32 * loc->lfn[j].index] = 0xe5;
    v->dir_sector[32 * loc->sfn.index] = 0xe5;
    return dir_store(v, loc->sfn.sector);
}

int fwr_exists(struct fwr_volume *v, const char *path, uint64_t *size, int *is_dir)
{
    uint32_t dir; char leaf[FWR_NAME_MAX + 1];
    int r = resolve(v, path, &dir, leaf);
    if (r) return r;
    struct scan sc = {.name = leaf};
    if ((r = scan_dir(v, dir, &sc))) return r;
    if (!sc.found) return FWR_NOT_FOUND;
    if (size) *size = le32(sc.loc.entry + 28);
    if (is_dir) *is_dir = (sc.loc.entry[11] & 0x10) != 0;
    return FWR_OK;
}

static struct fwr_file *open_file_at(struct fwr_volume *v, const struct fwr_slot *s)
{
    for (struct fwr_file *f = v->files; f; f = f->next)
        if (f->has_entry && f->loc.sfn.sector == s->sector && f->loc.sfn.index == s->index) return f;
    return NULL;
}

static int mkdir_transaction(struct fwr_volume *v, const char *path)
{
    uint32_t dir; char leaf[FWR_NAME_MAX + 1];
    int r = resolve(v, path, &dir, leaf);
    if (r) return r;
    struct scan sc = {.name = leaf};
    if ((r = scan_dir(v, dir, &sc))) return r;
    if (sc.found) return FWR_EXISTS;
    if ((r = begin_meta(v))) return r;
    uint32_t c;
    if ((r = alloc_one(v, &c))) return r;
    uint32_t date, time;
    stamp(v, &date, &time);
    memset(v->cfg.cluster_buffer, 0, v->csize);
    uint8_t *e = v->cfg.cluster_buffer;
    for (unsigned i = 0; i < 2; ++i, e += 32) {
        memset(e, ' ', 11); e[0] = '.'; if (i) e[1] = '.';
        e[11] = 0x10;
        uint32_t target = i ? (dir == v->root ? 0 : dir) : c;
        put16(e + 14, time); put16(e + 16, date); put16(e + 18, date); put16(e + 22, time); put16(e + 24, date);
        put16(e + 20, target >> 16); put16(e + 26, target);
    }
    /* Order: directory cluster, FAT entry, parent entry. */
    if ((r = dev_write(v, cluster_sector(v, c), v->spc, v->cfg.cluster_buffer, 1))) return r;
    if ((r = fat_set(v, c, EOC)) || (r = fat_flush(v))) return r;
    if (v->free_clusters != UINT32_MAX) --v->free_clusters;
    struct fwr_slot slot;
    return entry_create(v, dir, leaf, 0x10, c, 0, &slot);
}

static int unlink_transaction(struct fwr_volume *v, const char *path)
{
    uint32_t dir; char leaf[FWR_NAME_MAX + 1];
    int r = resolve(v, path, &dir, leaf);
    if (r) return r;
    struct scan sc = {.name = leaf};
    if ((r = scan_dir(v, dir, &sc))) return r;
    if (!sc.found) return FWR_NOT_FOUND;
    if (open_file_at(v, &sc.loc.sfn)) return FWR_BUSY;
    for (struct fwr_file *f = v->files; f; f = f->next)
        if (f->replace_pending && f->directory == dir && name_equal(f->name, leaf)) return FWR_BUSY;
    uint32_t first = entry_cluster(sc.loc.entry);
    if (sc.loc.entry[11] & 0x10) {
        struct scan inner = {0};
        if (!first || (r = scan_dir(v, first, &inner))) return r ? r : FWR_CORRUPT;
        if (inner.count) return FWR_NOT_EMPTY;
    }
    if ((r = entry_delete(v, &sc.loc))) return r;
    return first ? free_chain(v, first) : FWR_OK;
}

/* ---------------------------------------------------------------- files */

static int latch(struct fwr_file *f, int r) { if (r) f->failed = 1; return r; }

static void link_file(struct fwr_file *f)
{
    f->next = f->volume->files; f->volume->files = f;
}

static void unlink_file(struct fwr_file *f)
{
    for (struct fwr_file **p = &f->volume->files; *p; p = &(*p)->next)
        if (*p == f) { *p = f->next; break; }
    f->closed = 1;
}

int fwr_open(struct fwr_volume *v, struct fwr_file *f, const char *path, unsigned mode,
             uint64_t prealloc_bytes, uint8_t *buffer, size_t capacity)
{
    if (!v || !f || !buffer || capacity < S || capacity % S) return FWR_INVALID;
    unsigned kind = mode & (FWR_O_APPEND | FWR_O_REPLACE);
    if (kind == (FWR_O_APPEND | FWR_O_REPLACE) || (!(mode & (FWR_O_CREATE | FWR_O_APPEND | FWR_O_REPLACE)))) return FWR_INVALID;
    memset(f, 0, sizeof *f);
    f->volume = v; f->mode = (uint8_t)mode; f->buffer = buffer; f->capacity = capacity;
    char leaf[FWR_NAME_MAX + 1];
    int r = resolve(v, path, &f->directory, leaf);
    if (r) return r;
    memcpy(f->name, leaf, strlen(leaf) + 1);
    struct scan sc = {.name = leaf};
    if ((r = scan_dir(v, f->directory, &sc))) return r;
    for (struct fwr_file *o = v->files; o; o = o->next)
        if (o->directory == f->directory && name_equal(o->name, leaf)) return FWR_BUSY;
    if (sc.found && (sc.loc.entry[11] & 0x18)) return FWR_INVALID;      /* directory or volume label */
    if (mode & FWR_O_REPLACE) {
        f->replace_pending = 1;
    } else if (sc.found) {
        if (!(mode & FWR_O_APPEND)) return FWR_EXISTS;
        uint32_t first = entry_cluster(sc.loc.entry), size = le32(sc.loc.entry + 28);
        uint64_t needed = ((uint64_t)size + v->csize - 1) / v->csize;
        uint32_t n = 0, tail = 0, c = first;
        while (c >= 2 && c < v->clusters + 2 && n <= needed) {
            uint32_t next;
            if ((r = fat_get(v, c, &next))) return r;
            ++n; tail = c;
            if (next >= EOC_MIN) { c = 0; break; }
            if (next < 2 || next == BAD) return FWR_CORRUPT;
            c = next;
        }
        if ((size == 0 && first) || n != needed || (first && c)) return FWR_CORRUPT;
        f->loc = sc.loc; f->has_entry = 1;
        f->first_cluster = first; f->chained = n; f->tail = tail;
        f->synced_size = f->size = f->written_to = size;
        if (size % S) {
            uint64_t sec = cluster_sector(v, tail) + (size % v->csize) / S;
            if ((r = dev_read(v, sec, 1, buffer))) return r;
        }
    } else if (!(mode & FWR_O_CREATE)) return FWR_NOT_FOUND;
    link_file(f);
    if (prealloc_bytes && (r = fwr_preallocate(f, prealloc_bytes))) { unlink_file(f); return r; }
    return FWR_OK;
}

int fwr_preallocate(struct fwr_file *f, uint64_t size)
{
    if (!f || f->closed || f->failed) return FWR_FAILED;
    struct fwr_volume *v = f->volume;
    uint64_t total = (size + v->csize - 1) / v->csize;
    uint64_t have = reserved_clusters(f);
    if (have >= total) return FWR_OK;
    uint32_t got = 0;
    int r = reserve(f, (uint32_t)(total - have), &got);
    if (r) return r;
    return reserved_clusters(f) >= total ? FWR_OK : FWR_FULL;
}

size_t fwr_write(struct fwr_file *f, const void *data, size_t n)
{
    if (!f || f->closed || f->failed || !data) return 0;
    uint64_t boff = f->written_to & ~(uint64_t)(S - 1);
    size_t used = (size_t)(f->size - boff), room = f->capacity - used;
    if (n > room) n = room;
    memcpy(f->buffer + used, data, n);
    f->size += n;
    return n;
}

/* The alignment a flush honours: the allocation unit, or when the buffer is smaller than two units
 * the largest divisor of the unit (a power of two times a small factor) that fits half the buffer. */
static uint32_t flush_unit(const struct fwr_file *f)
{
    uint32_t au = f->volume->au, half = (uint32_t)(f->capacity / 2 / S);
    if (au <= half) return au;
    uint32_t unit = au;
    while (unit > half && unit % 2 == 0) unit /= 2;
    return unit > half || !unit ? 1 : unit;
}

static int64_t flush_data(struct fwr_file *f, int final)
{
    struct fwr_volume *v = f->volume;
    uint64_t boff = f->written_to & ~(uint64_t)(S - 1);
    uint64_t end = final ? (f->size + S - 1) & ~(uint64_t)(S - 1) : f->size & ~(uint64_t)(S - 1);
    int unaligned_ok = final || (f->size - boff) >= f->capacity;
    if (final && f->size == f->written_to) return 0;
    if (end <= boff) return 0;
    if (final && end > f->size) memset(f->buffer + (f->size - boff), 0, (size_t)(end - f->size));
    int r = ensure_reserved(f, (end + v->csize - 1) / v->csize);
    if (r) return r;
    uint64_t o = boff;
    while (o < end) {
        uint32_t c, n;
        if ((r = contig_from(f, o / v->csize, &c, &n))) return r;
        uint64_t sector = cluster_sector(v, c) + (o % v->csize) / S;
        uint64_t seg = (uint64_t)n * v->csize - o % v->csize;
        if (seg > end - o) seg = end - o;
        uint64_t sectors = seg / S;
        int last = o + seg == end, stop = 0;
        if (last && !unaligned_ok) {
            uint32_t unit = flush_unit(f);
            uint64_t e = sector + sectors, cut = e - e % unit;
            if (cut <= sector) break;
            if (cut < e) { sectors = cut - sector; stop = 1; }
        }
        while (sectors) {
            uint32_t piece = sectors > 0x7fffffffu ? 0x7fffffffu : (uint32_t)sectors;
            if ((r = dev_write(v, sector, piece, f->buffer + (o - boff), 1))) return r;
            sector += piece; sectors -= piece; o += (uint64_t)piece * S;
        }
        if (stop) break;
    }
    v->wear.flushes++;
    uint64_t new_written = final ? f->size : o;
    uint64_t new_boff = new_written & ~(uint64_t)(S - 1);
    if (new_boff > boff) memmove(f->buffer, f->buffer + (new_boff - boff), (size_t)(f->size - new_boff));
    f->written_to = new_written;
    return (int64_t)(o - boff);
}

int64_t fwr_service(struct fwr_file *f, int force)
{
    if (!f || f->closed) return FWR_INVALID;
    if (f->failed) return FWR_FAILED;
    uint64_t boff = f->written_to & ~(uint64_t)(S - 1);
    uint64_t pending = f->size - boff, unit = (uint64_t)flush_unit(f) * S;
    if (!force && pending < (unit < f->capacity / 2 ? unit : f->capacity / 2)) return 0;
    int64_t r = flush_data(f, 0);
    if (r < 0) latch(f, (int)r);
    return r;
}

/* Chain the clusters that now hold data: new entries first, then the link from the old tail. */
static int chain_new(struct fwr_file *f)
{
    struct fwr_volume *v = f->volume;
    uint64_t needed = (f->size + v->csize - 1) / v->csize;
    if (needed <= f->chained) return FWR_OK;
    uint32_t k = (uint32_t)(needed - f->chained), first_new = f->runs[0].start, last = 0, left = k;
    int r = begin_meta(v);
    if (r) return r;
    for (unsigned i = 0; i < f->run_count && left; ++i) {
        uint32_t take = f->runs[i].length < left ? f->runs[i].length : left;
        for (uint32_t j = 0; j < take; ++j) {
            uint32_t c = f->runs[i].start + j, next;
            left--;
            if (j + 1 < take) next = c + 1;
            else if (!left) next = EOC;
            else next = f->runs[i + 1].start;
            if ((r = fat_set(v, c, next))) return r;
            last = c;
        }
    }
    if ((r = fat_flush(v))) return r;
    if (f->chained) {
        if ((r = fat_set(v, f->tail, first_new)) || (r = fat_flush(v))) return r;
    }
    /* Drop the consumed clusters from the reservation. */
    left = k;
    while (left) {
        if (f->runs[0].length > left) { f->runs[0].start += left; f->runs[0].length -= left; left = 0; }
        else {
            left -= f->runs[0].length;
            memmove(f->runs, f->runs + 1, (size_t)(f->run_count - 1) * sizeof f->runs[0]);
            --f->run_count;
        }
    }
    if (!f->first_cluster) f->first_cluster = first_new;
    f->tail = last; f->chained = (uint32_t)needed;
    if (v->free_clusters != UINT32_MAX) v->free_clusters -= k;
    return FWR_OK;
}

static int publish_replace(struct fwr_file *f)
{
    struct fwr_volume *v = f->volume;
    struct scan sc = {.name = f->name};
    int r = scan_dir(v, f->directory, &sc);
    if (r) return r;
    if (!sc.found) {
        if ((r = entry_create(v, f->directory, f->name, 0x20, f->first_cluster, (uint32_t)f->size, &f->loc.sfn))) return r;
    } else {
        if (sc.loc.entry[11] & 0x18) return FWR_INVALID;
        if (open_file_at(v, &sc.loc.sfn)) return FWR_BUSY;
        uint32_t old = entry_cluster(sc.loc.entry);
        if ((r = begin_meta(v))) return r;
        f->loc = sc.loc;
        if ((r = entry_update(v, &sc.loc.sfn, f->first_cluster, (uint32_t)f->size))) return r;   /* the one-sector switch */
        if (old && (r = free_chain(v, old))) return r;
    }
    f->has_entry = 1; f->replace_pending = 0; f->synced_size = f->size;
    return FWR_OK;
}

static int sync_transaction(struct fwr_file *f)
{
    if (!f || f->closed) return FWR_INVALID;
    if (f->failed) return FWR_FAILED;
    struct fwr_volume *v = f->volume;
    int r = (int)flush_data(f, 1);
    if (r < 0) return latch(f, r);
    if ((r = chain_new(f))) return latch(f, r);
    if (f->replace_pending) r = publish_replace(f);
    else if (!f->has_entry) {
        r = entry_create(v, f->directory, f->name, 0x20, f->first_cluster, (uint32_t)f->size, &f->loc.sfn);
        if (!r) { f->has_entry = 1; f->synced_size = f->size; }
    } else if (f->size != f->synced_size) {
        r = entry_update(v, &f->loc.sfn, f->first_cluster, (uint32_t)f->size);
        if (!r) f->synced_size = f->size;
    }
    if (r) return latch(f, r);
    v->wear.syncs++;
    return FWR_OK;
}

int fwr_close(struct fwr_file *f)
{
    if (!f || f->closed) return FWR_INVALID;
    int r = f->failed ? FWR_FAILED : fwr_sync(f);
    unlink_file(f);
    return r;
}

static int truncate_transaction(struct fwr_file *f, uint64_t size)
{
    if (!f || f->closed) return FWR_INVALID;
    if (f->failed) return FWR_FAILED;
    struct fwr_volume *v = f->volume;
    if (size > 0xffffffffu) return FWR_INVALID;
    int r = fwr_sync(f);
    if (r) return r;
    if (size == f->size) return FWR_OK;
    if (size > f->size) {
        static const uint8_t zeros[512];
        while (f->size < size) {
            uint64_t want = size - f->size;
            size_t n = want > sizeof zeros ? sizeof zeros : (size_t)want;
            if (!fwr_write(f, zeros, n)) { int64_t fr = flush_data(f, 0); if (fr < 0) return latch(f, (int)fr); if (!fr) return latch(f, FWR_FULL); }
        }
        return fwr_sync(f);
    }
    uint64_t keep = (size + v->csize - 1) / v->csize;
    uint32_t first = f->first_cluster, tail_after = 0, free_from = 0, c = first;
    for (uint64_t i = 0; keep && i < keep; ++i) {
        uint32_t next;
        if ((r = fat_get(v, c, &next))) return latch(f, r);
        if (i + 1 == keep) { tail_after = c; free_from = next >= EOC_MIN ? 0 : next; }
        else c = next;
    }
    uint32_t new_first = keep ? first : 0;
    if (!keep) free_from = first;
    /* Directory entry first, then the FAT: a cut between leaves a chain longer than the size. */
    if ((r = begin_meta(v))) return latch(f, r);
    if ((r = entry_update(v, &f->loc.sfn, new_first, (uint32_t)size))) return latch(f, r);
    if (keep && free_from) {
        if ((r = fat_set(v, tail_after, EOC)) || (r = fat_flush(v))) return latch(f, r);
    }
    if (free_from && (r = free_chain(v, free_from))) return latch(f, r);
    f->first_cluster = new_first; f->chained = (uint32_t)keep; f->tail = keep ? tail_after : 0;
    f->size = f->synced_size = f->written_to = size;
    f->run_count = 0;
    if (size % S) {
        uint64_t sec = cluster_sector(v, f->tail) + (size % v->csize) / S;
        if ((r = dev_read(v, sec, 1, f->buffer))) return latch(f, r);
    }
    return FWR_OK;
}

int fwr_read(struct fwr_volume *v, const char *path, uint64_t offset, void *out, size_t n, size_t *got)
{
    struct fwr_cursor cursor = {0};
    return fwr_read_cursor(v, &cursor, path, offset, out, n, got);
}

int fwr_read_cursor(struct fwr_volume *v, struct fwr_cursor *cur, const char *path, uint64_t offset, void *out, size_t n, size_t *got)
{
    int r;
    if (cur->stamp != v->dir_generation + 1) {
        uint32_t dir; char leaf[FWR_NAME_MAX + 1];
        if ((r = resolve(v, path, &dir, leaf))) return r;
        struct scan sc = {.name = leaf};
        if ((r = scan_dir(v, dir, &sc))) return r;
        if (!sc.found) return FWR_NOT_FOUND;
        cur->size = le32(sc.loc.entry + 28);
        cur->first = cur->cluster = entry_cluster(sc.loc.entry);
        cur->index = 0;
        cur->stamp = v->dir_generation + 1;
        ++v->wear.read_resolves;
    }
    *got = 0;
    if (offset >= cur->size) return FWR_OK;
    if (n > cur->size - offset) n = (size_t)(cur->size - offset);
    uint64_t want = offset / v->csize;
    uint32_t c = cur->cluster;
    uint64_t at = cur->index;
    if (want < at) { c = cur->first; at = 0; }
    for (; at < want; ++at) {
        uint32_t next;
        ++v->wear.read_chain_steps;
        if ((r = fat_get(v, c, &next))) return r;
        if (next >= EOC_MIN || next < 2 || next == BAD) return FWR_CORRUPT;
        c = next;
    }
    cur->cluster = c; cur->index = at;       /* a failed read below leaves the cursor at a valid cluster of this file */
    uint8_t *dst = out;
    uint64_t pos = offset;
    while (n) {
        uint32_t in = (uint32_t)(pos % v->csize);
        uint32_t lead = in % S;
        /* The run of contiguous clusters from c that this request reaches: one card read for all of them (the device splits it into pieces
         * as for writes), not one per cluster. An unaligned start or length reads through the cluster buffer, one cluster at a time. */
        uint64_t avail = v->csize - in;
        uint32_t last = c;
        if (!lead && n % S == 0) {
            while (avail < n) {
                uint32_t next;
                ++v->wear.read_chain_steps;
                if ((r = fat_get(v, last, &next))) return r;
                if (next != last + 1) break;
                last = next; avail += v->csize;
            }
        }
        size_t chunk = avail > n ? n : (size_t)avail;
        uint64_t sector = cluster_sector(v, c) + in / S;
        uint32_t sectors = (lead + (uint32_t)chunk + S - 1) / S;
        if (lead || chunk % S) {
            /* Unaligned edge: read through the cluster buffer. */
            if ((r = dev_read(v, sector, sectors, v->cfg.cluster_buffer))) return r;
            memcpy(dst, v->cfg.cluster_buffer + lead, chunk);
        } else if ((r = dev_read(v, sector, sectors, dst))) return r;
        dst += chunk; pos += chunk; n -= chunk; *got += chunk;
        /* The cluster that holds the last byte read is the last of the run. */
        cur->cluster = last; cur->index = (pos - 1) / v->csize;
        if (n && pos % v->csize == 0) {
            uint32_t next;
            ++v->wear.read_chain_steps;
            if ((r = fat_get(v, last, &next))) return r;
            if (next >= EOC_MIN || next < 2 || next == BAD) return FWR_CORRUPT;
            c = next; at = pos / v->csize;
        }
    }
    return FWR_OK;
}

/* ---------------------------------------------------------------- rename */

static int rename_transaction(struct fwr_volume *v, const char *from, const char *to)
{
    uint32_t d1, d2; char l1[FWR_NAME_MAX + 1], l2[FWR_NAME_MAX + 1];
    int r = resolve(v, from, &d1, l1);
    if (!r) r = resolve(v, to, &d2, l2);
    if (r) return r;
    if (d1 != d2) return FWR_UNSUPPORTED;
    struct scan src = {.name = l1}, dst = {.name = l2};
    if ((r = scan_dir(v, d1, &src))) return r;
    if (!src.found) return FWR_NOT_FOUND;
    if (src.loc.entry[11] & 0x18) return FWR_UNSUPPORTED;
    if ((r = scan_dir(v, d1, &dst))) return r;
    if (dst.found && src.loc.sfn.sector == dst.loc.sfn.sector && src.loc.sfn.index == dst.loc.sfn.index) return FWR_OK;
    if (open_file_at(v, &src.loc.sfn) || (dst.found && open_file_at(v, &dst.loc.sfn))) return FWR_BUSY;
    for (struct fwr_file *f = v->files; f; f = f->next)
        if (f->directory == d1 && (name_equal(f->name, l1) || name_equal(f->name, l2))) return FWR_BUSY;
    if (dst.found && (dst.loc.entry[11] & 0x18)) return FWR_UNSUPPORTED;
    uint32_t first = entry_cluster(src.loc.entry), size = le32(src.loc.entry + 28);
    if ((r = begin_meta(v))) return r;
    /* Step 0: tag the source so that recovery can tell it from the destination when both name one chain. */
    if (first) {
        if ((r = dir_load(v, src.loc.sfn.sector))) return r;
        v->dir_sector[32 * src.loc.sfn.index + 12] |= 0x80;
        if ((r = dir_store(v, src.loc.sfn.sector))) return r;
    }
    uint32_t old = 0;
    if (dst.found) {
        old = entry_cluster(dst.loc.entry);
        if ((r = entry_update(v, &dst.loc.sfn, first, size))) return r;      /* the one-sector switch */
    } else {
        struct fwr_slot slot;
        if ((r = entry_create(v, d1, l2, src.loc.entry[11], first, size, &slot))) return r;
    }
    if ((r = entry_delete(v, &src.loc))) return r;
    if (old && (r = free_chain(v, old))) return r;
    return FWR_OK;
}

/* ---------------------------------------------------------------- recovery */

struct diter { uint32_t first, cluster, sec, idx, walked; unsigned lfn_n, lfn_exp; uint8_t lfn_sum; struct fwr_slot lfn[20]; };

static int diter_next(struct fwr_volume *v, struct diter *it, struct fwr_loc *loc)
{
    for (;;) {
        if (!it->cluster) return 0;
        uint64_t abs = cluster_sector(v, it->cluster) + it->sec;
        int r = dir_load(v, abs);
        if (r) return r;
        uint8_t *e = v->dir_sector + 32 * it->idx;
        unsigned idx = it->idx;
        if (++it->idx == ENTRIES) {
            it->idx = 0;
            if (++it->sec == v->spc) {
                it->sec = 0;
                uint32_t next;
                if ((r = fat_get(v, it->cluster, &next))) return r;
                if (next >= EOC_MIN) it->cluster = 0;
                else if (next < 2 || next == BAD || next >= v->clusters + 2 || ++it->walked >= DIR_MAX_CLUSTERS) return FWR_CORRUPT;
                else it->cluster = next;
            }
        }
        if (e[0] == 0x00) { it->cluster = 0; return 0; }
        if (e[0] == 0xe5) { it->lfn_exp = 0; it->lfn_n = 0; continue; }
        if ((e[11] & 0x3f) == 0x0f) {
            unsigned seq = e[0] & 0x3f;
            if (e[0] & 0x40) { it->lfn_exp = seq; it->lfn_sum = e[13]; it->lfn_n = 0; }
            if (!it->lfn_exp || seq != it->lfn_exp || e[13] != it->lfn_sum || seq < 1 || seq > 20) { it->lfn_exp = 0; continue; }
            it->lfn[it->lfn_n++] = (struct fwr_slot){abs, (uint8_t)idx};
            --it->lfn_exp;
            continue;
        }
        if (e[11] & 0x08) { it->lfn_exp = 0; continue; }
        if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' '))) { it->lfn_exp = 0; it->lfn_n = 0; continue; }
        memset(loc, 0, sizeof *loc);
        loc->sfn = (struct fwr_slot){abs, (uint8_t)idx};
        memcpy(loc->entry, e, 32);
        if (it->lfn_n && it->lfn_exp == 0 && it->lfn_sum == sfn_checksum(e)) {
            loc->lfn_count = (uint8_t)it->lfn_n;
            memcpy(loc->lfn, it->lfn, it->lfn_n * sizeof it->lfn[0]);
        }
        it->lfn_n = 0; it->lfn_exp = 0;
        return 1;
    }
}

static int bm_get(const struct fwr_volume *v, uint32_t c) { return v->cfg.recovery_bitmap[c / 8] >> (c % 8) & 1; }
static void bm_set(struct fwr_volume *v, uint32_t c) { v->cfg.recovery_bitmap[c / 8] |= (uint8_t)(1u << (c % 8)); }

/* Mark a chain; stop at the first free, bad, out of range or already claimed cluster, and at max
 * clusters when max is nonzero. The last kept cluster becomes the end of the chain. */
static int mark_chain(struct fwr_volume *v, uint32_t first, uint32_t max, uint32_t *len_out)
{
    uint32_t c = first, prev = 0, len = 0, val;
    int r, ended = 0;
    while (!max || len < max) {
        if (c < 2 || c >= v->clusters + 2 || bm_get(v, c)) break;
        if ((r = fat_get(v, c, &val))) return r;
        if (val == 0 || val == BAD) break;
        bm_set(v, c); ++len; prev = c;
        if (val >= EOC_MIN) { ended = 1; break; }
        c = val;
    }
    if (prev && !ended) {
        if ((r = fat_set(v, prev, EOC))) return r;
    }
    *len_out = len;
    return FWR_OK;
}

static int entry_fix(struct fwr_volume *v, const struct fwr_slot *slot, int clear_tag, uint32_t first, uint32_t size, int set_fields)
{
    int r = dir_load(v, slot->sector);
    if (r) return r;
    uint8_t *e = v->dir_sector + 32 * slot->index;
    if (clear_tag) e[12] &= (uint8_t)~0x80;
    if (set_fields) { put16(e + 20, first >> 16); put16(e + 26, first); put32(e + 28, size); }
    return dir_store(v, slot->sector);
}

#define REC_STACK 256u

static int recover(struct fwr_volume *v)
{
    size_t bytes = ((size_t)v->clusters + 2 + 7) / 8;
    if (!v->cfg.recovery_bitmap || v->cfg.recovery_bitmap_bytes < bytes) return FWR_NEEDS_RECOVERY;
    memset(v->cfg.recovery_bitmap, 0, bytes);
    v->in_recovery = 1;
    uint32_t stack[REC_STACK]; unsigned sp = 0;
    uint32_t len;
    int r = mark_chain(v, v->root, 0, &len);
    if (r) return r;
    if (!len) return FWR_CORRUPT;
    stack[sp++] = v->root;
    while (sp) {
        struct diter it = {.first = stack[sp - 1], .cluster = stack[sp - 1]};
        --sp;
        struct fwr_loc loc;
        while ((r = diter_next(v, &it, &loc)) == 1) {
            const uint8_t *e = loc.entry;
            uint32_t first = entry_cluster(e), size = le32(e + 28);
            if (e[12] & 0x80) {
                /* An interrupted rename: the tagged entry is the source. */
                int dup = 0;
                if (first) {
                    struct diter scan2 = {.first = it.first, .cluster = it.first};
                    struct fwr_loc other;
                    uint64_t save = v->dir_cached;
                    (void)save;
                    int q;
                    while ((q = diter_next(v, &scan2, &other)) == 1)
                        if (!(other.entry[12] & 0x80) && entry_cluster(other.entry) == first && !(other.entry[11] & 0x10)) { dup = 1; break; }
                    if (q < 0) return q;
                }
                if (dup) {
                    if ((r = entry_delete(v, &loc))) return r;
                    continue;
                }
                if ((r = entry_fix(v, &loc.sfn, 1, 0, 0, 0))) return r;
            }
            if (e[11] & 0x10) {
                if (first < 2 || (r = mark_chain(v, first, 0, &len)) || !len) {
                    if (r) return r;
                    if ((r = entry_delete(v, &loc))) return r;
                    continue;
                }
                if (sp == REC_STACK) return FWR_UNSUPPORTED;
                stack[sp++] = first;
            } else {
                uint64_t needed = ((uint64_t)size + v->csize - 1) / v->csize;
                uint32_t nf = first, ns = size;
                if (!needed) nf = 0;
                else {
                    if ((r = mark_chain(v, first, (uint32_t)needed, &len))) return r;
                    if (len < needed) { ns = len * v->csize; if (!len) nf = 0; }
                }
                if (nf != first || ns != size) {
                    if ((r = entry_fix(v, &loc.sfn, 0, nf, ns, 1))) return r;
                }
            }
        }
        if (r < 0) return r;
    }
    if ((r = fat_flush(v))) return r;
    /* Free every cluster no entry claims; make both FAT copies equal; count the free clusters. */
    uint32_t entries = v->clusters + 2, free_count = 0, lowest = 0, freed = 0, highest_used = 1;
    uint32_t old_hint = v->next_hint >= 2 && v->next_hint < entries ? v->next_hint : 0, old_hint_free = 0;
    for (uint32_t first = 0; first * (S / 4) < entries; first += FWR_FAT_WINDOW_SECTORS) {
        if ((r = window_for(v, first))) return r;
        for (uint32_t i = 0; i < v->window_valid_sectors * (S / 4); ++i) {
            uint32_t c = first * (S / 4) + i;
            if (c < 2) continue;
            if (c >= entries) break;
            uint32_t val = le32(v->cfg.fat_window + (size_t)i * 4) & MASK;
            if (val != 0 && val != BAD && !bm_get(v, c)) {
                if ((r = fat_set(v, c, 0))) return r;
                val = 0; ++freed;
            }
            if (!val) { ++free_count; if (!lowest) lowest = c; } else highest_used = c;
            if (c == old_hint) old_hint_free = !val;
        }
        if ((r = fat_flush(v))) return r;
        if (v->fats == 2) {
            uint8_t *other = v->cfg.fat_window + (size_t)FWR_FAT_WINDOW_SECTORS * S;
            uint32_t n = v->window_valid_sectors;
            if ((r = dev_read(v, v->fat2 + first, n, other))) return r;
            for (uint32_t i = 0; i < n;) {
                if (!memcmp(v->cfg.fat_window + (size_t)i * S, other + (size_t)i * S, S)) { ++i; continue; }
                uint32_t j = i;
                while (j < n && memcmp(v->cfg.fat_window + (size_t)j * S, other + (size_t)j * S, S)) ++j;
                if ((r = dev_write(v, v->fat2 + first + i, j - i, v->cfg.fat_window + (size_t)i * S, 0))) return r;
                i = j;
            }
        }
    }
    /* The allocation hint: the old one while it still points at free space, else the first cluster after the highest
     * used one, else the lowest free one. Rebuilding the count by a full scan must not send the next allocation back
     * to the start of the FAT. */
    v->free_clusters = free_count;
    v->next_hint = old_hint_free ? old_hint : highest_used + 1 < entries ? highest_used + 1 : lowest ? lowest : 2;
    if ((r = fsinfo_write(v))) return r;
    v->wear.recoveries++; v->wear.recovered_clusters += freed;
    r = set_flag(v, 1);
    v->in_recovery = 0;
    return r;
}

/* The caller owns these objects and may be suspended at any instruction.
 * Only completed owner events change the independent retirement proof. */
static int transaction_enter(struct fwr_volume *v, unsigned operation)
{
    if (v->transaction_depth) { ++v->transaction_depth; return FWR_OK; }
    v->transaction_operation = operation; v->transaction_failed = 0;
    v->transaction_token = 0;
    if (v->cfg.device.transaction &&
        v->cfg.device.transaction(v->cfg.device.context, FWR_TX_BEGIN, operation, &v->transaction_token) != 1)
        return FWR_REFUSED;
    v->transaction_depth = 1;
    return FWR_OK;
}
static int transaction_leave(struct fwr_volume *v, int result)
{
    if (result) v->transaction_failed = 1;
    if (--v->transaction_depth) return result;
    unsigned event = v->transaction_failed ? FWR_TX_ABORT : FWR_TX_COMMIT;
    if (v->cfg.device.transaction &&
        v->cfg.device.transaction(v->cfg.device.context, event, v->transaction_operation, &v->transaction_token) != 1 && !result)
        result = FWR_IO;
    v->transaction_token = 0;
    return result;
}
int fwr_mount(struct fwr_volume *v, const struct fwr_config *cfg)
{
    if (!v || !cfg || !cfg->device.read || !cfg->device.write || !cfg->cluster_buffer || !cfg->fat_window ||
        (!!cfg->device.transaction != !!cfg->device.write_classified)) return FWR_INVALID;
    struct fwr_config copy = *cfg;
    memset(v, 0, sizeof *v); v->cfg = copy;
    int r = transaction_enter(v, FWR_OP_MOUNT);
    return r ? r : transaction_leave(v, mount_transaction(v, &v->cfg));
}
int fwr_checkpoint(struct fwr_volume *v)
{
    if (!v) return FWR_INVALID;
    int r = transaction_enter(v, FWR_OP_CHECKPOINT);
    return r ? r : transaction_leave(v, checkpoint_transaction(v));
}
int fwr_sync(struct fwr_file *f)
{
    if (!f || !f->volume) return FWR_INVALID;
    int r = transaction_enter(f->volume, FWR_OP_SYNC);
    return r ? r : transaction_leave(f->volume, sync_transaction(f));
}
int fwr_truncate(struct fwr_file *f, uint64_t size)
{
    if (!f || !f->volume) return FWR_INVALID;
    int r = transaction_enter(f->volume, FWR_OP_TRUNCATE);
    return r ? r : transaction_leave(f->volume, truncate_transaction(f, size));
}
int fwr_mkdir(struct fwr_volume *v, const char *path)
{
    if (!v) return FWR_INVALID;
    int r = transaction_enter(v, FWR_OP_MKDIR);
    return r ? r : transaction_leave(v, mkdir_transaction(v, path));
}
int fwr_unlink(struct fwr_volume *v, const char *path)
{
    if (!v) return FWR_INVALID;
    int r = transaction_enter(v, FWR_OP_UNLINK);
    return r ? r : transaction_leave(v, unlink_transaction(v, path));
}
int fwr_rename(struct fwr_volume *v, const char *from, const char *to)
{
    if (!v) return FWR_INVALID;
    int r = transaction_enter(v, FWR_OP_RENAME);
    return r ? r : transaction_leave(v, rename_transaction(v, from, to));
}
