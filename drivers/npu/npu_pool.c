#include "npu_pool.h"
#include <string.h>

static void *window_allocate(struct npu_pool *p, size_t bytes)
{
    struct npu_pool_window *w = (struct npu_pool_window *)p;
    bytes = (bytes + 63) & ~(size_t)63;
    if (bytes > w->limit - w->used) return NULL;
    void *block = w->base ? w->base + w->used : (void *)(uintptr_t)64; /* a counting window has no memory: any non-NULL token */
    if (w->base) memset(block, 0, bytes);
    w->used += bytes;
    return block;
}
static void window_release(struct npu_pool *p, void *block, size_t bytes)
{
    struct npu_pool_window *w = (struct npu_pool_window *)p;
    bytes = (bytes + 63) & ~(size_t)63;
    if (w->base && bytes <= w->used && (uint8_t *)block == w->base + w->used - bytes) w->used -= bytes;
    else if (!w->base && bytes <= w->used) w->used -= bytes;
}
void npu_pool_window_init(struct npu_pool_window *w, void *base, size_t bytes)
{
    w->pool.allocate = window_allocate; w->pool.release = window_release;
    w->base = base; w->used = 0; w->limit = bytes;
}

static void *meta_allocate(struct npu_meta *m, size_t bytes)
{
    struct npu_meta_window *w = (struct npu_meta_window *)m;
    bytes = (bytes + 63) & ~(size_t)63;
    if (bytes > w->limit - w->used) return NULL;
    void *block = w->base ? w->base + w->used : (void *)(uintptr_t)64;
    if (w->base) memset(block, 0, bytes);
    w->used += bytes;
    return block;
}
static void meta_release(struct npu_meta *m, void *block, size_t bytes)
{
    struct npu_meta_window *w = (struct npu_meta_window *)m;
    bytes = (bytes + 63) & ~(size_t)63;
    if (bytes <= w->used && (!w->base || (uint8_t *)block == w->base + w->used - bytes)) w->used -= bytes;
}
void npu_meta_window_init(struct npu_meta_window *w, void *base, size_t bytes)
{
    w->meta.allocate = meta_allocate; w->meta.release = meta_release;
    w->base = base; w->used = 0; w->limit = bytes;
}
