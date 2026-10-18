#include "thermal_session.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
int thermal_session_valid(const struct thermal_session *s)
{
 if(!s||!a733_board_session_valid(&s->board,0)||s->board.workload!=2||
 s->magic!=A733_THERMAL_SESSION_MAGIC||s->version!=1||s->bytes!=sizeof(*s)||
 !s->owner||s->owner==s->board.pmic_owner||s->owner==s->board.usb0_owner||!s->authority||
 !present(s->initial,32)||!present(s->electrical,32)||!present(s->clocks,32)||!present(s->supervision,32)||
 s->configure_adc_clock>1||s->alarm_mask!=31||s->critical_mask!=31||s->reserved0||present(s->reserved,6)||
 (s->apb1&~UINT32_C(0x0300001f)))return 0;
 unsigned parent=s->apb1>>24,div=(s->apb1&31)+1;
 uint32_t hz=parent==0?24000000u:parent==3?600000000u:0;
 if(!hz||hz%div||s->apb1_hz!=hz/div)return 0;
 for(unsigned i=0;i<5;++i)if(!s->limits[i][0]||s->limits[i][0]>=s->limits[i][1]||s->limits[i][1]>4095||
 !s->limits[i][2]||s->limits[i][2]>s->limits[i][0])return 0;
 return 1;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
 const struct thermal_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
 if(!c||!thermal_session_valid(s)||c->magic!=A733_THERMAL_CONTRACT_MAGIC||c->reserved[1]!=sizeof(*s))return 0;
 uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);if(memcmp(hash,&c->reserved[2],32))return 0;
 struct a733_smp_contract base=*c;base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
 storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
 return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
