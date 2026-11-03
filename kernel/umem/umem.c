#include "umem.h"
#include <string.h>

/* One cell is an extent (free or allocated), a mapping record or a spare. Extents sit in one treap ordered by base,
 * so lookup, insertion and removal take a logarithmic number of steps; free extents are also in a list per policy
 * and size class, so a request visits only extents that can be large enough. */
#define KIND_SPARE 1u
#define KIND_EXTENT 2u
#define KIND_MAP 3u

#define E_FREE 1u
#define E_DIRTY 2u          /* the extent may hold dirty cache lines */
#define E_TOUCHED 4u        /* the CPU may have written the buffer since it became ready */
#define E_NEEDS_ZERO 8u
#define E_NEEDS_CLEAN 16u
#define E_READY 32u
#define E_META 64u

struct ent {
    uint8_t kind, policy, state, flags, access;
    uint16_t owner, last_owner;
    uint32_t generation, prio;
    uint64_t base, bytes, prepared;
    union umem_cell *left, *right, *prev, *next;
    union umem_cell *maps;
};
struct map_rec {
    uint8_t kind;
    uint16_t prot;
    uint64_t address, bytes;
    void *space;
    const struct umem_space_ops *ops;
    union umem_cell *next;
};
union umem_cell { uint8_t kind; struct ent e; struct map_rec m; };
typedef union umem_cell C;

size_t umem_cell_bytes(void) { return sizeof(C); }

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
static int pow2(uint64_t v) { return v && !(v & (v - 1)); }
static unsigned size_class(uint64_t pages)
{
    unsigned c = 0;
    while (pages > 1) { pages >>= 1; ++c; }
    return c;
}
static void lock(struct umem *u) { if (u->hooks.lock) u->hooks.lock(u->hooks.ctx); }
static void unlock(struct umem *u) { if (u->hooks.unlock) u->hooks.unlock(u->hooks.ctx); }
static uint32_t next_prio(struct umem *u)
{
    uint32_t x = u->rng ? u->rng : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return u->rng = x;
}

/* ---- spare cells ---- */
static C *cell_get(struct umem *u)
{
    C *c = u->spare;
    if (!c) return 0;
    u->spare = c->e.next;
    --u->spare_count;
    memset(c, 0, sizeof *c);
    return c;
}
static void cell_put(struct umem *u, C *c)
{
    memset(c, 0, sizeof *c);
    c->e.kind = KIND_SPARE;
    c->e.next = u->spare;
    u->spare = c;
    ++u->spare_count;
}

/* ---- the treap of extents ---- */
static void split(struct umem *u, C *t, uint64_t key, C **l, C **r)   /* l: base < key */
{
    if (!t) { *l = *r = 0; return; }
    ++u->steps;
    if (t->e.base < key) { *l = t; split(u, t->e.right, key, &t->e.right, r); }
    else { *r = t; split(u, t->e.left, key, l, &t->e.left); }
}
static C *merge(struct umem *u, C *a, C *b)
{
    if (!a) return b;
    if (!b) return a;
    ++u->steps;
    if (a->e.prio > b->e.prio) { a->e.right = merge(u, a->e.right, b); return a; }
    b->e.left = merge(u, a, b->e.left);
    return b;
}
static void tree_insert(struct umem *u, C *n)
{
    C *l, *r;
    n->e.left = n->e.right = 0;
    split(u, u->root, n->e.base, &l, &r);
    u->root = merge(u, merge(u, l, n), r);
}
static void tree_erase(struct umem *u, uint64_t base)
{
    C *l, *m, *r, *r2;
    split(u, u->root, base, &l, &r);
    split(u, r, base + 1, &m, &r2);
    u->root = merge(u, l, r2);
}
static C *tree_floor(struct umem *u, uint64_t addr)     /* the extent with the greatest base <= addr */
{
    C *best = 0;
    for (C *t = u->root; t; ++u->steps) {
        if (t->e.base <= addr) { best = t; t = t->e.right; } else t = t->e.left;
    }
    return best;
}
static C *tree_floor_const(const struct umem *u, uint64_t addr)
{
    C *best = 0;
    for (C *t = u->root; t;) {
        if (t->e.base <= addr) { best = t; t = t->e.right; } else t = t->e.left;
    }
    return best;
}

/* ---- the free lists ---- */
static void list_add(struct umem *u, C *f)
{
    C **head = &u->free_head[f->e.policy][size_class(f->e.bytes / UMEM_PAGE)];
    f->e.prev = 0;
    f->e.next = *head;
    if (*head) (*head)->e.prev = f;
    *head = f;
}
static void list_remove(struct umem *u, C *f)
{
    C **head = &u->free_head[f->e.policy][size_class(f->e.bytes / UMEM_PAGE)];
    if (f->e.prev) f->e.prev->e.next = f->e.next; else *head = f->e.next;
    if (f->e.next) f->e.next->e.prev = f->e.prev;
    f->e.prev = f->e.next = 0;
}

