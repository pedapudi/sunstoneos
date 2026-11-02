#ifndef SUNSTONE_NPU_DECLARATION_H
#define SUNSTONE_NPU_DECLARATION_H
#include <stddef.h>
#include <stdint.h>
/* What an application declares of the NPU (docs/designs/npu-resources.md), as the session record carries it: four
 * bytes in the NPU extent. The operating system admits the declaration against the capacity of the image and of the
 * board, and reserves the memory it implies, before the application runs. */
struct npu_declaration { unsigned slots, in_flight, graphs, links; };
/* What the image itself links: slots in use, graphs and tensor links. A declaration must cover them. */
struct npu_image_needs { unsigned slots, graphs, links; };
enum npu_declaration_result {
    NPU_DECLARATION_OK,
    NPU_DECLARATION_SLOTS,        /* zero, or beyond the runtime's slot array */
    NPU_DECLARATION_DEPTH,        /* zero, or beyond the queue's ring */
    NPU_DECLARATION_GRAPHS,       /* zero, or beyond the runtime's graph array */
    NPU_DECLARATION_LINKS,        /* beyond the queue's edge table */
    NPU_DECLARATION_IMAGE_SLOTS,  /* fewer slots than the image uses */
    NPU_DECLARATION_IMAGE_GRAPHS, /* fewer graphs than the image links */
    NPU_DECLARATION_IMAGE_LINKS,  /* fewer links than the image declares */
    NPU_DECLARATION_MODEL,        /* a linked graph is not admissible */
    NPU_DECLARATION_DEVICE_SPACE, /* the regions do not fit the NPU's 32-bit address space */
    NPU_DECLARATION_MEMORY        /* the reservation exceeds the pool */
};
uint32_t npu_declaration_pack(const struct npu_declaration *);
struct npu_declaration npu_declaration_unpack(uint32_t word);
/* The cheap checks: ranges against the arrays, and the declaration against what the image links. */
enum npu_declaration_result npu_declaration_check(const struct npu_declaration *, const struct npu_image_needs *);
const char *npu_declaration_name(enum npu_declaration_result);
#endif
