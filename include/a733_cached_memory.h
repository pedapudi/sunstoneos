#ifndef SUNSTONE_A733_CACHED_MEMORY_H
#define SUNSTONE_A733_CACHED_MEMORY_H
#include <stdint.h>

extern uint64_t a733_cached_el3_l1[512], a733_cached_el3_l2[512], a733_cached_el3_l3[512];
extern uint64_t a733_cached_el1_l1[512], a733_cached_el1_l2[512];
/* Primary EL3, interrupts masked, both translation regimes and caches cold.
 * Establish the complete shared profiles before publication or secondary
 * release. No CPU accesses table pages after publication. External boot
 * admission must establish cold exclusive ownership and absence of aliases;
 * register checks cannot establish cache/interconnect or remote-user state.
 */
int a733_cached_memory_prepare(void);
#endif