/* Make extent a free, merge it with free neighbors of the same policy and file it. a is in the tree. */
static void release_extent(struct umem *u, C *a, uint64_t dirty, uint16_t last_owner)
{
    a->e.flags = E_FREE | (dirty ? E_DIRTY : 0);
    a->e.last_owner = last_owner;
    a->e.owner = 0; a->e.state = 0; a->e.prepared = 0; a->e.maps = 0;
    u->free_bytes += a->e.bytes;
    C *pred = a->e.base ? tree_floor(u, a->e.base - 1) : 0;
    if (pred && (pred->e.flags & E_FREE) && pred->e.policy == a->e.policy &&
        pred->e.base + pred->e.bytes == a->e.base) {
        list_remove(u, pred);
        pred->e.bytes += a->e.bytes;
        if (a->e.flags & E_DIRTY) pred->e.flags |= E_DIRTY;
        if (pred->e.last_owner != a->e.last_owner) pred->e.last_owner = UMEM_OWNER_MIXED;
        tree_erase(u, a->e.base);
        cell_put(u, a);
        a = pred;
    }
    C *succ = tree_floor(u, a->e.base + a->e.bytes);
    if (succ && succ != a && succ->e.base == a->e.base + a->e.bytes && (succ->e.flags & E_FREE) &&
        succ->e.policy == a->e.policy) {
        list_remove(u, succ);
        a->e.bytes += succ->e.bytes;
        if (succ->e.flags & E_DIRTY) a->e.flags |= E_DIRTY;
        if (a->e.last_owner != succ->e.last_owner) a->e.last_owner = UMEM_OWNER_MIXED;
        tree_erase(u, succ->e.base);
        cell_put(u, succ);
    }
    list_add(u, a);
}

/* Split free extent f so that [s, s + bytes) becomes its own cell, removed from the free lists; returns it. f, the
 * remainder before it and the remainder after it keep f's flags. Needs two spare cells. */
static C *carve(struct umem *u, C *f, uint64_t s, uint64_t bytes)
{
    uint64_t end = f->e.base + f->e.bytes;
    list_remove(u, f);
    u->free_bytes -= f->e.bytes;
    const uint8_t policy = f->e.policy, dirty = f->e.flags & E_DIRTY;
    const uint16_t last = f->e.last_owner;
    C *a = f;
    if (s > f->e.base) {
        a = cell_get(u);
        f->e.bytes = s - f->e.base;
        u->free_bytes += f->e.bytes;
        list_add(u, f);
        a->e.base = s;
        a->e.prio = next_prio(u);
        a->e.kind = KIND_EXTENT;
    }
    if (s + bytes < end) {
        C *after = cell_get(u);
        after->e.kind = KIND_EXTENT;
        after->e.base = s + bytes; after->e.bytes = end - (s + bytes);
        after->e.policy = policy; after->e.flags = E_FREE | dirty; after->e.last_owner = last;
        after->e.prio = next_prio(u);
        tree_insert(u, after);
        u->free_bytes += after->e.bytes;
        list_add(u, after);
    }
    a->e.base = s; a->e.bytes = bytes;
    a->e.policy = policy; a->e.flags = dirty; a->e.last_owner = last;
    if (a != f) tree_insert(u, a);
    return a;
}

/* The first start in free extent f that satisfies the constraints, or 0 with *ok clear. */
static int fits(const C *f, uint64_t bytes, uint64_t align, uint64_t phys_max, uint64_t window, uint64_t *start)
{
    uint64_t end = f->e.base + f->e.bytes, s = align_up(f->e.base, align);
    if (window) {
        if (bytes > window) return 0;
        for (int i = 0; i < 2; ++i) {
            if ((s & ~(window - 1)) == ((s + bytes - 1) & ~(window - 1))) break;
            s = align_up(s, window);
            s = align_up(s, align);
        }
    }
    if (s < f->e.base || s > end || bytes > end - s) return 0;
    if (phys_max && s + bytes > phys_max) return 0;
    if (window && (s & ~(window - 1)) != ((s + bytes - 1) & ~(window - 1))) return 0;
    *start = s;
    return 1;
}

/* Segregated fit: every extent of a class above the request's holds the request, so the scan starts there and a plain
 * request takes the first extent it meets. Extents of the request's own class are scanned last, because some of them
 * are smaller than the request. */
