#include "spi_session.h"
#include "a733_pins.h"
#ifdef SUNSTONE_SPI_DMA
#include "a733_dma_platform.h"
#endif
#define a733_smp_contract_valid board_contract_unused
#include "../board/admission.c"
#undef a733_smp_contract_valid
int spi_session_valid(const struct spi_session *s)
{
 if(!s||!a733_board_session_valid(&s->board,0)||s->board.workload!=2||
 s->magic!=A733_SPI_SESSION_MAGIC||!SPI_SESSION_VALID_VERSION(s->version)||s->bytes!=sizeof(*s)||!s->owner||!s->authority||
 s->owner==s->board.pmic_owner||s->owner==s->board.usb0_owner||
 s->operation_timeout_us<250000||s->operation_timeout_us>10000000||
 !s->tx_bytes||s->tx_bytes>16||
#ifdef SUNSTONE_SPI_DMA
 s->rx_bytes!=4096||
#else
 !s->rx_bytes||s->rx_bytes>32||
#endif
 !present(s->initial,32)||!present(s->electrical,32)||!present(s->clocks,32)||!present(s->supervision,32))return 0;
 const unsigned pins[]={1,2,3,0};
 for(unsigned i=0;i<4;++i){
  struct a733_pin_config e={s->expected[i][0],s->expected[i][1],s->expected[i][2]};
  struct a733_pin_config d={s->desired[i][0],s->desired[i][1],s->desired[i][2]};
  if(!a733_pin_config_valid_banked(A733_PIN_E,pins[i],e)||
     !a733_pin_config_valid_banked(A733_PIN_E,pins[i],d)||
     (e.function!=0&&e.function!=15)||d.function!=5)return 0;
 }
 for(unsigned i=s->tx_bytes;i<sizeof(s->tx);++i)if(s->tx[i])return 0;
#ifdef SUNSTONE_SPI_DMA
 const struct spi_dma_authority *d=&s->dma;
 if(!d->owner||d->owner==s->owner||d->owner==s->board.pmic_owner||d->owner==s->board.usb0_owner||
 !d->handoff||d->evidence!=d->handoff||d->controller!=0x04601000||d->established!=0x5f||
 d->owned_channels!=0xffff||d->service_channels!=3||d->cold>1||
 d->startup_timeout_us<250000||d->startup_timeout_us>s->operation_timeout_us||!present(d->authority,32))return 0;
 struct a733_dma_platform_config c={.owner=d->owner,.handoff=d->handoff,.deadline_us=d->startup_timeout_us,
 .expected_cpu=d->expected_cpu,.expected_mcu=d->expected_mcu,.desired_cpu=d->desired_cpu,
 .desired_mcu=d->desired_mcu,.route_change_mask=d->route_change_mask,.cold=d->cold};
 for(unsigned i=0;i<3;++i){
  if(d->fabric[i]!=s->board.fabric[i]||d->rate_hz[i]!=s->board.fabric_hz[i])return 0;
  c.fabric[i]=d->fabric[i];c.rate_hz[i]=d->rate_hz[i];
 }
 a733_clock_mask_t shared;
 if((d->service_channels&~d->desired_cpu)||!a733_dma_platform_scopes(&c,&shared))return 0;
#endif
 return 1;
}
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
 const struct spi_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
 if(!c||!spi_session_valid(s)||c->magic!=A733_SPI_CONTRACT_MAGIC||c->reserved[1]!=sizeof(*s))return 0;
 uint8_t hash[32];storage_digest_bytes(s,sizeof(*s),hash);if(memcmp(hash,&c->reserved[2],32))return 0;
 struct a733_smp_contract base=*c;base.magic=A733_BOARD_CONTRACT_MAGIC;base.reserved[1]=sizeof(s->board);
 storage_digest_bytes(&s->board,sizeof(s->board),(void *)&base.reserved[2]);
 return a733_board_contract_valid(&base,&s->board,a733_smp_build_id,now);
}
