#include "dsi-session.h"
#include "a733_storage_digest.h"
#include <string.h>
/* Retained EL1-readable record, separately cleared by the existing board branch.
 * No EL3 cache is enabled while this snapshot is copied. */
static struct {struct dsi_session value;unsigned attempted,ready;} sealed __attribute__((section(".board_runtime_retained")));
int dsi_session_prepare(const struct a733_smp_contract *c)
{
 if(sealed.attempted)return 0;sealed.attempted=1;
 if(!a733_smp_contract_valid(c,a733_smp_counter()))return 0;
 const volatile uint8_t *source=(const void *)A733_BOARD_SESSION_ADDRESS;
 for(unsigned i=0;i<sizeof(sealed.value);++i)((uint8_t *)&sealed.value)[i]=source[i];
 uint8_t digest[32];storage_digest_bytes(&sealed.value,sizeof(sealed.value),digest);
 const uint8_t *bound=dsi_contract_session(c);
 if(!bound||memcmp(digest,bound,32)||!dsi_profile_valid(&sealed.value)||
 !a733_board_session_valid(&sealed.value.board,a733_smp_counter()))return 0;
 sealed.ready=1;__asm__ volatile("dsb sy":::"memory");return 1;
}
const struct a733_board_session *board_session_snapshot(void){return sealed.ready?&sealed.value.board:0;}
const struct dsi_session *dsi_admitted_session(void){return sealed.ready?&sealed.value:0;}
