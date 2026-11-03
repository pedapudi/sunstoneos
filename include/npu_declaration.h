#ifndef SUNSTONE_NPU_DECLARATION_H
#define SUNSTONE_NPU_DECLARATION_H
#include <stddef.h>
#include <stdint.h>
/* What an application declares of the NPU (docs/designs/npu-resources.md), as the session record carries it
 * (apps/os/npu-session.h, version 2): four full-width fields. The operating system admits the declaration against the
 * capacity of the image and of the board, and reserves the memory it implies, before the application runs. */
struct npu_declaration { uint32_t slots, in_flight, graphs, links, graph_bytes, tensor_bytes; };
/* What the image itself links: slots in use, graphs and tensor links. A declaration must cover them. */
struct npu_image_needs { uint32_t slots, graphs, links; };
/* What the image's tables hold. Slots, graphs and links have no table: the runtime takes those tables from the reservation
 * (docs/designs/npu-resources.md), so an image states UINT32_MAX for them. The in-flight depth is the ring of the queue, the
 * one bounded structure: a bounded ring is backpressure, and the image is built for the depth it declares. */
struct npu_tables { uint32_t slots, graphs, in_flight, links; };
enum npu_declaration_result {
    NPU_DECLARATION_OK,
    NPU_DECLARATION_MISSING,      /* the session record declares nothing (every field zero) */
    NPU_DECLARATION_SLOTS,        /* zero */
    NPU_DECLARATION_DEPTH,        /* zero */
    NPU_DECLARATION_GRAPHS,       /* zero */
    NPU_DECLARATION_IMAGE_SLOTS,  /* fewer slots than the image uses */
    NPU_DECLARATION_IMAGE_GRAPHS, /* fewer graphs than the image links */
    NPU_DECLARATION_IMAGE_LINKS,  /* fewer links than the image declares */
    NPU_DECLARATION_TABLES,       /* more than the image's tables hold */
    NPU_DECLARATION_MODEL,        /* a linked graph is not admissible */
    NPU_DECLARATION_DEVICE_SPACE, /* the regions do not fit the NPU's 32-bit address space */
    NPU_DECLARATION_MEMORY        /* the reservation, with the tables of the declared slots, graphs and links, exceeds the pool */
};
/* The cheap checks: ranges, the declaration against what the image links, and against the image's tables. */
enum npu_declaration_result npu_declaration_check(const struct npu_declaration *, const struct npu_image_needs *,
                                                  const struct npu_tables *);
const char *npu_declaration_name(enum npu_declaration_result);
#endif