static C *scan_class(struct umem *u, unsigned policy, unsigned c, uint64_t bytes, uint64_t align, uint64_t phys_max,
                     uint64_t window, uint64_t *start)
{
    for (C *f = u->free_head[policy][c]; f; f = f->e.next) {
        ++u->steps;
        if (f->e.bytes >= bytes && fits(f, bytes, align, phys_max, window, start)) return f;
    }
    return 0;
}
static C *search(struct umem *u, unsigned policy, uint64_t bytes, uint64_t align, uint64_t phys_max,
                 uint64_t window, uint64_t *start)
{
    unsigned c0 = size_class(bytes / UMEM_PAGE);
    if (u->exact_first) {
        C *f = scan_class(u, policy, c0, bytes, align, phys_max, window, start);
        if (f) return f;
    }
    for (unsigned c = c0 + 1; c < UMEM_CELL_CLASSES; ++c) {
        C *f = scan_class(u, policy, c, bytes, align, phys_max, window, start);
        if (f) return f;
    }
    return scan_class(u, policy, c0, bytes, align, phys_max, window, start);
}

/* ---- run-time growth ---- */
static int alloc_locked(struct umem *u, uint64_t bytes, uint64_t align, uint64_t phys_max, uint64_t window,
                        unsigned policy, uint16_t owner, int at, uint64_t at_base, int convert, C **out);

static int grow(struct umem *u)
{
    if (u->growing) return UMEM_NO_META;
    C *page;
    u->growing = 1;
    int r = alloc_locked(u, UMEM_PAGE, UMEM_PAGE, 0, 0, UMEM_WB, UMEM_OWNER_KERNEL, 0, 0, 0, &page);
    u->growing = 0;
    if (r) return UMEM_NO_META;
    page->e.flags |= E_META | E_READY | E_TOUCHED;
    u->meta_bytes += UMEM_PAGE;
    C *cells = u->hooks.phys_to_virt ? u->hooks.phys_to_virt(u->hooks.ctx, page->e.base) : (C *)(uintptr_t)page->e.base;
    for (size_t i = 0; i < UMEM_PAGE / sizeof(C); ++i) cell_put(u, &cells[i]);
    return UMEM_OK;
}
static int ensure(struct umem *u, uint64_t n)
{
    while (u->spare_count < n) { int r = grow(u); if (r) return r; }
    return UMEM_OK;
}

/* ---- cache policy conversion ---- */
static int chunk_index(const struct umem *u, uint64_t addr, uint64_t *index)
{
    if (addr < u->chunk_base) return 0;
    uint64_t i = (addr - u->chunk_base) / UMEM_CHUNK;
    if (i >= u->chunk_count) return 0;
    *index = i;
    return 1;
}

/* Make every chunk of [lo, hi) carry `policy`. Each chunk that differs must lie in a free extent. */
static int convert_range(struct umem *u, uint64_t lo, uint64_t hi, unsigned policy)
{
    for (uint64_t c = lo; c < hi; c += UMEM_CHUNK) {
        uint64_t index;
        if (!chunk_index(u, c, &index)) return UMEM_NO_SPACE;
        if (u->chunk_policy[index] == policy) continue;
        int r = ensure(u, 4);
        if (r) return r;
        C *f = tree_floor(u, c);
        if (!f || !(f->e.flags & E_FREE) || f->e.base + f->e.bytes < c + UMEM_CHUNK || f->e.policy == policy)
            return UMEM_NO_SPACE;
        const uint8_t old = f->e.policy;
        C *x = carve(u, f, c, UMEM_CHUNK);
        uint64_t dirty = x->e.flags & E_DIRTY;
        const uint16_t last = x->e.last_owner;
        r = u->hooks.convert_chunk ? u->hooks.convert_chunk(u->hooks.ctx, c, policy) : UMEM_HW;
        if (r) {
            x->e.policy = old;
            release_extent(u, x, dirty, last);
            return UMEM_HW;
        }
        u->chunk_policy[index] = (uint8_t)policy;
        x->e.policy = (uint8_t)policy;
        release_extent(u, x, 0, last);       /* the conversion invalidated the chunk */
        (void)dirty;
    }
    return UMEM_OK;
}

