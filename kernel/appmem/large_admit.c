#include "app_region.h"
#include "sunstone_large.h"
#include "app_large.h"
#include <string.h>

/* Length of the path, or -1 when it holds a character outside the allowed set or no terminator. */
static int path_ok(const char *path)
{
    size_t n = 0;
    while (n < SS_LARGE_PATH_BYTES && path[n]) {
        char c = path[n++];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '/';
        if (!ok) return -1;
    }
    if (n == SS_LARGE_PATH_BYTES) return -1;                   /* no terminator */
    for (size_t i = n; i < SS_LARGE_PATH_BYTES; ++i) if (path[i]) return -1;
    return (int)n;
}

int ss_large_records_valid(const struct ss_large_region *r, unsigned count)
{
    if (count > APP_REGION_MAX_REGIONS || (count && !r)) return 0;
    uint64_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (r[i].index != i || (r[i].flags & ~(unsigned)(SS_LARGE_WRITE | SS_LARGE_EXTENT | SS_LARGE_HEAP))) return 0;
        if (!r[i].bytes || r[i].bytes % APP_REGION_BLOCK || r[i].bytes > APP_REGION_LIMIT - APP_REGION_BASE) return 0;
        total += r[i].bytes;
        if (total > APP_REGION_LIMIT - APP_REGION_BASE) return 0;
        int zero = 1;
        for (unsigned k = 0; k < 32; ++k) if (r[i].sha256[k]) zero = 0;
        int n = path_ok(r[i].path);
        if (n < 0) return 0;
        if (r[i].flags & SS_LARGE_HEAP) {
            /* The heap has no source: read-write, the last record, nothing to load or to verify. */
            if (r[i].flags != (SS_LARGE_HEAP | SS_LARGE_WRITE) || i != count - 1 || r[i].source_bytes || r[i].first_sector ||
                n || !zero) return 0;
            continue;
        }
        if (!r[i].source_bytes || r[i].source_bytes > r[i].bytes || zero) return 0;
        if (r[i].flags & SS_LARGE_EXTENT) {
            if (n || !r[i].first_sector) return 0;                 /* sector 0 holds the partition table */
        } else if (!n || r[i].path[0] != '/' || r[i].first_sector) return 0;
    }
    return 1;
}

int large_admit(struct app_region_ledger *l, uint16_t owner, const struct ss_large_region *r, unsigned count,
                struct large_slot *out, unsigned out_capacity)
{
    if (out_capacity) memset(out, 0, sizeof(struct large_slot) * out_capacity);
    if (!l || !owner || out_capacity < count || (count && !out) || !ss_large_records_valid(r, count)) return APP_REGION_INVALID;
    int status = APP_REGION_OK;
    for (unsigned i = 0; i < count && status == APP_REGION_OK; ++i) {
        struct large_slot *s = &out[i];
        unsigned access = APP_REGION_READ | (r[i].flags & SS_LARGE_WRITE ? APP_REGION_WRITE : 0);
        status = app_region_acquire(l, owner, r[i].bytes, access, &s->base, &s->bytes);
        if (status == APP_REGION_OK) {
            s->index = r[i].index; s->flags = r[i].flags;
            s->state = r[i].flags & SS_LARGE_HEAP ? SS_LARGE_READY : SS_LARGE_PENDING;    /* nothing loads the heap */
        }
    }
    if (status != APP_REGION_OK) {                               /* all or nothing */
        for (unsigned i = 0; i < count; ++i)
            if (out[i].bytes) app_region_release(l, owner, out[i].base);
        memset(out, 0, sizeof(struct large_slot) * out_capacity);
    }
    return status;
}
