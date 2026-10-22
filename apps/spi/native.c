#include "spi.h"
#include "irq.h"
#include "boot.h"
#include "a733_mapping_check.h"
#include "a733_range.h"
#include "a733_storage_digest.h"
#include <string.h>
struct spi_application spi;
int spi_live(void)
{return board_live(NULL)&&!atomic_load_explicit(&spi.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.locks.retained,memory_order_acquire)&&
 a733_smp_counter()/24<board.session.deadline_us-10000;}
void spi_cancel(void)
{spi_irq_stop();atomic_store_explicit(&spi.failed,1,memory_order_release);atomic_store_explicit(&spi.result,2,memory_order_release);}
int spi_output_ready(void)
{return spi_live()&&atomic_load_explicit(&spi.result,memory_order_acquire)==1;}
static const struct a733_mapping_rule device_rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
static int mappings(void)
{
 const uintptr_t pages[]={0x02000000,0x02002000,0x02543000,0x03400000,0x07090000};
 for(unsigned i=0;i<sizeof(pages)/sizeof(*pages);++i)
  if(!a733_mapping_check(aarch64_device_translate,pages[i],pages[i],&device_rule))return 0;
 return 1;
}
static int admitted(void *v,const struct a733_spi_platform_config *c,
 const struct a733_clock_token *token,enum a733_spi_platform_admission phase)
{
 return v==&spi&&spi_live()&&phase==A733_SPI_PLATFORM_START&&c==&spi.platform.config&&
 token==&spi.platform.clock&&token->id&&token->owner==spi.session.owner&&
 token->exclusive==A733_CLOCK_SPI3&&token->shared==(A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC)&&
 c->owner==spi.config.owner&&c->handoff==spi.config.handoff&&c->deadline==spi.config.deadline&&
 c->route==A733_SPI3_PE&&c->format.port==3&&!c->format.cs&&!c->format.mode&&!c->format.lsb_first&&
 !c->format.active_high&&c->format.max_hz==500000&&c->format.setup_us==2&&c->format.hold_us==2&&
 !memcmp(c->expected,spi.config.expected,sizeof(c->expected))&&
 !memcmp(c->desired,spi.config.desired,sizeof(c->desired))&&mappings();
}
static int now(void *v,uint64_t *n)
{return v==&spi&&spi_live()&&spi.native.operations.now(&spi.native,n);}
static int current(void *v)
{return v==&spi&&spi_live()&&spi.native.operations.current(&spi.native);}
static int authorize(void *v,const struct a733_spi_platform_config *c,
 const struct a733_clock_token *t,enum a733_spi_platform_admission phase)
{return v==&spi&&spi_live()?spi.native.operations.authorize(&spi.native,c,t,phase):-1;}
static int enter(void *v,uint64_t d)
{return v==&spi&&spi_live()?spi.native.operations.enter(&spi.native,d):-1;}
static void leave(void *v)
{if(v==&spi)spi.native.operations.leave(&spi.native);}
static void retain(void *v)
{if(v==&spi)spi.native.operations.retain(&spi.native);}
static int read_register(void *v,uintptr_t a,uint32_t *out)
{return v==&spi&&spi_live()&&spi.native.operations.read(&spi.native,a,out)==1&&spi_live();}
static int write_register(void *v,uintptr_t a,uint32_t x)
{return v==&spi&&spi_live()&&spi.native.operations.write(&spi.native,a,x)==1&&spi_live();}
static int read_byte(void *v,uintptr_t a,uint8_t *out)
{return v==&spi&&spi_live()&&spi.native.operations.read8(&spi.native,a,out)==1&&spi_live();}
static int write_byte(void *v,uintptr_t a,uint8_t x)
{return v==&spi&&spi_live()&&spi.native.operations.write8(&spi.native,a,x)==1&&spi_live();}
int spi_construct(void)
{
 if(!board_live(NULL)||board.session.deadline_us<=10000||!spi.handle||!mappings())return 0;
 uint64_t start=board.session.session_start_ticks/24,end;
 if(!a733_deadline_after(start,spi.session.operation_timeout_us,&end)||end>=board.session.deadline_us-10000)return 0;
 atomic_init(&spi.failed,0);atomic_init(&spi.result,0);spi.deadline=end;
 spi.config=(struct a733_spi_platform_config){.owner=spi.session.owner,.handoff=spi.session.authority,
 .deadline=spi.deadline,.route=A733_SPI3_PE,.format={3,0,0,0,0,500000,2,2,0,0}};
 for(unsigned i=0;i<4;++i){
  spi.config.expected[i]=(struct a733_pin_config){spi.session.expected[i][0],spi.session.expected[i][1],spi.session.expected[i][2]};
  spi.config.desired[i]=(struct a733_pin_config){spi.session.desired[i][0],spi.session.desired[i][1],spi.session.desired[i][2]};}
 if(!a733_spi_platform_native_init(&spi.native,&providers.shared.locks,spi.handle,3,admitted,&spi))return 0;
 spi.operations=(struct a733_spi_platform_ops){now,current,authorize,enter,leave,retain,read_register,write_register,read_byte,write_byte};
 memcpy(spi.tx,spi.session.tx,sizeof(spi.tx));
 spi.transfer=(struct a733_spi_transfer){A733_SPI_WRITE_READ,spi.tx,spi.rx,spi.session.tx_bytes,spi.session.rx_bytes,0,0,0};
 storage_digest_bytes(&spi.session,sizeof(spi.session),spi.session_digest);
 return a733_spi_platform_init(&spi.platform,&spi.config,&spi.operations,&spi,&providers.shared.clocks,&providers.shared.pins);
}