/* ---- allocation ---- */
static int alloc_locked(struct umem *u, uint64_t bytes, uint64_t align, uint64_t phys_max, uint64_t window,
                        unsigned policy, uint16_t owner, int at, uint64_t at_base, int convert, C **out)
{
    uint64_t s = 0;
    C *f = 0;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (at) {
            f = tree_floor(u, at_base);
            if (f && (f->e.flags & E_FREE) && f->e.policy == policy && at_base + bytes <= f->e.base + f->e.bytes) { s = at_base; break; }
            f = 0;
        } else {
            if (bytes >= UMEM_GIB && align < UMEM_GIB) f = search(u, policy, bytes, UMEM_GIB, phys_max, window, &s);
            if (!f) f = search(u, policy, bytes, align, phys_max, window, &s);
            if (f) break;
        }
        if (attempt || !convert) break;
        /* No extent of this policy: convert chunks of free memory, once. */
        uint64_t lo, hi;
        if (at) {
            lo = at_base & ~(UMEM_CHUNK - 1); hi = align_up(at_base + bytes, UMEM_CHUNK);
        } else {
            uint64_t k = align_up(bytes, UMEM_CHUNK);
            uint64_t ca = align > UMEM_CHUNK ? align : UMEM_CHUNK, cs;
            C *g = search(u, !policy, k, ca, phys_max, window, &cs);
            if (!g) return UMEM_NO_SPACE;
            lo = cs; hi = cs + k;
        }
        int r = convert_range(u, lo, hi, policy);
        if (r) return r;
    }
    if (!f) return UMEM_NO_SPACE;
    C *a = carve(u, f, s, bytes);
    a->e.kind = KIND_EXTENT;
    a->e.owner = owner;
    a->e.state = UMEM_CPU;
    a->e.generation = ++u->generation ? u->generation : ++u->generation;
    a->e.flags &= (uint8_t)E_DIRTY;
    a->e.prepared = 0;
    a->e.maps = 0;
    *out = a;
    return UMEM_OK;
}

static uint64_t device_align(unsigned mask, uint64_t bytes)
{
    uint64_t a = UMEM_PAGE;
    if (mask & UMEM_DEV_NPU) {
        uint64_t n = bytes >= (16u << 20) ? (16u << 20) : bytes >= (1u << 20) ? (1u << 20) : bytes >= (64u << 10) ? (64u << 10) : UMEM_PAGE;
        if (n > a) a = n;
    }
    if ((mask & UMEM_DEV_GPU) && bytes >= (256u << 10) && a < (256u << 10)) a = 256u << 10;
    return a;
}

int umem_alloc(struct umem *u, const struct umem_request *rq, struct umem_buf *out)
{
    if (!u || !rq || !out || !rq->bytes || !rq->owner || rq->policy > UMEM_NC ||
        (rq->flags & ~(unsigned)(UMEM_AT | UMEM_REUSE | UMEM_NO_CONVERT | UMEM_RAW)) || (rq->align && !pow2(rq->align)) ||
        (rq->window && !pow2(rq->window)) || (rq->device_mask >> UMEM_DEVICES) ||
        rq->bytes > UINT64_MAX - UMEM_PAGE) return UMEM_INVALID;
    uint64_t bytes = align_up(rq->bytes, UMEM_PAGE);
    uint64_t align = rq->align > UMEM_PAGE ? rq->align : UMEM_PAGE;
    uint64_t da = device_align(rq->device_mask, bytes);
    if (da > align) align = da;
    uint64_t phys_max = rq->phys_max;
    for (unsigned d = 0; d < UMEM_DEVICES; ++d)
        if ((rq->device_mask >> d) & 1u && (!phys_max || u->device_limit[d] < phys_max)) phys_max = u->device_limit[d];
    int at = (rq->flags & UMEM_AT) != 0;
    if (at && ((rq->base & (UMEM_PAGE - 1)) || (rq->base & (align - 1)) || rq->base + bytes < rq->base ||
               (phys_max && rq->base + bytes > phys_max) ||
               (rq->window && (rq->base & ~(rq->window - 1)) != ((rq->base + bytes - 1) & ~(rq->window - 1))))) return UMEM_INVALID;
    lock(u);
    int r = ensure(u, 8);
    C *a = 0;
    u->exact_first = (rq->flags & UMEM_REUSE) != 0;
    if (!r) r = alloc_locked(u, bytes, align, phys_max, rq->window, rq->policy, rq->owner, at, rq->base,
                             !(rq->flags & UMEM_NO_CONVERT), &a);
    u->exact_first = 0;
    if (!r) {
        uint8_t was_dirty = a->e.flags & E_DIRTY;
        int zero = !((rq->flags & UMEM_REUSE) && a->e.last_owner == rq->owner);
        int clean = rq->policy == UMEM_WB && (was_dirty || zero);
        a->e.flags = (uint8_t)(was_dirty | (zero ? E_NEEDS_ZERO : 0) | (clean ? E_NEEDS_CLEAN : 0));
        a->e.access = rq->access;
        if (rq->flags & UMEM_RAW) { zero = clean = 0; a->e.flags = was_dirty; }
        if (!zero && !clean) a->e.flags |= E_READY;
        *out = (struct umem_buf){a->e.base, a->e.bytes, a->e.generation, a};
    }
    unlock(u);
    return r;
}

static int live(const struct umem_buf *b)
{
    return b && b->cell && b->generation && b->cell->kind == KIND_EXTENT &&
           __atomic_load_n(&b->cell->e.generation, __ATOMIC_ACQUIRE) == b->generation;
}

int umem_ready(const struct umem *u, const struct umem_buf *b)
{
    (void)u;
    return live(b) && (b->cell->e.flags & E_READY) ? 1 : 0;
}

