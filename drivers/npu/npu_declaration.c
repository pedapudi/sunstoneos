#include "npu_declaration.h"
enum npu_declaration_result npu_declaration_check(const struct npu_declaration *d, const struct npu_image_needs *n,
                                                  const struct npu_tables *t)
{
    if (!d->slots && !d->in_flight && !d->graphs && !d->links) return NPU_DECLARATION_MISSING;
    if (!d->slots || d->slots > NPU_MAX_SLOTS) return NPU_DECLARATION_SLOTS;
    if (!d->in_flight) return NPU_DECLARATION_DEPTH;
    if (!d->graphs) return NPU_DECLARATION_GRAPHS;
    if (d->slots < n->slots) return NPU_DECLARATION_IMAGE_SLOTS;
    if (d->graphs < n->graphs) return NPU_DECLARATION_IMAGE_GRAPHS;
    if (d->links < n->links) return NPU_DECLARATION_IMAGE_LINKS;
    if (d->slots > t->slots || d->graphs > t->graphs || d->in_flight > t->in_flight || d->links > t->links)
        return NPU_DECLARATION_TABLES;
    return NPU_DECLARATION_OK;
}
const char *npu_declaration_name(enum npu_declaration_result r)
{
    static const char *const names[] = {"ok", "declaration missing", "slots", "queue depth", "graphs", "image slots",
                                        "image graphs", "image links", "image tables", "model",
                                        "device address space", "memory"};
    return (unsigned)r < sizeof(names) / sizeof(*names) ? names[r] : "unknown";
}
