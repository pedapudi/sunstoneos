/* Actual native service/controller/audio/pin providers; registers/DMA modeled. */
#define kernel_main finite_kernel_main
#include "i2s-stream.c"
#undef kernel_main
#include "../apps/i2s/application.h"
#ifndef I2S_FILE_CHECK
#define I2S_FILE_CHECK 0
#endif
struct i2s_application app;
static _Alignas(4096) uint8_t pcm_pool[5*4096];
static uint8_t pcm_device[4][4096],fragment[512];
static unsigned completed_pcm,final_busy_reads,fifo_emptied;
int i2s_live(void *p){(void)p;return 1;}
void i2s_fail(void *p){(void)p;configASSERT(0);}
int i2s_request(enum dma_service_operation op,uint64_t timeout,uint64_t ticket)
{
 app.reply=request(op,0,timeout,ticket);
 return app.reply.result==DMA_SERVICE_OK?1:app.reply.result==DMA_SERVICE_BUSY?0:-1;
}
static void pcm_advance(void)
{
 if(live && get(&requested)){
  set(&requested,0);const struct a733_dma_transfer *t=service.controller.channels[0].transfer;
  configASSERT(t && t->count==1 && t->segments[0].bytes==(completed_pcm==2&&I2S_FILE_CHECK?2u:4096u) && (ir[28/4]&128) && (ir[0]&4));
  unsigned slot=(unsigned)(((uintptr_t)t->mappings[0].cpu-(uintptr_t)pcm_pool-4096)/4096);
  configASSERT(slot<4);for(unsigned i=0;i<t->segments[0].bytes;++i)configASSERT(pcm_device[slot][i]==0x31+completed_pcm);
  ++completed_pcm;ir[40/4]+=t->segments[0].bytes/2;dma[di(0,0x38)]|=4;live=0;
 }
 if(!dma[di(0,0)]&&get(&drain_enabled)){
  if(I2S_FILE_CHECK&&completed_pcm==3&&(dma[0x30/4]&1)&&final_busy_reads++<4){
   configASSERT(atomic_load(&app.runtime.consumed)==4096&&!atomic_load(&app.runtime.file_done));return;
  }
  dma[0x30/4]&=~1u;
 }
}
static uint32_t pcm_dma_read(void *p,uintptr_t at)
{(void)p;privileged();pcm_advance();return dma[(at-D)/4];}
static void pcm_dma_write(void *p,uintptr_t at,uint32_t value)
{
 (void)p;privileged();unsigned off=at-D,r=(off-0x100)%64;
 if(r==0){configASSERT(!(ir[28/4]&128));if(value){live=1;dma[0x30/4]|=1;inc(&submissions);}else live=0;}
 if(r==0x38)dma[off/4]&=~value;else dma[off/4]=value;
}
static void pcm_cache(void *p,enum a733_dma_cache op,void *cpu,size_t bytes)
{
 (void)p;privileged();a733_dma_native_operations.cache(&native,op,cpu,bytes);
 uintptr_t at=(uintptr_t)cpu,first=(uintptr_t)pcm_pool+4096;
 if(at>=first && at<first+4*4096){configASSERT(op==A733_DMA_CLEAN && (bytes==4096 || (I2S_FILE_CHECK&&bytes==64)) && !live);
  memcpy(pcm_device[(at-first)/4096],cpu,bytes);}
}
static int pcm_read_reg(void *p,uintptr_t at,uint32_t *out)
{
 if(I2S_FILE_CHECK&&at==A733_I2S0_BASE+24&&completed_pcm==3&&!(dma[0x30/4]&1)){
  /* The first sample still holds eight entries. The serializer then empties the
   * FIFO and latches TXU, so later ISTA reads report the expected underrun. */
  (void)p;locked();inc(&io_count);*out=(fifo_emptied?128u:120u)<<16;
  if(!fifo_emptied){fifo_emptied=1;ir[12/4]|=0x40;}return 1;
 }
 return read_reg(p,at,out);
}
static int pcm_prepared(void *p,uintptr_t base,uint16_t channels)
{return p==&parent&&base==D&&channels==1&&a733_audio_parent_ready(&parent);}
static void pcm_client(void *unused)
{
 (void)unused;while(!get(&peer_stage))vTaskDelay(1);unsigned uploaded=0,refusals=0;
 if(I2S_FILE_CHECK)configASSERT(i2s_runtime_begin(4097));
 for(unsigned n=0;n<2000;++n){
  if(uploaded<(I2S_FILE_CHECK?3u:12u)&&i2s_runtime_credits()){
   memset(fragment,0x31+uploaded,sizeof(fragment));
   unsigned bytes=i2s_runtime_upload_bytes(uploaded+1);
   for(unsigned off=0;off<bytes;off+=512){unsigned n=bytes-off<512?bytes-off:512;configASSERT(i2s_runtime_upload(uploaded+1,off,fragment,n)==1);}
   ++uploaded;
  }
  if(I2S_FILE_CHECK&&uploaded==3&&!atomic_load(&app.runtime.eof))configASSERT(i2s_runtime_end(4097));
  if(live&&n%16==0)set(&requested,1);
  if(!I2S_FILE_CHECK&&atomic_load(&app.runtime.consumed)>=8*2048)atomic_store(&app.cancel,1);
  int r=i2s_runtime_step();
  if(I2S_FILE_CHECK&&app.runtime.held&&!refusals){
   uint64_t ticket=app.runtime.held;
   configASSERT(request(DMA_SERVICE_PERIOD_END,0,0,ticket).result==DMA_SERVICE_INVALID);
   configASSERT(request(DMA_SERVICE_PERIOD_END,0,2049,ticket).result==DMA_SERVICE_INVALID);
   configASSERT(request(DMA_SERVICE_PERIOD_END,0,1,ticket+1).result==DMA_SERVICE_INVALID);
   configASSERT(!service.streams[0].input_ended);refusals=1;
  }
  if(r){configASSERT(r==1&&atomic_load(&app.result)==2&&atomic_load(&app.runtime.drained)==1);
   configASSERT(I2S_FILE_CHECK?(completed_pcm==3&&final_busy_reads>=5&&atomic_load(&app.runtime.consumed)==4097&&atomic_load(&app.runtime.committed)==4097&&atomic_load(&app.runtime.file_done)):(completed_pcm>=8 && atomic_load(&app.runtime.consumed)==8*2048));
   if(I2S_FILE_CHECK){configASSERT(service.streams[0].status==A733_I2S_INPUT_ENDED&&(ir[0]&5)==5&&fifo_emptied&&(ir[12/4]&0x40));
    configASSERT(request(DMA_SERVICE_REAP,0,0,0).result==DMA_SERVICE_BUSY&&request(DMA_SERVICE_CLOSE,0,100000,0).result==DMA_SERVICE_BUSY);}
   configASSERT(bindings[0].pin_lease.id && service.channels[0].holds_buffers);
   platform_puts(I2S_FILE_CHECK?"PASS file PCM actual providers two full periods and one frame, exact memory/FIFO accounting and retained EOF\n":"PASS runtime caller PCM actual providers, distinct periods, exact completed refill and retained cancellation\n");platform_exit(0);}
  vTaskDelay(1);
 }
 configASSERT(0);
}
void kernel_main(void)
{
 tx_mode=1;platform_init();__asm__ volatile("mrs %0,cntfrq_el0":"=r"(frequency));
 ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);ccu[0x280/4]=0xf8047f00;
 ccu[0x191c/4]=0x60006;ccu[0x199c/4]=0x60000;dma[0x34/4]=3;dma[di(0,8)]=0x41000000;
 configASSERT(a733_clock_leases_init(&clocks,clock_now,NULL)&&a733_platform_locks_init(&locks,clock_now,NULL));
 const struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=clock_now,.read=pcm_read_reg,.write=write_reg};
 configASSERT(a733_audio_leases_init(&audio,&clocks,7,&backend,NULL));
 configASSERT(a733_audio_parent_init(&parent,&audio.operations,&audio,0,100000));
 struct a733_pins_operations pops={enter,leave,clock_now,read_reg,write_reg};configASSERT(a733_pins_init(&pins,&pops,NULL,A733_PINS_PB_MASK));
 struct a733_pin_request pins_req[]={{5,{0,0,0},{3,0,0}},{6,{0,0,0},{3,0,0}},{7,{0,0,0},{3,0,0}}};
 configASSERT(a733_i2s_platform_stream_init(&bindings[0],&parent,&pins,&audio,7,pins_req,3,A733_I2S_TRANSMIT));
 profiles[0].stream=(struct a733_i2s_stream_profile){{24576000,16000,0,16,32,1,0},a733_i2s_platform_stream_operations(),&bindings[0],A733_I2S_TRANSMIT,10000,0};
 struct a733_i2s_plan plan;configASSERT(a733_i2s_plan(&profiles[0].stream.format,&plan)&&plan.frame_bytes==2);
 struct dma_memory_window window={(uintptr_t)pcm_pool,(uintptr_t)pcm_pool,sizeof(pcm_pool),7};
 struct dma_fifo_port port={A733_I2S0_TXFIFO,3,1u<<2,1u<<8,A733_DMA_WRITE};
 configASSERT(a733_dma_native_init(&native,0x09000000,0x09000000,&window,1,&port,1,fatal,NULL));
 struct a733_dma_segment segments[4];struct a733_dma_mapping maps[4];
 for(unsigned i=0;i<4;++i){uintptr_t at=(uintptr_t)pcm_pool+(i+1)*4096;
  maps[i]=(struct a733_dma_mapping){(void *)at,at,4096,A733_DMA_READ};
  segments[i]=(struct a733_dma_segment){{at,0,2,8},{A733_I2S0_TXFIFO,3,2,8},4096};app.period_memory[i]=(void *)at;}
 struct dma_service_job job={.kind=DMA_SERVICE_I2S_STREAM,.channel=0,.stream=&profiles[0],
  .transfer={{pcm_pool,(uintptr_t)pcm_pool,4096,A733_DMA_DESCRIPTORS},segments,4,maps,4}};
 const struct a733_dma_operations ops={pcm_dma_read,pcm_dma_write,pcm_cache,barrier,ticks,mapping,fifo};
 struct dma_service_protected protected[]={{(uintptr_t)&service,(uintptr_t)&service,sizeof(service)},
  {(uintptr_t)&native,(uintptr_t)&native,sizeof(native)},{(uintptr_t)&audio,(uintptr_t)&audio,sizeof(audio)},
  {(uintptr_t)&parent,(uintptr_t)&parent,sizeof(parent)},{(uintptr_t)&bindings,(uintptr_t)&bindings,sizeof(bindings)},
  {(uintptr_t)&pins,(uintptr_t)&pins,sizeof(pins)},{(uintptr_t)&clocks,(uintptr_t)&clocks,sizeof(clocks)},
  {(uintptr_t)&locks,(uintptr_t)&locks,sizeof(locks)}};
 creating_bootstrap=1;bootstrap_task=xTaskCreateStaticAffinitySet(bootstrap,"audio-bootstrap",4096,NULL,2,stacks[0],&tasks[0],1);creating_bootstrap=0;
 struct dma_service_activation activation={bootstrap_task,pcm_prepared,&parent};
 configASSERT(dma_service_init_deferred(&service,&ops,NULL,D,1,frequency/10,&job,1,protected,8,3,3,&activation));
 TaskHandle_t client_task=xTaskCreateStaticAffinitySet(pcm_client,"runtime-pcm",4096,NULL,2,stacks[1],&tasks[1],2);
 configASSERT(client_task&&dma_service_bind(&service,0,client_task,1));vTaskStartScheduler();configASSERT(0);
}
