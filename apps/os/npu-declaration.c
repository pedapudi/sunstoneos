/* The admission of an application's NPU declaration against what the image links and holds. It is its own unit because the
 * EL3 admission (apps/os/admission.c) calls it, and the host pre-flight (tools/host_admission.py) builds the same code with
 * the image's definitions. The image's data it reads (os_npu_model_count, ss_npu_queue_edges) comes from the linked image, or,
 * on the host, from the image's ELF. */
#include "os.h"
#include "npu.h"
#include "npu_declaration.h"
#include "sunstone_crumb.h"
#include "sunstone_npu_queue.h"
#include "../board/application.h"
/* What the image uses of the NPU: one slot for the synchronous wrapper, one for each graph under the queue. */
static struct npu_image_needs image_needs(void)
{
    return (struct npu_image_needs){
#ifdef SUNSTONE_A733_OS_NPU_QUEUE
        os_npu_model_count,os_npu_model_count,ss_npu_queue_edges.count
#else
        1,os_npu_model_count,0
#endif
    };
}
/* The tables this image holds (fixed when it is built). */
static struct npu_tables image_tables(void)
{
    /* Slots, graphs and links have no table limit: their tables come from the reservation (memory bounds them). The ring's
     * depth is the one bounded structure. */
    return (struct npu_tables){UINT32_MAX,UINT32_MAX,SS_NPU_QUEUE_MAX_DEPTH,UINT32_MAX};
}
struct npu_declaration os_npu_declaration_of(const struct os_npu_session *n)
{
    return (struct npu_declaration){n->slots,n->in_flight,n->graphs,n->links,n->graph_bytes,n->tensor_bytes};
}
/* A refused declaration leaves its reason in the first-failure record, as the value of a record in group NPU. */
static enum npu_declaration_result refused_declaration(enum npu_declaration_result r)
{
    board_fail_note(SUNSTONE_CRUMB_FAIL_SITE(NPU),(uint32_t)r,SUNSTONE_CRUMB_HERE());
    return r;
}
/* A session that declares nothing is refused (NPU_DECLARATION_MISSING): there is no default. */
enum npu_declaration_result os_npu_declaration_admitted(const struct os_npu_session *n)
{
    const struct npu_declaration d=os_npu_declaration_of(n);
    const struct npu_image_needs need=image_needs();
    const struct npu_tables tables=image_tables();
    const enum npu_declaration_result r=npu_declaration_check(&d,&need,&tables);
#ifndef SUNSTONE_A733_OS_NPU_QUEUE
    /* The synchronous wrapper has one client task, so exactly one slot. */
    if(r==NPU_DECLARATION_OK&&d.slots!=1)return refused_declaration(NPU_DECLARATION_SLOTS);
#endif
    return r==NPU_DECLARATION_OK?r:refused_declaration(r);
}
