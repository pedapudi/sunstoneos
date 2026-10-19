#include "../board/identity.h"
#include "../board/session.h"
#include "a733_cpu_coherence.h"
#include "boot.h"
#include "assets.h"
/* All declarations are image-owned read-only constants. No mutable identity
 * depends on BSS lifetime. Cached start.S clears BSS before this hook;
 * the fixed external inputs are outside that clearing extent. */
int board_identity_prepare(uint64_t deadline)
{
    const struct a733_board_session *s=board_session_snapshot();
    uint64_t el,sctlr;
    __asm__ volatile("mrs %0, CurrentEL\n mrs %1, sctlr_el3":"=r"(el),"=r"(sctlr));
    if(el!=12||(sctlr&5)||!a733_board_session_valid(s,a733_smp_counter())||s->workload)return 0;
    for(unsigned i=0;i<3;++i)
        if(a733_cpu_coherence_publish(mimus_asset_address[i],mimus_asset_bytes[i],deadline)!=1)return 0;
    return 1;
}
/* Workload zero has no radio authority or SID access. */
const struct board_radio_chip_id *board_radio_chip_id(void){return 0;}
