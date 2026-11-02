#include "npu_declaration.h"
#include "npu_runtime.h"
#include "sunstone_npu_queue.h"
/* Limits of the current arrays; each has a source in docs/designs/npu-resources.md. */
#define MAX_LINKS (SS_NPU_QUEUE_MAX_HANDLES * SS_NPU_QUEUE_MAX_TENSORS)
uint32_t npu_declaration_pack(const struct npu_declaration *d)
{
    return (d->slots & 255u) | (d->in_flight & 255u) << 8 | (d->graphs & 255u) << 16 | (d->links & 255u) << 24;
}
struct npu_declaration npu_declaration_unpack(uint32_t w)
{
    return (struct npu_declaration){w & 255u, w >> 8 & 255u, w >> 16 & 255u, w >> 24 & 255u};
}
enum npu_declaration_result npu_declaration_check(const struct npu_declaration *d, const struct npu_image_needs *n)
{
    if (!d->slots || d->slots > NPU_RUNTIME_SLOTS) return NPU_DECLARATION_SLOTS;
    if (!d->in_flight || d->in_flight > SS_NPU_QUEUE_MAX_DEPTH) return NPU_DECLARATION_DEPTH;
    if (!d->graphs || d->graphs > NPU_RUNTIME_MODELS) return NPU_DECLARATION_GRAPHS;
    if (d->links > MAX_LINKS) return NPU_DECLARATION_LINKS; /* the packed field holds 255, below the edge table */
    if (d->slots < n->slots) return NPU_DECLARATION_IMAGE_SLOTS;
    if (d->graphs < n->graphs) return NPU_DECLARATION_IMAGE_GRAPHS;
    if (d->links < n->links) return NPU_DECLARATION_IMAGE_LINKS;
    return NPU_DECLARATION_OK;
}
const char *npu_declaration_name(enum npu_declaration_result r)
{
    static const char *const names[] = {"ok", "slots", "queue depth", "graphs", "links", "image slots", "image graphs",
                                        "image links", "model", "device address space", "memory"};
    return (unsigned)r < sizeof(names) / sizeof(*names) ? names[r] : "unknown";
}
