#include "pcie_session.h"
#include "a733_pins.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
/* Startup must cover the 100 ms PERST hold after PHY completion and the 1 s
 * endpoint readiness window after release; 400 ms remain for earlier phases. */
#define PCIE_LINK_TIMEOUT_MIN_US 1500000u
int pcie_session_valid(const struct pcie_session *s)
{
 if(!s||!a733_board_session_valid(&s->board,0)||s->board.workload!=2||
 s->magic!=A733_PCIE_SESSION_MAGIC||s->version!=1||s->bytes!=sizeof(*s)||!s->owner||!s->authority||
 s->owner==s->board.pmic_owner||s->owner==s->board.usb0_owner||s->pck_client>=16||
 s->link_timeout_us<PCIE_LINK_TIMEOUT_MIN_US||s->link_timeout_us>10000000||!s->vendor||s->vendor==65535||
 !s->device||s->device==65535||s->command_timeout_us<1000||s->command_timeout_us>5000000||
 s->function>7||!s->namespace_id||s->namespace_id==UINT32_MAX||!present(s->nvme,32)||
 !present(s->initial,32)||!present(s->electrical,32)||!present(s->clocks,32)||!present(s->supervision,32))return 0;
 for(unsigned i=0;i<4;++i){
  struct a733_pin_config p={s->inherited[i][0],s->inherited[i][1],s->inherited[i][2]};
  if(!a733_pin_config_valid_banked(A733_PIN_D,20+i,p)||
  (i==2?p.function!=1:p.function!=0&&p.function!=15))return 0;
 }
 return 1;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
 const struct pcie_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
 if(!c||!pcie_session_valid(s)||c->magic!=A733_PCIE_CONTRACT_MAGIC||c->reserved[1]!=sizeof(*s))return 0;
 uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);if(memcmp(hash,&c->reserved[2],32))return 0;
 struct a733_smp_contract base=*c;base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
 storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
 return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
