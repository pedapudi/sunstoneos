#include "adc_session.h"
#include "a733_pins.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
int adc_session_valid(const struct adc_session *s)
{
 if(!s||!a733_board_session_valid(&s->board,0)||s->board.workload!=2||
 s->magic!=A733_ADC_SESSION_MAGIC||(s->version!=1&&s->version!=2&&s->version!=3)||s->bytes!=sizeof(*s)||!s->owner||!s->authority||
 s->owner==s->board.pmic_owner||s->owner==s->board.usb0_owner||
 s->configure_adc_clock>1||(s->version==2&&s->configure_adc_clock)||s->cldo3_selector!=13||s->cldo3_uv!=1800000||
 !s->source_impedance_ohms||s->source_impedance_ohms>1000000||
 !present(s->initial,32)||!present(s->electrical,32)||!present(s->clocks,32)||!present(s->supervision,32))return 0;
 if(s->apb1&~0x0300001fu)return 0;
 unsigned parent=s->apb1>>24,div=(s->apb1&31)+1;uint32_t hz=parent==0?24000000u:parent==3?600000000u:0;
 if(!hz||hz%div||s->apb1_hz!=hz/div)return 0;
 if(s->version==3){
  if(!s->reserved[0]||(s->reserved[0]&~15u)||s->reserved[1]||
   adc_window_low(s)>=adc_window_high(s)||adc_window_high(s)>4095)return 0;
 }else for(unsigned i=0;i<sizeof(s->reserved);++i)if(s->reserved[i])return 0;
 return 1;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
 const struct adc_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
 if(!c||!adc_session_valid(s)||c->magic!=A733_ADC_CONTRACT_MAGIC||c->reserved[1]!=sizeof(*s))return 0;
 uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);if(memcmp(hash,&c->reserved[2],32))return 0;
 struct a733_smp_contract base=*c;base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
 storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
 return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