int umem_prepare_step(struct umem *u, const struct umem_buf *b, uint64_t max_bytes)
{
    if (!u || !live(b)) return UMEM_NOT_OWNED;
    C *c = b->cell;
    if (c->e.flags & E_READY) return 1;
    uint64_t remaining = c->e.bytes - c->e.prepared;
    uint64_t slice = max_bytes ? align_up(max_bytes, UMEM_PAGE) : remaining;
    if (slice > remaining) slice = remaining;
    int op_zero = (c->e.flags & E_NEEDS_ZERO) != 0;
    int r = u->hooks.maintain ? u->hooks.maintain(u->hooks.ctx, op_zero ? UMEM_OP_ZERO : UMEM_OP_CLEAN,
                                                  c->e.base + c->e.prepared, slice) : UMEM_HW;
    if (r) return UMEM_HW;
    c->e.prepared += slice;
    if (c->e.prepared < c->e.bytes) return 0;
    c->e.prepared = 0;
    c->e.flags &= (uint8_t)~(op_zero ? E_NEEDS_ZERO : E_NEEDS_CLEAN);
    if (c->e.flags & (E_NEEDS_ZERO | E_NEEDS_CLEAN)) return 0;
    if (c->e.policy == UMEM_NC && u->hooks.maintain) u->hooks.maintain(u->hooks.ctx, UMEM_OP_STORE_BARRIER, c->e.base, c->e.bytes);
    c->e.flags &= (uint8_t)~(E_DIRTY | E_TOUCHED);
    c->e.flags |= E_READY;
    return 1;
}

int umem_describe(const struct umem *u, const struct umem_buf *b, struct umem_extent *out, int *state)
{
    (void)u;
    if (!b || !b->cell || !b->generation || !out) return UMEM_INVALID;
    const C *c = b->cell;
    if (__atomic_load_n(&c->e.generation, __ATOMIC_ACQUIRE) != b->generation) return UMEM_STALE;
    struct umem_extent e = {c->e.base, c->e.bytes, c->e.owner, c->e.policy, c->e.access};
    int st = c->e.state;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&c->e.generation, __ATOMIC_RELAXED) != b->generation) return UMEM_STALE;
    *out = e;
    if (state) *state = st;
    return UMEM_OK;
}

int umem_touch(struct umem *u, const struct umem_buf *b)
{
    if (!u || !live(b)) return UMEM_NOT_OWNED;
    b->cell->e.flags |= E_TOUCHED;
    return UMEM_OK;
}

int umem_free(struct umem *u, const struct umem_buf *b)
{
    if (!u || !live(b)) return UMEM_NOT_OWNED;
    lock(u);
    C *c = b->cell;
    int r = UMEM_OK;
    if (!live(b)) r = UMEM_NOT_OWNED;
    else if (c->e.state != UMEM_CPU) r = UMEM_BUSY;
    else for (C *m = c->e.maps; m; m = m->m.next)
        if (m->m.ops->kind != UMEM_SPACE_CPU) { r = UMEM_BUSY; break; }
    if (!r) {
        for (C *m = c->e.maps; m;) {
            C *next = m->m.next;
            if (m->m.ops->unmap) m->m.ops->unmap(m->m.space, m->m.address, m->m.bytes);
            if (m->m.ops->flush) m->m.ops->flush(m->m.space, m->m.address, m->m.bytes);
            cell_put(u, m);
            m = next;
        }
        c->e.maps = 0;
        uint64_t dirty = c->e.policy == UMEM_WB && (c->e.flags & (E_DIRTY | E_TOUCHED));
        uint16_t owner = c->e.owner;
        __atomic_store_n(&c->e.generation, 0, __ATOMIC_RELEASE);
        release_extent(u, c, dirty, owner);
    }
    unlock(u);
    return r;
}

int umem_handoff(struct umem *u, const struct umem_buf *b, enum umem_state from, enum umem_state to,
                 const struct umem_range *ranges, unsigned count)
{
    if (!u || !live(b) || from > UMEM_RETAINED || to > UMEM_RETAINED || (count && !ranges)) return UMEM_INVALID;
    C *c = b->cell;
    if (!(c->e.flags & E_READY)) return UMEM_BUSY;
    if (c->e.state != from) return UMEM_BUSY;
    if (from == UMEM_RETAINED) return UMEM_BUSY;
    for (unsigned i = 0; i < count; ++i)
        if (ranges[i].offset > c->e.bytes || ranges[i].bytes > c->e.bytes - ranges[i].offset) return UMEM_INVALID;
    enum umem_op op = UMEM_OP_STORE_BARRIER;
    int run = 0;
    if (to != UMEM_RETAINED && from != to) {
        if (from == UMEM_CPU) { op = c->e.policy == UMEM_WB ? UMEM_OP_CLEAN : UMEM_OP_STORE_BARRIER; run = 1; }
        else { op = c->e.policy == UMEM_WB ? UMEM_OP_CLEAN_INVALIDATE : UMEM_OP_LOAD_BARRIER; run = 1; }
    }
    if (run && u->hooks.maintain) {
        unsigned n = count ? count : 1;
        for (unsigned i = 0; i < n; ++i) {
            uint64_t off = count ? ranges[i].offset : 0, len = count ? ranges[i].bytes : c->e.bytes;
            if (u->hooks.maintain(u->hooks.ctx, op, c->e.base + off, len)) return UMEM_HW;
        }
    }
    if (to == UMEM_CPU) { c->e.flags |= E_TOUCHED; if (from == UMEM_DEVICE && c->e.policy == UMEM_WB) c->e.flags &= (uint8_t)~E_DIRTY; }
    c->e.state = (uint8_t)to;
    return UMEM_OK;
}

