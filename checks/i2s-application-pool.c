/* Actual native singleton constructor and service initialization on QEMU.
 * No device operation occurs: scheduling starts only in physical application. */
#include "a733_dma_pool_session.h"
#include "a733_i2s_platform.h"
#include "platform.h"
_Noreturn void platform_exit_privileged(unsigned);
static struct a733_counter_clock clock_source;
static struct a733_clock_leases registry;
static struct a733_platform_locks locks;
static struct a733_dma_pool_session session;
static struct a733_i2s_platform binding;
static struct dma_service_i2s_stream_profile profiles[2];
#define profile profiles[0]
static _Noreturn void fatal(void *v){(void)v;platform_panic("unexpected device operation",1);}
static int no_now(void *v,uint64_t *out){(void)v;(void)out;return 0;}
static enum a733_i2s_resource_result no_health(void *v,uint64_t d)
{(void)v;(void)d;return A733_I2S_RESOURCE_RETAINED;}
static enum a733_i2s_resource_result no_acquire(void *v,const struct a733_i2s_plan *p,uint64_t d)
{(void)v;(void)p;(void)d;return A733_I2S_RESOURCE_RETAINED;}
static int no_read(void *v,uintptr_t a,uint32_t *n){(void)v;(void)a;(void)n;return 0;}
static int no_write(void *v,uintptr_t a,uint32_t n){(void)v;(void)a;(void)n;return 0;}
void vApplicationStackOverflowHook(TaskHandle_t t,char *n){(void)t;platform_panic(n,1);}
void kernel_main(void)
{
 platform_init();uint64_t hz;__asm__ volatile("mrs %0,cntfrq_el0":"=r"(hz));
 configASSERT(a733_counter_clock_init(&clock_source,(uint32_t)hz));
 configASSERT(a733_clock_leases_init(&registry,a733_counter_clock_now,&clock_source));
 configASSERT(a733_platform_locks_init(&locks,a733_counter_clock_now,&clock_source));
 uint64_t now;configASSERT(a733_counter_clock_now(&clock_source,&now));
 struct a733_dma_handoff handoff={
  .allowed={.fabric={0x03000002,0x82000001,0x84000001},.rate_hz={200000000,400000000,200000000},
   .desired_cpu=I2S_CHECK_TX==2?3:1,.route_change_mask=I2S_CHECK_TX==2?3:1,.owner=7,.handoff=123,.deadline_us=now+2000000,.cold=1},
  .controller=0x04601000,.evidence=123,.established=A733_DMA_HANDOFF_SUPERVISED,
  .owned_channels=0xffff,.service_channels=I2S_CHECK_TX==2?3:1};
 profile.stream=(struct a733_i2s_stream_profile){.format={24576000,16000,0,16,32,1,0},
  .operations={.io={no_acquire,no_health,no_health,no_now,no_read,no_write},.health=no_health},
  .cookie=&binding,.direction=I2S_CHECK_TX==1?A733_I2S_TRANSMIT:A733_I2S_RECEIVE,.operation_us=10000};
 if(I2S_CHECK_TX==2){profile.duplex=1;profiles[1]=profile;profiles[1].stream.direction=A733_I2S_TRANSMIT;}
 struct a733_dma_pool_config config={&handoff,&clock_source,&registry,&locks,hz/100,4096,4,1,fatal,NULL};
 configASSERT(!a733_dma_pool_session_stream_init(&session,&config,&profile,4,4096));
 configASSERT(!session.admitted);
 configASSERT(a733_dma_pool_session_stream_supervised_init(&session,&config,&profile,4,4096));
 configASSERT(session.count==(I2S_CHECK_TX==2?2u:1u)&&session.periods==4&&session.extent.bytes==(I2S_CHECK_TX==2?10u:5u)*4096);
 configASSERT(session.startup.supervised&&session.startup.handoff.established==A733_DMA_HANDOFF_SUPERVISED);
 configASSERT(!device_memory_release(session.pool,session.lease));
 const struct a733_dma_operations *ops=&session.service.controller.operations;
 struct a733_dma_mapping m=session.jobs[0].transfer.descriptors;
 configASSERT(ops->validate_mapping(&session,&m));
 m.cpu=(uint8_t *)m.cpu+3*64;m.bus+=3*64;m.bytes=64;
 configASSERT(ops->validate_mapping(&session,&m));m.cpu=(uint8_t *)m.cpu+64;m.bus+=64;
 configASSERT(!ops->validate_mapping(&session,&m));
 for(unsigned i=0;i<4;++i){
  m=session.stream_mappings[i];configASSERT(ops->validate_mapping(&session,&m));
  m.bytes=64;configASSERT(!ops->validate_mapping(&session,&m));
  m=session.stream_mappings[i];m.access=I2S_CHECK_TX==1?A733_DMA_WRITE:A733_DMA_READ;
  configASSERT(!ops->validate_mapping(&session,&m));
 }
 struct a733_dma_endpoint fifo={I2S_CHECK_TX==1?A733_I2S0_TXFIFO:A733_I2S0_RXFIFO,3,2,8};
 configASSERT(ops->validate_fifo(&session,&fifo,I2S_CHECK_TX==1?A733_DMA_WRITE:A733_DMA_READ));
 fifo.address=I2S_CHECK_TX==1?A733_I2S0_RXFIFO:A733_I2S0_TXFIFO;
 configASSERT(!ops->validate_fifo(&session,&fifo,I2S_CHECK_TX==1?A733_DMA_WRITE:A733_DMA_READ));
 if(I2S_CHECK_TX==2){
  configASSERT(ops->validate_fifo(&session,&fifo,A733_DMA_WRITE));
  for(unsigned i=0;i<4;++i){
   m=session.duplex_mappings[i];configASSERT(ops->validate_mapping(&session,&m));
   m.access=A733_DMA_WRITE;configASSERT(!ops->validate_mapping(&session,&m));
  }
  m=session.jobs[1].transfer.descriptors;configASSERT(ops->validate_mapping(&session,&m));
  m.cpu=(uint8_t *)m.cpu+3*64;m.bus+=3*64;m.bytes=64;configASSERT(ops->validate_mapping(&session,&m));
  m.cpu=(uint8_t *)m.cpu+64;m.bus+=64;configASSERT(!ops->validate_mapping(&session,&m));
 }
 configASSERT(session.pool->allocations[session.lease.slot].state==DEVICE_MEMORY_DEVICE);
 platform_puts(I2S_CHECK_TX==2?"PASS native I2S duplex pool catalogues\n":I2S_CHECK_TX?"PASS native I2S TX pool catalogue\n":"PASS native I2S RX pool catalogue\n");
 platform_exit_privileged(0);
}
