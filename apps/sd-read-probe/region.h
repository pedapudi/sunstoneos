#ifndef SUNSTONE_REGION_H
#define SUNSTONE_REGION_H
/* A large application region and the EL0 tests that exercise it (docs/designs/large-app-regions.md).
 * The code runs at EL1 with the MMU on, builds per-application address spaces with the real ledger
 * and builder, runs short EL0 routines in them, and reports what each access did. */
#include <stdint.h>
#include "app_region.h"
#include "probe.h"
#include "sunstone_probe.h"

struct region_el0 {                 /* layout used by el0.S */
    uint64_t entry, stack, a0, a1, a2, a3;                 /* in */
    uint64_t esr, far, elr, x0, saved_vbar, saved_daif;    /* out */
};
/* Run EL0 code with IRQs masked and the vectors of el0.S; returns the exception class that ended it
 * (0x15, an SVC, is a normal return) and fills the out fields. TTBR0 must already select the space. */
uint64_t region_el0_call(struct region_el0 *);
extern const char region_el0_code_start[], region_el0_code_end[];
extern const char region_el0_touch[], region_el0_stream[];

struct region_state {
    struct app_region_ledger ledger;
    struct app_table_arena arena;
    struct app_space owner, other, view;     /* the region's application, a second one, a 2 MiB-block view */
    uint64_t base_ttbr;                      /* the kernel's table, ASID 0 */
    uint64_t base, bytes;
    uint64_t code_window, stack_window;      /* physical blocks the EL0 routines use */
};

#define REGION_ASID_OWNER 1u
#define REGION_ASID_OTHER 2u
#define REGION_ASID_VIEW 3u
#define REGION_CODE_WINDOW UINT64_C(0x46000000)
#define REGION_STACK_WINDOW UINT64_C(0x46200000)

/* Build the ledger, the three spaces and the EL0 window. Fills r->status and the build fields. */
int region_prepare(struct region_state *, struct sdp_region *r, uint64_t bytes);
/* Isolation cases, the EL0/EL1 digest comparison and the read passes; kernel table installed on return. */
/* pass_cap, when nonzero, caps the bytes of each read pass (host fixtures only). */
int region_exercise(struct region_state *, struct sdp_region *r, uint64_t pass_cap);
#endif