int umem_map(struct umem *u, const struct umem_buf *b, const struct umem_space_ops *ops, void *space,
             uint64_t offset, uint64_t bytes, unsigned prot, uint64_t *address)
{
    if (!u || !live(b) || !ops || !ops->map || !address || !bytes || (prot & ~(unsigned)(UMEM_PROT_READ | UMEM_PROT_WRITE)) ||
        !(prot & UMEM_PROT_READ) || (ops->kind != UMEM_SPACE_CPU && ops->kind != UMEM_SPACE_DEVICE) ||
        offset > b->cell->e.bytes || bytes > b->cell->e.bytes - offset) return UMEM_INVALID;
    if (!(b->cell->e.flags & E_READY)) return UMEM_BUSY;
    lock(u);
    int r = ensure(u, 4);
    C *m = r ? 0 : cell_get(u);
    unlock(u);
    if (r) return r;
    uint64_t a = 0;
    r = ops->map(space, b, offset, bytes, prot, &a);
    lock(u);
    if (r || !live(b)) {
        cell_put(u, m);
        unlock(u);
        if (!r) ops->unmap(space, a, bytes);
        return r ? r : UMEM_NOT_OWNED;
    }
    m->m.kind = KIND_MAP; m->m.prot = (uint16_t)prot; m->m.address = a; m->m.bytes = bytes;
    m->m.space = space; m->m.ops = ops;
    m->m.next = b->cell->e.maps;
    b->cell->e.maps = m;
    if (ops->kind == UMEM_SPACE_CPU && (prot & UMEM_PROT_WRITE)) b->cell->e.flags |= E_TOUCHED;
    unlock(u);
    *address = a;
    return UMEM_OK;
}

int umem_unmap(struct umem *u, const struct umem_buf *b, const struct umem_space_ops *ops, void *space, uint64_t address)
{
    if (!u || !live(b) || !ops) return UMEM_INVALID;
    lock(u);
    C **link = &b->cell->e.maps, *m = *link;
    while (m && !(m->m.space == space && m->m.ops == ops && m->m.address == address)) { link = &m->m.next; m = *link; }
    int r = UMEM_OK;
    if (!m) r = UMEM_NOT_OWNED;
    else if (ops->quiesced && !ops->quiesced(space)) r = UMEM_BUSY;
    else {
        if (ops->unmap) ops->unmap(space, address, m->m.bytes);
        if (ops->flush) ops->flush(space, address, m->m.bytes);
        *link = m->m.next;
        cell_put(u, m);
    }
    unlock(u);
    return r;
}

int umem_trim(struct umem *u, const struct umem_buf *b, uint64_t new_bytes)
{
    if (!u || !live(b) || !new_bytes || (new_bytes & (UMEM_PAGE - 1))) return UMEM_INVALID;
    lock(u);
    C *c = b->cell;
    int r = UMEM_OK;
    if (!live(b)) r = UMEM_NOT_OWNED;
    else if (new_bytes > c->e.bytes) r = UMEM_INVALID;
    else if (new_bytes == c->e.bytes) r = UMEM_OK;
    else if (c->e.state != UMEM_CPU || c->e.maps || !(c->e.flags & E_READY)) r = UMEM_BUSY;
    else if ((r = ensure(u, 4)) == UMEM_OK) {
        C *tail = cell_get(u);
        tail->e.kind = KIND_EXTENT;
        tail->e.base = c->e.base + new_bytes;
        tail->e.bytes = c->e.bytes - new_bytes;
        tail->e.policy = c->e.policy;
        tail->e.prio = next_prio(u);
        uint64_t dirty = c->e.policy == UMEM_WB && (c->e.flags & (E_DIRTY | E_TOUCHED));
        uint16_t owner = c->e.owner;
        c->e.bytes = new_bytes;
        tree_insert(u, tail);
        release_extent(u, tail, dirty, owner);
    }
    unlock(u);
    return r;
}

