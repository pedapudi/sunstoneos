/* Execute the new client with actual shared providers; controller responses,
 * endpoint DMA/MSI and scheduler progress are explicit software models. */
#define main prior_pcie_cases_not_run
#include "a733-pcie.c"
#undef main
#include "a733_pcie_epf_test.h"
#include <sys/mman.h>
static struct a733_pcie_epf_test client;
static struct device_memory_pool memory;
static unsigned oversize_bar,wide_bar,irq_armed,irq_count,irq_stopped,submitted;
static uint8_t output[4096];
static int epf_read(void *v,uintptr_t a,unsigned width,uint32_t *out)
{
 assert(a733_pcie_native_address(a,width,0)||a733_pcie_epf_native_address(&port,a,width,0));
 if(a>=0x22000000&&a<=0x2200002c){assert(width==4);*out=*pmic_model_word(v,a);return 1;}
 return pcie_read(v,a,width,out);
}
static int epf_write(void *v,uintptr_t a,unsigned width,uint32_t value)
{
 assert(a733_pcie_native_address(a,width,1)||a733_pcie_epf_native_address(&port,a,width,1));
 if(port.test)assert(a733_pcie_epf_native_write(&port,a,width,value));
 if(a==0x20000010&&value==UINT32_MAX){*pmic_model_word(v,a)=(oversize_bar?0xffe00000u:0xfffffe00u)|(wide_bar?4u:0);return 1;}
 if(wide_bar&&a==0x20000014&&value==UINT32_MAX){*pmic_model_word(v,a)=UINT32_MAX;return 1;}
 if(a==0x06000830){assert(value==1);*pmic_model_word(v,a)&=~value;return 1;}
 if(a==0x22000004){assert(irq_armed&&!submitted&&value==32);++submitted;}
 if(a733_pcie_native_address(a,width,1))return pcie_write(v,a,width,value);
 uint32_t *word=pmic_model_word(v,a&~(uintptr_t)3),mask=width==4?UINT32_MAX:65535u<<((a&3)*8);
 *word=(*word&~mask)|(value<<((a&3)*8));return 1;
}
static int arm(void *unused){(void)unused;assert(!irq_armed&&!irq_count&&!irq_stopped);irq_armed=1;return 1;}
static int event(void *unused,uint32_t *count){(void)unused;*count=irq_count;return irq_count?1:0;}
static void stop(void *unused){(void)unused;++irq_stopped;}
static void begin(unsigned wide,unsigned bar)
{
 setup();
 *pmic_model_word(&model,0x20000034)=0x40;
 *pmic_model_word(&model,0x20000040)=0x00028010;
 *pmic_model_word(&model,0x20000050)=0x00110000;
 *pmic_model_word(&model,0x20000070)=0;
 *pmic_model_word(&model,0x20000080)=0x00800005;
 assert(complete()==A733_PCIE_READY); /* Nominal provider construction only. */
 port.ops.read=epf_read;port.ops.write=epf_write;
 memset(&client,0,sizeof(client));memset(&memory,0,sizeof(memory));memset(output,0xa7,sizeof(output));
 wide_bar=wide;irq_armed=irq_count=irq_stopped=submitted=0;
 *pmic_model_word(&model,0x20000010)=wide?4:0;

 assert(device_memory_pool_init(&memory,0x41c00000,0x41c00000,2*1024*1024));
 const struct a733_pcie_epf_config c={.identity=91,.timeout_us=10000,.vendor=0x1234,.device=0x5678,.bar=bar};
 const struct a733_pcie_epf_irq irq={arm,event,stop,NULL};
 assert(a733_pcie_epf_init(&client,&port,&c,&memory,&irq));
}
static enum a733_pcie_result advance(void)
{
 enum a733_pcie_result r=a733_pcie_epf_poll(&client);
 if(r!=A733_PCIE_RETAINED)assert(pmic_model_step(&model)!=A733_PMIC_RETAINED);
 model.now+=100;return r;
}
static void wait_command(void)
{
 for(unsigned i=0;i<20&&!submitted;++i)assert(advance()!=A733_PCIE_RETAINED);
 assert(submitted==1&&irq_armed&&port.test==&client);
 assert(a733_pcie_poll(&port)==A733_PCIE_INVALID);
 uint32_t untouched=0xa5a5a5a5;assert(a733_pcie_config_read(&port,0,0,model.now+1000,&untouched)==A733_PCIE_INVALID&&untouched==0xa5a5a5a5);
 assert(*pmic_model_word(&model,0x06000004)==0x00100106);
 assert(*pmic_model_word(&model,0x06300208)==0x22000000&&*pmic_model_word(&model,0x06300210)==0x220fffff);
 assert(*pmic_model_word(&model,0x2200002c)==1&&*pmic_model_word(&model,0x2200001c)==4096);
 assert(!a733_pcie_epf_native_write(&port,0x06000828,4,UINT32_MAX));
 assert(!a733_pcie_epf_native_write(&port,0x0600082c,4,0));
 assert(!a733_pcie_epf_native_write(&port,0x06300214,4,0x41000000));
}
static void endpoint_complete(void)
{
 /* Bus transactions and ordered endpoint completion are modeled; the actual
  * client must validate every returned byte before copying into output. */
 memcpy((void *)(client.memory[1].cpu+4096),(void *)(client.memory[0].cpu+4096),4096);
 *pmic_model_word(&model,0x22000004)=0;*pmic_model_word(&model,0x22000008)=0x50;
 *pmic_model_word(&model,0x06000830)=1;irq_count=1;
}
static void refused(void)
{
 enum a733_pcie_result r=A733_PCIE_PENDING;
 for(unsigned i=0;i<20&&r!=A733_PCIE_RETAINED;++i)r=advance();
 assert(r==A733_PCIE_RETAINED&&client.failed&&irq_stopped);
 assert(a733_pcie_epf_copy(&client,output)!=A733_PCIE_READY);
 for(unsigned i=0;i<3;++i){enum device_memory_state state;struct device_memory_extent e;
  assert(device_memory_describe(&memory,client.leases[i],&e,&state)&&state==DEVICE_MEMORY_RETAINED);}
 unsigned writes=trace_count;assert(a733_pcie_epf_poll(&client)==A733_PCIE_RETAINED&&trace_count==writes);
}
int main(int argc,char **argv)
{
 assert(argc==2);assert(mmap((void *)0x41c00000,2*1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
 const char *mode=argv[1];
 if(!strncmp(mode,"gen2",4))link_speed=negotiated=2; /* Modeled directed change to 5 GT/s. */
 begin(!strcmp(mode,"wide")||!strcmp(mode,"high-half"),!strcmp(mode,"high-half")?1:0);
 oversize_bar=!strcmp(mode,"oversize");
 if(!strcmp(mode,"fixture")){
  for(unsigned i=0;i<model.word_count;++i)printf("%lx %x\n",(unsigned long)model.words[i].address,model.words[i].value);
  return 0;
 }
 if(!strcmp(mode,"other-bar")){*pmic_model_word(&model,0x20000018)=0x22000000;refused();assert(!irq_armed&&!submitted);}
 else if(!strcmp(mode,"high-half")){*pmic_model_word(&model,0x20000010)=4;refused();assert(!irq_armed&&!submitted);}
 else if(!strcmp(mode,"oversize")){refused();assert(!irq_armed&&!submitted);}
 else {
  wait_command();
  if(!strcmp(mode,"deadline")){model.now=client.operation_deadline;refused();}
  else {
   endpoint_complete();
   if(!strcmp(mode,"missing-msi")){irq_count=0;assert(advance()==A733_PCIE_PENDING);model.now=client.operation_deadline;refused();}
   else if(!strcmp(mode,"status-error")){*pmic_model_word(&model,0x22000008)=0x60;refused();}
   else if(!strcmp(mode,"extra-msi")){*pmic_model_word(&model,0x06000830)=3;refused();}
   else if(!strcmp(mode,"data-error")){*(uint8_t *)(client.memory[1].cpu+4103)^=1;refused();}
   else if(!strcmp(mode,"guard-error")){*(uint8_t *)client.memory[1].cpu^=1;refused();}
   else if(!strcmp(mode,"source-error")){*(uint8_t *)(client.memory[0].cpu+4103)^=1;refused();}
   else if(!strcmp(mode,"link-loss")){*pmic_model_word(&model,0x06400e0c)=0;refused();}
   else if(!strcmp(mode,"gen2-retrain")){*pmic_model_word(&model,0x06000080)=0x00110000;refused();}
   else {
    assert(!strcmp(mode,"nominal")||!strcmp(mode,"wide")||!strcmp(mode,"gen2"));assert(advance()==A733_PCIE_READY);
    assert(port.endpoint.link_status==(!strcmp(mode,"gen2")?0x12:0x11));
    assert(a733_pcie_epf_copy(&client,(void *)&port)==A733_PCIE_INVALID);
    assert(a733_pcie_epf_copy(&client,(void *)&model.clocks)==A733_PCIE_INVALID);
    assert(a733_pcie_epf_copy(&client,output)==A733_PCIE_READY&&client.published);
    assert(!memcmp(output,(void *)(client.memory[0].cpu+4096),4096));
    unsigned writes=trace_count;assert(advance()==A733_PCIE_READY&&trace_count==writes);
    for(unsigned i=0;i<3;++i){enum device_memory_state state;struct device_memory_extent e;
     assert(device_memory_describe(&memory,client.leases[i],&e,&state)&&state==DEVICE_MEMORY_RETAINED);}
   }
  }
 }
 printf("PASS concrete PCIe endpoint client: %s (DMA/controller/IRQ modeled)\n",mode);return 0;
}
