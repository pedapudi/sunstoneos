#include "uart_session.h"
#include "a733_pins.h"
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
int uart_session_valid(const struct uart_session *s)
{
 if(!s||!a733_board_session_valid(&s->board,0)||s->board.workload!=2||
 s->magic!=A733_UART_SESSION_MAGIC||s->version!=1||s->bytes!=sizeof(*s)||!s->owner||!s->authority||
 s->owner==s->board.pmic_owner||s->owner==s->board.usb0_owner||
 s->operation_timeout_us<250000||s->operation_timeout_us>10000000||
 !s->tx_bytes||s->tx_bytes>32||s->rx_bytes!=s->tx_bytes||
 !present(s->initial,32)||!present(s->electrical,32)||!present(s->clocks,32)||!present(s->supervision,32))return 0;
 for(unsigned i=0;i<2;++i){
  struct a733_pin_config e={s->expected[i][0],s->expected[i][1],s->expected[i][2]};
  struct a733_pin_config d={s->desired[i][0],s->desired[i][1],s->desired[i][2]};
  if(!a733_pin_config_valid_banked(A733_PIN_B,i,e)||!a733_pin_config_valid_banked(A733_PIN_B,i,d)||
     (e.function!=0&&e.function!=15)||d.function!=2)return 0;
 }
 for(unsigned i=2;i<4;++i)for(unsigned j=0;j<3;++j)if(s->expected[i][j]||s->desired[i][j])return 0;
 for(unsigned i=s->tx_bytes;i<sizeof(s->tx);++i)if(s->tx[i])return 0;
 return 1;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
 const struct uart_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
 if(!c||!uart_session_valid(s)||c->magic!=A733_UART_CONTRACT_MAGIC||c->reserved[1]!=sizeof(*s))return 0;
 uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);if(memcmp(hash,&c->reserved[2],32))return 0;
 struct a733_smp_contract base=*c;base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
 storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
 return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