int umem_largest(struct umem *u, const struct umem_request *rq, uint64_t *base, uint64_t *bytes)
{
    if (!u || !rq || !base || !bytes || rq->policy > UMEM_NC || (rq->align && !pow2(rq->align)) || (rq->window && !pow2(rq->window)) ||
        (rq->device_mask >> UMEM_DEVICES)) return UMEM_INVALID;
    uint64_t align = rq->align > UMEM_PAGE ? rq->align : UMEM_PAGE, phys_max = rq->phys_max;
    for (unsigned d = 0; d < UMEM_DEVICES; ++d)
        if ((rq->device_mask >> d) & 1u && (!phys_max || u->device_limit[d] < phys_max)) phys_max = u->device_limit[d];
    if ((rq->device_mask & UMEM_DEV_NPU) && align < (16u << 20)) align = 16u << 20;   /* the alignment of a large NPU buffer */
    uint64_t best = 0, best_base = 0;
    lock(u);
    for (unsigned c = 0; c < UMEM_CELL_CLASSES; ++c)
        for (C *f = u->free_head[rq->policy][c]; f; f = f->e.next) {
            uint64_t s = align_up(f->e.base, align), end = f->e.base + f->e.bytes;
            if (phys_max && end > phys_max) end = phys_max;
            if (s >= end) continue;
            if (rq->window) {
                uint64_t w_end = (s & ~(rq->window - 1)) + rq->window;
                if (end > w_end) end = w_end;
            }
            if (end - s > best) { best = end - s; best_base = s; }
        }
    unlock(u);
    best &= ~(UMEM_PAGE - 1);
    if (!best) return UMEM_NO_SPACE;
    *base = best_base; *bytes = best;
    return UMEM_OK;
}

int umem_lookup(struct umem *u, uint64_t base, struct umem_buf *out)
{
    lock(u);
    C *c = tree_floor(u, base);
    int r = c && c->e.base == base && !(c->e.flags & E_FREE) ? UMEM_OK : UMEM_NOT_OWNED;
    if (!r) *out = (struct umem_buf){c->e.base, c->e.bytes, c->e.generation, c};
    unlock(u);
    return r;
}

uint16_t umem_owner_of(struct umem *u, uint64_t pa)
{
    lock(u);
    C *c = tree_floor(u, pa);
    uint16_t owner = c && pa - c->e.base < c->e.bytes && !(c->e.flags & E_FREE) ? c->e.owner : 0;
    unlock(u);
    return owner;
}

static unsigned walk(C *t, uint16_t owner, struct umem_extent *out, unsigned capacity, unsigned n)
{
    if (!t) return n;
    n = walk(t->e.left, owner, out, capacity, n);
    if (!(t->e.flags & E_FREE) && t->e.owner == owner) {
        if (n < capacity) out[n] = (struct umem_extent){t->e.base, t->e.bytes, t->e.owner, t->e.policy, t->e.access};
        ++n;
    }
    return walk(t->e.right, owner, out, capacity, n);
}
unsigned umem_extents_of(struct umem *u, uint16_t owner, struct umem_extent *out, unsigned capacity)
{
    lock(u);
    unsigned n = walk(u->root, owner, out, capacity, 0);
    unlock(u);
    return n;
}
struct visit { uint16_t owner; int (*fn)(void *, const struct umem_extent *); void *ctx; unsigned n; int stop; };
static void visit_walk(C *t, struct visit *v)
{
    if (!t || v->stop) return;
    visit_walk(t->e.left, v);
    if (!v->stop && !(t->e.flags & E_FREE) && t->e.owner == v->owner) {
        struct umem_extent e = {t->e.base, t->e.bytes, t->e.owner, t->e.policy, t->e.access};
        ++v->n;
        if (v->fn(v->ctx, &e)) v->stop = 1;
    }
    visit_walk(t->e.right, v);
}
unsigned umem_for_each(struct umem *u, uint16_t owner, int (*fn)(void *, const struct umem_extent *), void *ctx)
{
    struct visit v = {owner, fn, ctx, 0, 0};
    lock(u);
    visit_walk(u->root, &v);
    unlock(u);
    return v.n;
}
uint64_t umem_free_bytes(const struct umem *u) { return u->free_bytes; }

int umem_device_limit_set(struct umem *u, unsigned bit, uint64_t limit)
{
    if (!u || bit >= UMEM_DEVICES || limit < UMEM_GIB) return UMEM_INVALID;
    u->device_limit[bit] = limit;
    return UMEM_OK;
}

