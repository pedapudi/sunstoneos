#ifndef A733_STAGE2_SESSION_H
#define A733_STAGE2_SESSION_H
#include "a733_board_application.h"
#include "a733_cpu_clock.h"
#include "sunstone_session.h"

/* Cold templates carry no prior boot's epoch. Inspect only framing and cold
 * identity here; the OS still validates service policy against its manifest.
 * The OS stamps its private CPU-grant copy after the loader stamps the board
 * epoch. An inactive grant keeps both identity fields zero. */
static int a733_stage2_session_valid(const void *raw, size_t bytes, uint64_t generation)
{
    if (!raw || ((uintptr_t)raw & 7) || !generation ||
        bytes < SS_SESSION_PREFIX_BYTES + sizeof(struct ss_session_header) ||
        bytes > SS_SESSION_MAX_BYTES) return 0;
    const struct a733_board_session *s = raw;
    const uint8_t *p = raw;
    const struct ss_session_header *h = (const void *)(p + SS_SESSION_PREFIX_BYTES);
    if (s->magic != A733_BOARD_SUPERVISED_SESSION_MAGIC || s->version != 2 ||
        s->bytes != sizeof(*s) || s->epoch || s->generation != generation || !s->lease ||
        h->magic != SS_SESSION_MAGIC || h->version != SS_SESSION_VERSION ||
        h->bytes != bytes || (h->flags & ~SS_SESSION_BIND_SID)) return 0;
    size_t off = SS_SESSION_PREFIX_BYTES + sizeof(*h);
    if (h->extents > (bytes - off) / sizeof(struct ss_extent)) return 0;
    uint32_t seen = 0;
    for (unsigned i = 0; i < h->extents; ++i) {
        if (bytes - off < sizeof(struct ss_extent)) return 0;
        const struct ss_extent *e = (const void *)(p + off);
        off += sizeof(*e);
        if (e->type >= 32 || (seen & (1u << e->type)) || e->bytes > bytes - off) return 0;
        seen |= 1u << e->type;
        size_t padded = ((size_t)e->bytes + 7u) & ~(size_t)7;
        if (padded > bytes - off) return 0;
        if (e->type == SS_EXTENT_CPU_GRANT) {
            if (e->bytes != sizeof(struct a733_cpu_clock_grant_record)) return 0;
            const struct a733_cpu_clock_grant_record *g = (const void *)(p + off);
            if (g->magic != A733_CPU_CLOCK_GRANT_MAGIC || g->version != A733_CPU_CLOCK_GRANT_VERSION ||
                g->bytes != sizeof(*g) || !g->owner || g->epoch ||
                g->generation != (g->clusters ? generation : 0)) return 0;
        }
        for (size_t j = e->bytes; j < padded; ++j) if (p[off + j]) return 0;
        off += padded;
    }
    return off == bytes;
}
#endif
