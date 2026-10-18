#include "storage_read_session.h"
#include "a733_pins.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
int storage_read_session_valid(const struct storage_read_session *s)
{
 if(!s||!a733_board_session_valid(&s->board,0)||s->board.workload!=2||
 s->magic!=A733_STORAGE_READ_SESSION_MAGIC||s->version!=1||s->bytes!=sizeof(*s)||
 !s->owner||s->owner==s->board.pmic_owner||s->owner==s->board.usb0_owner||!s->authority||
 !present(s->initial,32)||!present(s->memory,32)||!present(s->supplies,32)||
 !present(s->topology,32)||!present(s->supervision,32)||
 !s->identify_us||s->identify_us>10000000||!s->read_us||s->read_us>5000000||
 !s->voltage_mask||(s->voltage_mask&~UINT32_C(0x00300000))||s->reserved32)return 0;
 for(unsigned i=0;i<6;++i)if(s->reserved[i])return 0;
 for(unsigned i=0;i<7;++i){
  if((s->expected[i].function!=0&&s->expected[i].function!=15)||
   !a733_pin_config_valid_banked(A733_PIN_F,i,s->expected[i])||
   !a733_pin_config_valid_banked(A733_PIN_F,i,s->desired[i])||
   s->desired[i].function!=(i>=1&&i<=3?2:0)||(i!=2&&s->desired[i].pull!=1))return 0;
 }
 return 1;
}

int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
    const struct storage_read_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
    if(!c||!storage_read_session_valid(s)||c->magic!=A733_STORAGE_READ_CONTRACT_MAGIC||
       c->reserved[1]!=sizeof(*s))return 0;
    uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);
    if(memcmp(hash,&c->reserved[2],32))return 0;
    struct a733_smp_contract base=*c;
    base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
    storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
    return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