int umem_init(struct umem *u, const struct umem_hooks *h, void *cells, size_t cell_count,
              uint8_t *chunk_policy, uint64_t chunk_base, uint64_t chunk_count,
              const struct umem_range *ranges, unsigned range_count)
{
    if (!u || !h || !cells || cell_count < UMEM_MIN_CELLS || !chunk_policy || !ranges || !range_count ||
        (chunk_base & (UMEM_CHUNK - 1))) return UMEM_INVALID;
    uint64_t prev_end = 0;
    for (unsigned i = 0; i < range_count; ++i) {
        const struct umem_range *r = &ranges[i];
        if (!r->bytes || (r->offset & (UMEM_CHUNK - 1)) || (r->bytes & (UMEM_CHUNK - 1)) || r->offset < prev_end ||
            r->offset < chunk_base || r->offset + r->bytes > chunk_base + chunk_count * UMEM_CHUNK) return UMEM_INVALID;
        prev_end = r->offset + r->bytes;
    }
    memset(u, 0, sizeof *u);
    u->hooks = *h;
    u->chunk_policy = chunk_policy; u->chunk_base = chunk_base; u->chunk_count = chunk_count;
    memset(chunk_policy, 0, (size_t)chunk_count);
    for (unsigned d = 0; d < UMEM_DEVICES; ++d) u->device_limit[d] = UMEM_GIB * 4;
    C *pool = cells;
    for (size_t i = cell_count; i-- > 0;) cell_put(u, &pool[i]);
    for (unsigned i = 0; i < range_count; ++i) {
        C *f = cell_get(u);
        f->e.kind = KIND_EXTENT; f->e.base = ranges[i].offset; f->e.bytes = ranges[i].bytes;
        f->e.policy = UMEM_WB; f->e.flags = E_FREE; f->e.prio = next_prio(u);
        tree_insert(u, f);
        list_add(u, f);
        u->free_bytes += f->e.bytes;
        u->usable_bytes += f->e.bytes;
    }
    return UMEM_OK;
}

/* ---- the consistency walk ---- */
struct check_state { const struct umem *u; const C *prev; uint64_t free_bytes, total, free_cells, cells; int code; };

static void check_tree(struct check_state *s, const C *t, uint32_t parent_prio)
{
    if (!t || s->code) return;
    if (t->e.kind != KIND_EXTENT) { s->code = 1; return; }
    if (t->e.prio > parent_prio) { s->code = 2; return; }
    check_tree(s, t->e.left, t->e.prio);
    if (s->code) return;
    const C *p = s->prev;
    if ((t->e.base & (UMEM_PAGE - 1)) || (t->e.bytes & (UMEM_PAGE - 1)) || !t->e.bytes) { s->code = 3; return; }
    if (p) {
        if (p->e.base + p->e.bytes > t->e.base) { s->code = 4; return; }
        if (p->e.base + p->e.bytes == t->e.base && (p->e.flags & E_FREE) && (t->e.flags & E_FREE) &&
            p->e.policy == t->e.policy) { s->code = 5; return; }
    }
    uint64_t first;
    if (!chunk_index(s->u, t->e.base, &first)) { s->code = 6; return; }
    for (uint64_t a = t->e.base & ~(UMEM_CHUNK - 1); a < t->e.base + t->e.bytes; a += UMEM_CHUNK) {
        uint64_t i;
        if (!chunk_index(s->u, a, &i)) { s->code = 6; return; }
        if (s->u->chunk_policy[i] != t->e.policy) { s->code = 7; return; }
    }
    s->total += t->e.bytes;
    ++s->cells;
    if (t->e.flags & E_FREE) {
        s->free_bytes += t->e.bytes;
        ++s->free_cells;
        if (t->e.owner || t->e.generation) { s->code = 8; return; }
    } else if (!t->e.owner || !t->e.generation) { s->code = 9; return; }
    s->prev = t;
    check_tree(s, t->e.right, t->e.prio);
}

int umem_check(struct umem *u)
{
    struct check_state s = {u, 0, 0, 0, 0, 0, 0};
    check_tree(&s, u->root, UINT32_MAX);
    if (s.code) return s.code;
    if (s.free_bytes != u->free_bytes) return 10;
    if (s.total != u->usable_bytes) return 11;
    uint64_t listed = 0, spares = 0;
    for (unsigned p = 0; p < 2; ++p)
        for (unsigned c = 0; c < UMEM_CELL_CLASSES; ++c) {
            const C *prev = 0;
            for (const C *f = u->free_head[p][c]; f; prev = f, f = f->e.next) {
                if (!(f->e.flags & E_FREE) || f->e.policy != p || size_class(f->e.bytes / UMEM_PAGE) != c || f->e.prev != prev) return 12;
                if (tree_floor_const(u, f->e.base) != f) return 13;
                ++listed;
            }
        }
    if (listed != s.free_cells) return 14;
    for (const C *c = u->spare; c; c = c->e.next) ++spares;
    if (spares != u->spare_count) return 15;
    return 0;
}
