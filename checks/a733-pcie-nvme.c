/* Execute the general function layer and the read-only NVMe driver with the
 * actual shared providers. The endpoint configuration space, BAR decode, the
 * NVMe controller, DesignWare MSI/SII latches and link loss are explicit
 * software models; nothing here establishes physical behavior. */
#define main prior_pcie_cases_not_run
#include "a733-pcie.c"
#undef main
#include "a733_pcie_nvme.h"
#include <sys/mman.h>
#define BAR0_BYTES 0x4000u
#define MEMORY 0x41c00000u
static struct a733_pcie_nvme nvme;
static struct a733_pcie_function plain;
static struct device_memory_pool memory;
/* Functions 1..7: configuration dwords, present only when the mask says so. */
static uint32_t sibling[8][64];static unsigned siblings;
static uint32_t table[8][4],bar2[1024];
static unsigned no_soft_reset,large_bar,assigned_bar,table_outside,class_mismatch;
static unsigned inherited,fatal_at_enable,identify_error,drop,shst_stuck,link_stuck;
static unsigned irq_armed,irq_stopped,dma_count,msi_count,intx_count,d3,d3_writes;
static uint64_t pm_since,ready_at,shutdown_at;
static struct {
    uint32_t cc,csts,aqa;uint64_t asq,acq,sq[2],cq[2];
    uint16_t sq_head[2],sq_tail[2],cq_tail[2],cq_head[2];uint8_t phase[2];unsigned created,ien;
} ctl;
static uint32_t *w(uintptr_t a){return pmic_model_word(&model,a);}
static unsigned target_function(void){return (*w(0x06300014)>>16)&7;}
static uint32_t command16(void){return *w(0x20000004)&65535;}
static uint32_t bar0(void){return *w(0x20000010)&~15u;}
static uint8_t pattern(uint64_t at){return (uint8_t)((at*131u+(at>>9)*7u)^0x3cu);}
static const struct a733_pcie_function *client(void){return port.function;}
static void owned_dma(void)
{
    /* Every modeled endpoint DMA needs bus mastering and device-owned memory. */
    assert(command16()&4);
    const struct a733_pcie_nvme *n=&nvme;
    struct device_memory_allocation *a=device_memory_lookup(&memory,n->lease);
    assert(a&&a->state==DEVICE_MEMORY_DEVICE);++dma_count;
}
static void *host(uint64_t physical,size_t bytes)
{
    assert(physical>=MEMORY&&physical+bytes<=MEMORY+2*1024*1024);
    return (void *)(uintptr_t)physical;
}
static void interrupt(unsigned vector)
{
    unsigned control=(*w(0x20000090)>>16)&0xffff;
    if(control&0x8000){
        if((control&0x4000)||(table[vector][3]&1))return; /* Masked: pending bit only. */
        assert(table[vector][0]==*w(0x06000820)&&!table[vector][1]&&table[vector][2]==vector);
        assert(*w(0x06000828)&(1u<<vector));*w(0x06000830)|=1u<<vector;++msi_count;return;
    }
    if(!(command16()&0x400)){*w(0x06400e08)|=1u<<5;++intx_count;}
}
static void post(unsigned q,uint16_t cid,uint32_t result,unsigned status)
{
    uint32_t *e=host(ctl.cq[q]+16*ctl.cq_tail[q],16);owned_dma();
    e[0]=result;e[1]=0;e[2]=ctl.sq_head[q]|(uint32_t)q<<16;
    e[3]=cid|(uint32_t)ctl.phase[q]<<16|status<<17;
    if(++ctl.cq_tail[q]==2){ctl.cq_tail[q]=0;ctl.phase[q]^=1;}
    if(!q||(ctl.ien&2))interrupt(0);
}
static void identify(const uint32_t *c)
{
    uint8_t *d=host((uint64_t)c[7]<<32|c[6],4096);owned_dma();memset(d,0,4096);
    if(c[10]==1){
        d[0]=0x4d;d[1]=0x14;d[2]=0x4d;d[3]=0x14;memcpy(d+4,"S4EWNX0R123456      ",20);
        memcpy(d+24,"MODEL NVME READ-ONLY FIXTURE            ",40);memcpy(d+64,"2B2QEXM7",8);
        d[77]=5;d[80]=0;d[81]=4;d[82]=1;d[516]=1;
    } else {
        assert(c[10]==0&&c[1]==1);
        d[2]=0x10; /* 0x100000 blocks of 512 bytes. */
        d[25]=0;d[26]=0;d[130]=9;
    }
}
static void execute(unsigned q)
{
    while(ctl.sq_head[q]!=ctl.sq_tail[q]){
        const uint32_t *c=host(ctl.sq[q]+64*ctl.sq_head[q],64);owned_dma();
        ctl.sq_head[q]=(uint16_t)((ctl.sq_head[q]+1)%2);
        unsigned opcode=c[0]&255;uint16_t cid=(uint16_t)(c[0]>>16);
        if(drop)return;
        if(q){
            /* Read only: no other I/O opcode reaches the model. */
            assert(opcode==2&&c[1]==1&&!c[8]&&!c[9]);
            uint64_t lba=(uint64_t)c[11]<<32|c[10];unsigned bytes=(c[12]+1)*512;
            assert(bytes<=4096);uint8_t *d=host((uint64_t)c[7]<<32|c[6],bytes);owned_dma();
            for(unsigned i=0;i<bytes;++i)d[i]=pattern(lba*512+i);
            post(1,cid,0,0);continue;
        }
        switch(opcode){
        case 6:if(identify_error){post(0,cid,0,0x2);break;}identify(c);post(0,cid,0,0);break;
        case 9:assert(c[10]==7&&!c[11]);post(0,cid,0,0);break;
        case 5:ctl.cq[1]=(uint64_t)c[7]<<32|c[6];ctl.ien=c[11];ctl.created|=1;post(0,cid,0,0);break;
        case 1:assert(ctl.created&1);ctl.sq[1]=(uint64_t)c[7]<<32|c[6];ctl.created|=2;post(0,cid,0,0);break;
        case 0:ctl.created&=~2u;post(0,cid,0,0);break;
        case 4:assert(!(ctl.created&2));ctl.created&=~1u;post(0,cid,0,0);break;
        default:assert(0);
        }
    }
}
static uint32_t nvme_read(uint32_t off)
{
    switch(off){
    case 0:return 0x140100ff; /* MQES 255, CQR, TO 10 s. */
    case 4:return 0x20; /* NVM command set, DSTRD 0, 4 KiB pages. */
    case 8:return 0x00010400;
    case 0x14:return ctl.cc;
    case 0x1c:
        if((ctl.cc&1)&&!(ctl.csts&1)&&model.now>=ready_at&&!fatal_at_enable)ctl.csts|=1;
        if((ctl.cc&1)&&fatal_at_enable&&model.now>=ready_at)ctl.csts|=2;
        if((ctl.cc&0xc000)==0x4000&&!shst_stuck&&model.now>=shutdown_at)ctl.csts=(ctl.csts&~0xcu)|8;
        return ctl.csts;
    case 0x24:return ctl.aqa;
    case 0x28:return (uint32_t)ctl.asq;case 0x2c:return (uint32_t)(ctl.asq>>32);
    case 0x30:return (uint32_t)ctl.acq;case 0x34:return (uint32_t)(ctl.acq>>32);
    default:break;
    }
    if(off>=0x2000&&off<0x2080)return table[(off-0x2000)/16][(off%16)/4];
    if(off>=0x3000&&off<0x3008)return 0;
    return 0;
}
static void nvme_write(uint32_t off,uint32_t v)
{
    switch(off){
    case 0x14:
        if((v&1)&&!(ctl.cc&1)){
            assert(ctl.aqa==0x00010001&&ctl.asq&&ctl.acq);ready_at=model.now+2000;
            ctl.sq[0]=ctl.asq;ctl.cq[0]=ctl.acq;
        }
        if((v&0xc000)==0x4000&&!(ctl.cc&0xc000))shutdown_at=model.now+500;
        ctl.cc=v;return;
    case 0x24:ctl.aqa=v;return;
    case 0x28:ctl.asq=(ctl.asq&~(uint64_t)UINT32_MAX)|v;return;
    case 0x2c:ctl.asq=(ctl.asq&UINT32_MAX)|(uint64_t)v<<32;return;
    case 0x30:ctl.acq=(ctl.acq&~(uint64_t)UINT32_MAX)|v;return;
    case 0x34:ctl.acq=(ctl.acq&UINT32_MAX)|(uint64_t)v<<32;return;
    case 0x1000:ctl.sq_tail[0]=(uint16_t)v;execute(0);return;
    case 0x1004:ctl.cq_head[0]=(uint16_t)v;return;
    case 0x1008:ctl.sq_tail[1]=(uint16_t)v;execute(1);return;
    case 0x100c:ctl.cq_head[1]=(uint16_t)v;return;
    default:break;
    }
    if(off>=0x2000&&off<0x2080){table[(off-0x2000)/16][(off%16)/4]=v;return;}
    assert(0);
}
static void reset_function(void)
{
    *w(0x20000004)=1u<<20;*w(0x20000090)&=~(0xc000u<<16);
    *w(0x20000010)=4;*w(0x20000014)=0;*w(0x20000018)=0;*w(0x2000001c)=0;*w(0x20000020)=1;*w(0x20000024)=0;
    for(unsigned i=0;i<8;++i){table[i][0]=table[i][1]=table[i][2]=0;table[i][3]=1;}
}
static void policy_read(uintptr_t a,unsigned width)
{assert(a733_pcie_native_address(a,width,0)||a733_pcie_function_native_address(&port,a,width,0));}
/* The same decisions as a733_pcie_native.c write() with the function hook. */
static int native_write(uintptr_t a,unsigned width,uint32_t v)
{
    if(!a733_pcie_native_address(a,width,1)&&!a733_pcie_function_native_address(&port,a,width,1))return 0;
    if(a==0x06000004&&v!=0x102&&!a733_pcie_function_native_write(&port,a,width,v))return 0;
    if((port.function||!a733_pcie_native_address(a,width,1))&&!a733_pcie_function_native_write(&port,a,width,v))return 0;
    if(a>=0x06300000&&a<0x06302000&&!a733_pcie_atu_write_allowed(&port,a,v))return 0;
    return 1;
}
static int in_d3_wait(void){return pm_since&&model.now-pm_since<A733_PCIE_D3_WAIT_US;}
static int ep_read(void *v,uintptr_t a,unsigned width,uint32_t *out)
{
    policy_read(a,width);
    if(a>=0x20000000&&a<0x20001000){
        unsigned f=target_function();
        assert(!f||!in_d3_wait());
        if(!f){assert(!in_d3_wait());return pcie_read(v,a,width,out);}
        uint32_t word=siblings&(1u<<f)&&a-0x20000000<256?sibling[f][(a-0x20000000)/4]:(siblings&(1u<<f)?0:UINT32_MAX);
        *out=word>>((a&3)*8);if(width==1)*out&=255;if(width==2)*out&=65535;return 1;
    }
    if(a>=0x22000000&&a<0x28000000){
        assert(width==4&&(command16()&2)&&!d3);
        if(a-bar0()<BAR0_BYTES){*out=nvme_read((uint32_t)(a-bar0()));return 1;}
        uint32_t base=*w(0x20000018)&~15u;assert(a-base<4096);*out=bar2[(a-base)/4];return 1;
    }
    if(a==0x06400e0c&&!(*w(0x06400c00)&1)&&!(*w(0x02000210)&(1u<<22))&&!link_stuck){*out=0;return 1;}
    if(a==0x06400e00||a==0x06400e08){*out=*w(a);return 1;}
    return pcie_read(v,a,width,out);
}
static int ep_write(void *v,uintptr_t a,unsigned width,uint32_t value)
{
    assert(native_write(a,width,value));
    if(a>=0x20000000&&a<0x20001000){
        unsigned f=target_function();assert(!f&&!in_d3_wait());
        if(a>=0x20000010&&a<=0x20000024&&value==UINT32_MAX){
            static const uint32_t sizes[6]={0xffffc004,UINT32_MAX,0xfffff000,0,1,0};
            assert(a!=0x20000020);uint32_t s=sizes[(a-0x20000010)/4];
            if(a==0x20000010&&large_bar)s=0xf8000004;
            *w(a)=s;return 1;
        }
        if(a==0x20000084){
            /* PCI PM: D3hot entry and exit, with or without an internal reset. */
            uint32_t *pm=w(0x20000084),old=*pm&3;
            assert(value==0||value==3);++d3_writes;pm_since=model.now;
            *pm=(*pm&~3u)|value;d3=value==3;
            if(old==3&&!value&&!no_soft_reset)reset_function();
            return 1;
        }
    }
    if(a==0x06000830||a==0x06400e08){*w(a)&=~value;return 1;}
    if(a==0x06400e00){*w(a)=value;return 1;}
    if(a>=0x22000000&&a<0x28000000){
        assert(width==4&&(command16()&2)&&!d3);
        if(a-bar0()<BAR0_BYTES){nvme_write((uint32_t)(a-bar0()),value);return 1;}
        uint32_t base=*w(0x20000018)&~15u;assert(a-base<4096);bar2[(a-base)/4]=value;return 1;
    }
    if(a733_pcie_native_address(a,width,1))return pcie_write(v,a,width,value);
    uint32_t *word=w(a&~(uintptr_t)3),mask=width==4?UINT32_MAX:(width==2?65535u:255u)<<((a&3)*8);
    *word=(*word&~mask)|(value<<((a&3)*8));return 1;
}
static int arm(void *unused){(void)unused;assert(!irq_stopped);irq_armed=1;return 1;}
static int event(void *unused,uint32_t *count){(void)unused;*count=0;return 0;}
static void stop(void *unused){(void)unused;++irq_stopped;}
static const struct a733_pcie_irq_ops irq={arm,event,stop,NULL};
static void device(void)
{
    setup();
    *w(0x20000000)=0xa808144d;*w(0x20000008)=0x01080201;*w(0x2000000c)=siblings?0x00800000:0;
    *w(0x20000034)=0x40;*w(0x2000003c)=0x100;
    *w(0x20000040)=0x00028010;*w(0x20000050)=0x00110000;
    *w(0x20000080)=0x00039001;*w(0x20000084)=no_soft_reset?8:0;
    *w(0x20000090)=0x00070011;*w(0x20000094)=table_outside?0x4000:0x2000;*w(0x20000098)=0x3000;
    *w(0x06400e00)=0;*w(0x06400e08)=0;
    reset_function();
    if(assigned_bar)*w(0x20000018)=0x22100000;
    if(class_mismatch)*w(0x20000008)=0x01060101;
    assert(complete()==A733_PCIE_READY);
    port.ops.read=ep_read;port.ops.write=ep_write;
    memset(&ctl,0,sizeof(ctl));ctl.phase[0]=ctl.phase[1]=1;
    if(inherited){ctl.cc=0x00460001;ctl.csts=1;}
    memset(&memory,0,sizeof(memory));memset((void *)MEMORY,0,2*1024*1024);
    assert(device_memory_pool_init(&memory,MEMORY,MEMORY,2*1024*1024));
}
static enum a733_pcie_result advance(enum a733_pcie_result (*poll)(void *),void *object)
{
    enum a733_pcie_result r=poll(object);
    if(r!=A733_PCIE_RETAINED)assert(pmic_model_step(&model)!=A733_PMIC_RETAINED);
    model.now+=100;return r;
}
static enum a733_pcie_result nvme_poll(void *n){return a733_pcie_nvme_poll(n);}
static enum a733_pcie_result function_poll(void *f){return a733_pcie_function_poll(f);}
static enum a733_pcie_result until(enum a733_pcie_result (*poll)(void *),void *object,enum a733_pcie_result want)
{
    enum a733_pcie_result r=A733_PCIE_PENDING;
    for(unsigned i=0;i<2000&&r!=want&&r!=A733_PCIE_RETAINED;++i)r=advance(poll,object);
    return r;
}
static void start(unsigned mode)
{
    device();
    const struct a733_pcie_nvme_config c={.identity=93,.timeout_us=20000,.vendor=0x144d,.device=0xa808,
        .irq=mode,.vectors=mode==A733_PCIE_IRQ_MSIX?1:0,.namespace_id=1};
    memset(&nvme,0,sizeof(nvme));
    assert(a733_pcie_nvme_init(&nvme,&port,&c,&memory,mode==A733_PCIE_IRQ_POLL?NULL:&irq));
}
static void stopped(void)
{
    /* PERST low, LTSSM off, link down, both outbound regions off, decode and
     * bus mastering clear, and every DMA allocation retained. */
    assert(!(*w(0x02000210)&(1u<<22))&&!(*w(0x06400c00)&1));
    assert(!(*w(0x06300004)&(1u<<31))&&!(*w(0x06300204)&(1u<<31)));
    assert(!(command16()&7)&&(command16()&0x400)&&*w(0x06000020)==0x0000fff0);
    assert(port.client_stopped&&port.phase==19&&!port.failed);
    unsigned writes=trace_count;assert(a733_pcie_poll(&port)==A733_PCIE_STOPPED&&trace_count==writes);
    uint32_t value=0;assert(a733_pcie_config_read(&port,0,0,model.now+1000,&value)==A733_PCIE_STOPPED&&!value);
}
static void retained_memory(void)
{
    enum device_memory_state state;struct device_memory_extent e;
    assert(device_memory_describe(&memory,nvme.lease,&e,&state)&&state==DEVICE_MEMORY_RETAINED);
}
static void read_back(uint64_t lba)
{
    static uint8_t out[4096];memset(out,0xa7,sizeof(out));
    assert(a733_pcie_nvme_read(&nvme,lba));
    assert(a733_pcie_nvme_copy(&nvme,out)==A733_PCIE_PENDING);
    assert(until(nvme_poll,&nvme,A733_PCIE_READY)==A733_PCIE_READY);
    assert(a733_pcie_nvme_copy(&nvme,(void *)&port)==A733_PCIE_INVALID);
    assert(a733_pcie_nvme_copy(&nvme,(void *)MEMORY)==A733_PCIE_INVALID);
    assert(a733_pcie_nvme_copy(&nvme,out)==A733_PCIE_READY);
    for(unsigned i=0;i<4096;++i)assert(out[i]==pattern(lba*512+i));
}
static void nominal(unsigned mode)
{
    start(mode);
    assert(until(nvme_poll,&nvme,A733_PCIE_READY)==A733_PCIE_READY);
    /* BAR0 16 KiB and BAR2 4 KiB in one 1 MiB window; the I/O BAR stays unassigned. */
    const struct a733_pcie_function *f=client();
    assert(f==&nvme.function&&f->bars[0].base==0x22000000&&f->bars[0].bytes==0x4000);
    assert(f->bars[2].base==0x22004000&&f->bars[2].bytes==4096&&!f->bars[4].bytes&&f->io==(1u<<4));
    assert(*w(0x20000010)==0x22000004&&!*w(0x20000014)&&*w(0x20000018)==0x22004000&&*w(0x20000020)==1);
    assert(*w(0x06000020)==0x22002200&&*w(0x06300208)==0x22000000&&*w(0x06300210)==0x220fffff);
    assert(command16()==(mode==A733_PCIE_IRQ_INTX?6u:0x406u));
    struct a733_pcie_nvme_identity id;
    assert(a733_pcie_nvme_identity(&nvme,&id)==A733_PCIE_READY&&id.vendor==0x144d&&id.namespaces==1);
    assert(id.blocks==0x100000&&id.lba_shift==9&&id.mdts==5&&!memcmp(id.serial,"S4EWNX0R123456",14));
    read_back(0);read_back(8);read_back(0x100000-8);
    assert(!a733_pcie_nvme_read(&nvme,0x100000-7));
    if(mode==A733_PCIE_IRQ_MSIX)assert(msi_count&&(nvme.events&1)&&irq_armed&&!*w(0x06000830));
    if(mode==A733_PCIE_IRQ_INTX)assert(intx_count&&(nvme.events&1)&&irq_armed&&*w(0x06400e00)==(1u<<5));
    if(mode==A733_PCIE_IRQ_POLL)assert(!msi_count&&!intx_count);
    assert(a733_pcie_nvme_shutdown(&nvme));
    assert(until(nvme_poll,&nvme,A733_PCIE_STOPPED)==A733_PCIE_STOPPED);
    assert(!ctl.created&&(ctl.csts&0xc)==8);
    stopped();retained_memory();
    if(mode!=A733_PCIE_IRQ_POLL)assert(irq_stopped);
    if(mode==A733_PCIE_IRQ_MSIX){
        enum device_memory_state state;struct device_memory_extent e;
        assert(device_memory_describe(&memory,f->msi_lease,&e,&state)&&state==DEVICE_MEMORY_RETAINED);
        assert(!*w(0x06000828)&&*w(0x0600082c)==UINT32_MAX&&(*w(0x20000090)>>16&0xc000)==0x4000);
    }
    if(mode==A733_PCIE_IRQ_INTX)assert(!*w(0x06400e00));
    assert(a733_pcie_nvme_poll(&nvme)==A733_PCIE_STOPPED);
}
static void refused(unsigned bme)
{
    assert(until(nvme_poll,&nvme,A733_PCIE_READY)==A733_PCIE_RETAINED&&nvme.failed&&port.failed);
    retained_memory();
    /* Without a quiescence proof bus mastering and PERST release stay as they were. */
    assert(!!(command16()&4)==bme&&(*w(0x02000210)&(1u<<22)));
    unsigned writes=trace_count;assert(a733_pcie_nvme_poll(&nvme)==A733_PCIE_RETAINED&&trace_count==writes);
}
static void forged(void)
{
    /* A queued write command cannot pass the submission doorbell rule. */
    start(A733_PCIE_IRQ_POLL);assert(until(nvme_poll,&nvme,A733_PCIE_READY)==A733_PCIE_READY);
    volatile uint32_t *sq=(volatile uint32_t *)(nvme.memory.cpu+2*4096)+16*nvme.tail[1];
    uint32_t next=(nvme.tail[1]+1u)%2,data=(uint32_t)(nvme.memory.physical+5*4096);
    for(unsigned i=0;i<16;++i)sq[i]=0;
    sq[0]=1|(uint32_t)nvme.command_id<<16;sq[1]=1;sq[6]=data;
    assert(!a733_pcie_nvme_write_allowed(&nvme,0,0x1008,next));
    nvme.lba=0;nvme.stage=10; /* READ */
    assert(!a733_pcie_nvme_write_allowed(&nvme,0,0x1008,next));
    sq[0]=2|(uint32_t)nvme.command_id<<16;sq[12]=7;
    assert(a733_pcie_nvme_write_allowed(&nvme,0,0x1008,next)); /* The exact read. */
    assert(!a733_pcie_nvme_write_allowed(&nvme,0,0x1008,next^1)&&!a733_pcie_nvme_write_allowed(&nvme,0,0x1000,1));
    sq[12]=8;assert(!a733_pcie_nvme_write_allowed(&nvme,0,0x1008,next));
    for(unsigned i=0;i<16;++i)sq[i]=0;
    nvme.stage=9;
    /* Controller registers, other doorbells and the MSI-X table keep exact rules,
     * and BAR writes need a driver session. */
    assert(!native_write(0x22000014,4,0x00460001));
    uint32_t events;
    assert(a733_pcie_function_begin(&nvme.function,port.config.lifetime_deadline,&events)==A733_PCIE_READY);
    assert(!native_write(0x22000014,4,0x00460000)&&!native_write(0x22000014,4,0x00464001)&&
        !native_write(0x22000028,4,0x41c00000)&&!native_write(0x22001004,4,1)&&
        !native_write(0x22002000,4,0)&&!native_write(0x2200200c,4,0)&&!native_write(0x22003000,4,0)&&
        !native_write(0x22004000,4,0)&&!native_write(0x22100000,4,0));
    assert(!a733_pcie_function_write(&nvme.function,0,0x14,0x00460000)&&!a733_pcie_function_write(&nvme.function,0,0x4000,0));
    assert(a733_pcie_function_end(&nvme.function,1)==A733_PCIE_READY);
    /* Configuration writes reach only the claimed function's exact values. */
    assert(!native_write(0x20000004,2,0x7)&&!native_write(0x20000010,4,UINT32_MAX)&&
        !native_write(0x20000010,4,0x22100004)&&!native_write(0x20000020,4,UINT32_MAX)&&
        !native_write(0x20000084,2,1)&&!native_write(0x20000092,2,0x8001));
    assert(native_write(0x20000004,2,0x406)&&native_write(0x20000010,4,0x22000004));
    /* The recorded region-zero target, not the claim, selects the function. */
    port.windows[0].pci|=1u<<16;assert(!native_write(0x20000004,2,0x406));port.windows[0].pci&=~(1u<<16);
    assert(!native_write(0x06000020,4,0x22102200)&&!native_write(0x06000820,4,0)&&
        !native_write(0x06400e00,4,1u<<5)&&!native_write(0x06400c00,4,0x40)&&!native_write(0x06300214,4,0x41000000));
    assert(!a733_pcie_nvme_read(&nvme,0x100000-7)&&!a733_pcie_nvme_read(&nvme,UINT64_MAX));
    read_back(16);
}
static int refuse(const void *d,unsigned b,uint32_t o,uint32_t v){(void)d;(void)b;(void)o;(void)v;return 0;}
static void function_case(const char *mode)
{
    /* The general layer alone with a refusing driver write rule. */
    static const struct a733_pcie_function_config base={.identity=94,.vendor=0x144d,.device=0xa808,
        .class_code=0x010802,.irq=A733_PCIE_IRQ_MSIX,.vectors=2,.write_allowed=NULL};
    struct a733_pcie_function_config c=base;
    c.write_allowed=refuse;
    if(!strcmp(mode,"suspend")||!strcmp(mode,"suspend-keep")){
        no_soft_reset=!strcmp(mode,"suspend-keep");device();memset(&plain,0,sizeof(plain));
        assert(a733_pcie_function_init(&plain,&port,&c,&memory,&irq));
        assert(until(function_poll,&plain,A733_PCIE_READY)==A733_PCIE_READY);
        uint32_t events=7;assert(a733_pcie_function_begin(&plain,port.config.lifetime_deadline,&events)==A733_PCIE_READY&&!events);
        assert(!a733_pcie_function_write(&plain,0,0x14,1)&&a733_pcie_function_master(&plain,1));
        assert(!a733_pcie_function_suspend(&plain)&&a733_pcie_function_end(&plain,1)==A733_PCIE_READY);
        assert(!a733_pcie_function_suspend(&plain));
        assert(a733_pcie_function_begin(&plain,port.config.lifetime_deadline,&events)==A733_PCIE_READY);
        assert(a733_pcie_function_master(&plain,0)&&a733_pcie_function_end(&plain,1)==A733_PCIE_READY);
        assert(a733_pcie_function_suspend(&plain));
        for(unsigned i=0;i<200&&!(plain.stage==9);++i)assert(advance(function_poll,&plain)==A733_PCIE_PENDING);
        assert(d3&&plain.stage==9&&plain.soft_reset==!no_soft_reset);
        assert(a733_pcie_function_begin(&plain,port.config.lifetime_deadline,&events)==A733_PCIE_INVALID);
        assert(a733_pcie_function_resume(&plain));
        assert(until(function_poll,&plain,A733_PCIE_READY)==A733_PCIE_READY&&!d3&&d3_writes==2);
        /* Restored or retained: identical BARs, decode and both MSI-X entries. */
        assert(*w(0x20000010)==0x22000004&&*w(0x20000018)==0x22004000&&command16()==0x402);
        assert(table[0][0]==*w(0x06000820)&&table[1][2]==1&&!(table[1][3]&1)&&(table[2][3]&1));
        assert((*w(0x20000090)>>16&0xc000)==0x8000);
        assert(a733_pcie_function_stop(&plain));
        assert(until(function_poll,&plain,A733_PCIE_STOPPED)==A733_PCIE_STOPPED);
        enum a733_pcie_result r=A733_PCIE_PENDING;
        for(unsigned i=0;i<100&&r==A733_PCIE_PENDING;++i){r=a733_pcie_stop(&port,port.config.lifetime_deadline);model.now+=100;}
        assert(r==A733_PCIE_STOPPED);stopped();return;
    }
    if(!strcmp(mode,"multifunction")){
        siblings=1u<<3;sibling[3][0]=0x0001144d;sibling[3][2]=0x08800000;device();
        memset(&plain,0,sizeof(plain));assert(a733_pcie_function_init(&plain,&port,&c,&memory,&irq));
        assert(until(function_poll,&plain,A733_PCIE_READY)==A733_PCIE_READY);
        assert(plain.present==((1u<<0)|(1u<<3))&&plain.ids[3].device==1&&target_function()==0);
        port.windows[0].pci=0x01030000;assert(!native_write(0x20000004,2,0x402));port.windows[0].pci=0x01000000;
        assert(native_write(0x20000004,2,0x402));
        return;
    }
    if(!strcmp(mode,"bridge")){siblings=1u<<1;sibling[1][0]=0x0002144d;sibling[1][3]=0x00010000;}
    else if(!strcmp(mode,"warm-sibling")){siblings=1u<<2;sibling[2][0]=0x0002144d;sibling[2][1]=2;}
    else if(!strcmp(mode,"large-bar"))large_bar=1;
    else if(!strcmp(mode,"assigned-bar"))assigned_bar=1;
    else if(!strcmp(mode,"table-outside"))table_outside=1;
    else if(!strcmp(mode,"class"))class_mismatch=1;
    else assert(0);
    device();memset(&plain,0,sizeof(plain));
    assert(a733_pcie_function_init(&plain,&port,&c,&memory,&irq));
    assert(until(function_poll,&plain,A733_PCIE_READY)==A733_PCIE_RETAINED&&plain.failed&&port.failed);
    /* No rollback: a late refusal leaves memory decode as it was. */
    assert(command16()==(strcmp(mode,"table-outside")?0u:0x402u)&&irq_stopped&&!irq_armed);
    enum device_memory_state state;struct device_memory_extent e;
    assert(device_memory_describe(&memory,plain.msi_lease,&e,&state)&&state==DEVICE_MEMORY_RETAINED);
    if(strcmp(mode,"table-outside"))assert(!(*w(0x06300204)&(1u<<31)));
}
static void port_case(const char *mode)
{
    /* Config-only stop, the endpoint test client's refusal and a link that stays up. */
    setup();assert(complete()==A733_PCIE_READY);
    port.ops.read=ep_read;port.ops.write=ep_write;
    if(!strcmp(mode,"stop-test")){
        assert(a733_pcie_test_claim(&port,(struct a733_pcie_epf_test *)&model)==A733_PCIE_READY);
        unsigned writes=trace_count;
        assert(a733_pcie_stop(&port,port.config.lifetime_deadline)==A733_PCIE_INVALID&&trace_count==writes);
        assert(*w(0x02000210)&(1u<<22));return;
    }
    link_stuck=!strcmp(mode,"stop-link-stuck");
    enum a733_pcie_result r=A733_PCIE_PENDING;uint64_t deadline=model.now+5000;
    for(unsigned i=0;i<200&&r==A733_PCIE_PENDING;++i){r=a733_pcie_stop(&port,deadline);model.now+=100;}
    if(link_stuck){
        assert(r==A733_PCIE_RETAINED&&port.failed&&port.phase==18);
        assert(a733_pcie_stop(&port,port.config.lifetime_deadline)==A733_PCIE_RETAINED);return;
    }
    assert(r==A733_PCIE_STOPPED&&port.phase==19);
    assert(!(*w(0x02000210)&(1u<<22))&&!(*w(0x06400c00)&1)&&!(*w(0x06300004)&(1u<<31)));
    assert(a733_pcie_stop(&port,deadline)==A733_PCIE_STOPPED&&a733_pcie_poll(&port)==A733_PCIE_STOPPED);
    struct a733_pcie_endpoint id;assert(a733_pcie_identity(&port,&id)==A733_PCIE_STOPPED);
}
int main(int argc,char **argv)
{
    assert(argc==2);
    assert(mmap((void *)(uintptr_t)MEMORY,2*1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
    const char *mode=argv[1];
    if(!strcmp(mode,"poll"))nominal(A733_PCIE_IRQ_POLL);
    else if(!strcmp(mode,"msix"))nominal(A733_PCIE_IRQ_MSIX);
    else if(!strcmp(mode,"intx"))nominal(A733_PCIE_IRQ_INTX);
    else if(!strcmp(mode,"gen2")){link_speed=negotiated=2;nominal(A733_PCIE_IRQ_MSIX);assert(port.endpoint.link_status==0x12);}
    else if(!strcmp(mode,"forged"))forged();
    else if(!strcmp(mode,"inherited")){inherited=1;start(A733_PCIE_IRQ_POLL);refused(0);assert(nvme.stage==1&&!dma_count);}
    else if(!strcmp(mode,"fatal")){fatal_at_enable=1;start(A733_PCIE_IRQ_POLL);refused(1);assert(!dma_count);}
    else if(!strcmp(mode,"identify-error")){identify_error=1;start(A733_PCIE_IRQ_POLL);refused(1);}
    else if(!strcmp(mode,"timeout")){drop=1;start(A733_PCIE_IRQ_POLL);refused(1);assert(nvme.pending);}
    else if(!strcmp(mode,"extra-vector")){
        start(A733_PCIE_IRQ_MSIX);assert(until(nvme_poll,&nvme,A733_PCIE_READY)==A733_PCIE_READY);
        *w(0x06000830)|=2;refused(1);
    }
    else if(!strcmp(mode,"extra-intx")){
        start(A733_PCIE_IRQ_INTX);assert(until(nvme_poll,&nvme,A733_PCIE_READY)==A733_PCIE_READY);
        *w(0x06400e08)|=1u<<6;refused(1);
    }
    else if(!strcmp(mode,"shutdown-stuck")){
        shst_stuck=1;start(A733_PCIE_IRQ_POLL);assert(until(nvme_poll,&nvme,A733_PCIE_READY)==A733_PCIE_READY);
        assert(a733_pcie_nvme_shutdown(&nvme));
        assert(until(nvme_poll,&nvme,A733_PCIE_STOPPED)==A733_PCIE_RETAINED&&!port.client_stopped);
        retained_memory();assert((command16()&4)&&(*w(0x02000210)&(1u<<22)));
    }
    else if(!strncmp(mode,"stop-",5))port_case(mode);
    else function_case(mode);
    printf("PASS PCIe function/NVMe model: %s (endpoint, controller and interrupts modeled)\n",mode);
    return 0;
}
