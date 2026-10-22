#include "uart.h"
#include "boot.h"
#include "a733_mapping_check.h"
#include "a733_range.h"
#include "a733_storage_digest.h"
#include <string.h>
struct uart_application uart;
int uart_live(void)
{return board_live(NULL)&&!atomic_load_explicit(&uart.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.locks.retained,memory_order_acquire)&&
 a733_smp_counter()/24<board.session.deadline_us-10000;}
int uart_output_ready(void)
{return uart_live()&&atomic_load_explicit(&uart.result,memory_order_acquire)==1;}
static const struct a733_mapping_rule device_rule={A733_MAPPING_WRITE,1,{{A733_MAIR_DEVICE_NGNRNE,A733_SH_OUTER}},0,1};
static int mappings(void)
{
 const uintptr_t pages[]={0x02002000,0x02502000,0x03400000,0x07090000};
 for(unsigned i=0;i<sizeof(pages)/sizeof(*pages);++i)
  if(!a733_mapping_check(aarch64_device_translate,pages[i],pages[i],&device_rule))return 0;
 return 1;
}
static int admitted(void *v,const struct a733_uart_platform_config *c,
 const struct a733_clock_token *token,enum a733_uart_admission phase)
{
 /* The admitted peer is a passive TX-to-RX wire. It has no independent
  * transmitter; stopped submissions and consumed echo establish STOP quiet. */
 return v==&uart&&uart_live()&&c==&uart.platform.config&&token==&uart.platform.clock&&
 token->id&&token->owner==uart.session.owner&&token->exclusive==A733_CLOCK_UART2&&
 token->shared==(A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_APB_UART)&&
 !memcmp(c,&uart.config,sizeof(*c))&&mappings()&&
 (phase==A733_UART_ADMIT_START||(phase==A733_UART_ADMIT_STOP&&uart.stage==3&&
 uart.sent==uart.session.tx_bytes&&uart.received==uart.session.rx_bytes));
}
/* Metadata retention must remain callable after health loss. Every operation
 * that can reach MMIO separately authenticates the live board lifetime. */
static int current(void *v)
{return v==&uart&&uart.native.operations.current(&uart.native);}
static int now(void *v,uint64_t *n)
{return v==&uart&&uart_live()&&uart.native.operations.now_us(&uart.native,n)&&uart_live();}
static int authorize(void *v,const struct a733_uart_platform_config *c,
 const struct a733_clock_token *t,enum a733_uart_admission phase)
{return v==&uart&&uart_live()?uart.native.operations.authorize(&uart.native,c,t,phase):-1;}
static int enter(void *v,uint64_t deadline)
{return v==&uart&&uart_live()?uart.native.operations.enter(&uart.native,deadline):-1;}
static void leave(void *v)
{if(v==&uart)uart.native.operations.leave(&uart.native);}
static void retain(void *v)
{if(v==&uart)uart.native.operations.retain(&uart.native);}
static int read_register(void *v,uintptr_t a,uint32_t *out)
{return v==&uart&&uart_live()&&uart.native.operations.read(&uart.native,a,out)==1&&uart_live();}
static int write_register(void *v,uintptr_t a,uint32_t value)
{return v==&uart&&uart_live()&&uart.native.operations.write(&uart.native,a,value)==1&&uart_live();}
int uart_construct(void)
{
 if(!board_live(NULL)||board.session.deadline_us<=10000||!uart.handle||!mappings())return 0;
 uint64_t start=board.session.session_start_ticks/24,end;
 if(!a733_deadline_after(start,uart.session.operation_timeout_us,&end)||end>=board.session.deadline_us-10000)return 0;
 atomic_init(&uart.failed,0);atomic_init(&uart.result,0);
 atomic_init(&uart.pending,0);atomic_init(&uart.irq_fault,0);atomic_init(&uart.notifications,0);
 uart.irq=(struct gicv3_spi){0x03400000,36};uart.masked=1;uart.deadline=end;
 uart.config=(struct a733_uart_platform_config){.owner=uart.session.owner,.handoff=uart.session.authority,
 .startup_deadline_us=uart.deadline,.route=A733_UART2_PB,.deferred_irq=
#ifdef SUNSTONE_UART_DMA
 0,
#else
 1,
#endif
.format={2,8,2,0,0,24000000,115200,2000}};
 for(unsigned i=0;i<2;++i){
  uart.config.expected[i]=(struct a733_pin_config){uart.session.expected[i][0],uart.session.expected[i][1],uart.session.expected[i][2]};
  uart.config.desired[i]=(struct a733_pin_config){uart.session.desired[i][0],uart.session.desired[i][1],uart.session.desired[i][2]};}
 if(!a733_uart_platform_native_init(&uart.native,&providers.shared.locks,uart.handle,2,admitted,&uart))return 0;
 memcpy(uart.tx,uart.session.tx,sizeof(uart.tx));
 storage_digest_bytes(&uart.session,sizeof(uart.session),uart.session_digest);
 uart.operations=(struct a733_uart_platform_ops){now,current,authorize,enter,leave,retain,read_register,write_register};
 if(!a733_uart_platform_init(&uart.platform,&uart.config,&uart.operations,&uart,
 &providers.shared.clocks,&providers.shared.pins))return 0;
#ifdef SUNSTONE_UART_DMA
 return uart_dma_construct();
#else
 return 1;
#endif
}
