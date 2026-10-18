/* SPDX-License-Identifier: Apache-2.0 */
#include "a733_usb2_native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int usb_gpu_init(struct a733_gpu_resource_manager *,const struct a733_gpu_resource_ops *,void *);
#define PMIC_MODEL_NO_MAIN
#define a733_gpu_resources_init usb_gpu_init
#include "a733-pmic.c"
#undef a733_gpu_resources_init
static struct pmic_model m;
static struct a733_usb2_native n;
static struct a733_typec role;
static struct a733_twi bus;
static _Alignas(4096) uint8_t dma[A733_XHCI_MEMORY_BYTES];
static uint32_t registers[0x8000/4];
static unsigned reg_fault,lose_on_lock,platform_writes,commands,transfers,events,event_cycle=1;
static unsigned bad_status,hold_bulk,short_data,bad_config,short_descriptor;
static uint64_t last_command,last_transfer[3];
static uint8_t request[8],cbw[31];
static int gpu_read(void *v,uintptr_t a,uint32_t *out){*out=*pmic_model_word(v,a);return 1;}
static int gpu_update(void *v,uintptr_t a,uint32_t required,uint32_t set,uint64_t d)
{struct pmic_model *p=v;assert(a==0x070101ac&&set==1&&p->now<d);uint32_t *w=pmic_model_word(p,a);assert((*w&required)==required);*w|=set;return 1;}
static int usb_gpu_init(struct a733_gpu_resource_manager *g,const struct a733_gpu_resource_ops *unused,void *v)
{(void)unused;const struct a733_gpu_resource_ops o={pmic_model_now,gpu_read,gpu_update,pmic_model_gpu_rail};return a733_gpu_resources_init(g,&o,v);}
static int current(void *v){(void)v;return !m.worker;}
static int admit(void *v,const struct a733_usb2_platform_config *c){assert(v==&m&&c->identity==88&&c->topology==12);return 1;}
static int lock(void *v,uint64_t d)
{struct pmic_model *p=v;if(p->register_held||reg_fault||p->now>=d)return 0;p->register_held=1;if(lose_on_lock){lose_on_lock=0;a733_typec_revoke(&role);}return 1;}
static void unlock(void *v){struct pmic_model *p=v;assert(p->register_held&&!reg_fault);p->register_held=0;}
static void retain(void *v){assert(((struct pmic_model *)v)->register_held);reg_fault=1;}
static int read_reg(void *v,uintptr_t a,uint32_t *out)
{assert(m.register_held&&!reg_fault&&a733_usb2_platform_address(a,0));*out=*pmic_model_word(v,a);return 1;}
static int write_reg(void *v,uintptr_t a,uint32_t value)
{assert(m.register_held&&!reg_fault&&a733_usb2_platform_address(a,1));uint32_t *p=pmic_model_word(v,a);assert(!((*p^value)&~a733_usb2_platform_mask(a)));*p=value;++platform_writes;return 1;}
static int unused_prepare(void *v,uint64_t d){(void)v;(void)d;assert(0);return -1;}
static int unused_vbus(void *v,unsigned on,uint64_t d){(void)v;(void)on;(void)d;assert(0);return -1;}
static int unused_irq(void *v,unsigned *out,uint64_t d){(void)v;(void)out;(void)d;assert(0);return -1;}
static void observe_role(void)
{
    /* Prepared-observation checkpoint: Type-C engine/physical power are not
     * requalified here. Exercise its actual public observation and revocation. */
    if(!atomic_load(&role.revoked))role.published=(struct a733_typec_token){(uintptr_t)&role,70,71,m.now,1};
}
static void setup(void)
{
    pmic_model_setup(&m);
    m.pmic[0x11]=0x11;m.pmic[0x1a]=30;m.pmic[0x23]=8;m.pmic[0x3f]=12;m.pmic[0x33]=13;
    *pmic_model_word(&m,0x02002000)=0xf8636300;
    *pmic_model_word(&m,0x020020a0)=0xfe106310;
    *pmic_model_word(&m,0x02003908)=0x0b860b86;*pmic_model_word(&m,0x02003988)=0x0b860000;
    *pmic_model_word(&m,0x02002580)=0x83000002;*pmic_model_word(&m,0x02002588)=0x84000001;
    *pmic_model_word(&m,0x020025c0)=1u<<8;*pmic_model_word(&m,0x020025e0)=1u<<28;
    *pmic_model_word(&m,0x020033c0)=0x81000005;*pmic_model_word(&m,0x020033c4)=1u<<16;
    *pmic_model_word(&m,0x02003b28)=0x20000;
    *pmic_model_word(&m,0x07067000)=*pmic_model_word(&m,0x07067008)=8;
    *pmic_model_word(&m,0x07068000)=*pmic_model_word(&m,0x07068008)=8;
    *pmic_model_word(&m,0x06b00010)=0x22418;
    *pmic_model_word(&m,0x06a0c120)=0x3331190a;*pmic_model_word(&m,0x06a0c1a0)=0x3139302a;
    *pmic_model_word(&m,0x06a0c140)=2;*pmic_model_word(&m,0x06a0c144)=1u<<24;
    *pmic_model_word(&m,0x06a0c14c)=4;
    const struct a733_typec_config tc={.ops={pmic_model_outside,pmic_model_now,current,unused_prepare,current,unused_vbus,unused_irq},
        .context=&m,.bus=&bus,.owner=70,.generation=71,.qualification=72,.recovery=73,
        .source_min_mv=5000,.source_max_mv=5100,.source_ma=500,.zero_threshold_max_mv=800,.device_id=0x80};
    assert(a733_typec_init(&role,&tc));observe_role();
    struct a733_usb2_platform_config c={.owner=99,.identity=88,.topology=12,.deadline=2000000,.lifetime_deadline=10000000,
        .pck_client=2,.fabric={0,0x83000002,0x84000001},.fabric_hz={24000000,200000000,200000000},
        .supply_uv={3300000,3300000,800000,800000,1800000},.revision=0x3139302a};
    const struct a733_usb2_platform_ops o={pmic_model_now,current,admit,lock,unlock,retain,read_reg,write_reg,pmic_model_outside};
    n.self=&n;n.initialized=1;
    assert(a733_usb2_platform_init(&n.platform,&c,&o,&m,&m.clocks,&m.rails,&m.provider,&m.gpu,&role));
}
extern void usb2_native_mapping_check(void);
extern void usb2_source_reference(uint32_t *);
static void prepare(void)
{
    for(unsigned i=0;i<10000;++i){observe_role();enum a733_usb2_result r=a733_usb2_platform_poll(&n.platform);
        if(r==A733_USB2_READY)return;
        if(r==A733_USB2_RETAINED)fprintf(stderr,"platform phase%u now%llu writes%u\n",n.platform.phase,(unsigned long long)m.now,platform_writes);
        assert(r!=A733_USB2_RETAINED);assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);m.now+=20;}
    assert(0);
}
static uint32_t get32(const void *v){const uint8_t *p=v;return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(void *v,uint32_t x){uint8_t *p=v;for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(x>>(8*i));}
static void event(unsigned type,uint64_t pointer,unsigned code,unsigned remaining,unsigned endpoint,unsigned slot)
{
    struct a733_xhci_trb *t=&n.core.events[events];assert((t->control&1)!=event_cycle);
    *t=(struct a733_xhci_trb){(uint32_t)pointer,(uint32_t)(pointer>>32),(code<<24)|remaining,
        (slot<<24)|(endpoint<<16)|(type<<10)|event_cycle};
    if(++events==A733_XHCI_EVENT_TRBS){events=0;event_cycle^=1;}
}
static void control_data(uint8_t *p,unsigned count)
{
    memset(p,0,count);
    static const uint8_t device[]={18,1,0,2,0,0,0,64,0x34,0x12,0x78,0x56,0,1,0,0,0,1};
    static const uint8_t config[]={9,2,32,0,1,1,0,0x80,50,9,4,0,0,2,8,6,0x50,0,7,5,0x81,2,0,2,0,7,5,2,2,0,2,0};
    if(request[1]==6){static const uint8_t string[]={4,3,9,4};const uint8_t *src=request[3]==1?device:request[3]==2?config:string;unsigned z=request[3]==1?sizeof(device):request[3]==2?sizeof(config):sizeof(string);assert(count<=z);memcpy(p,src,count);if(bad_config&&request[3]==2&&count==32){p[2]=255;p[3]=255;}}
    else assert(request[1]==0xfe&&count==1); /* Actual GET_MAX_LUN response0. */
}
static void wire(void)
{
    struct a733_xhci *x=&n.core;
    if(x->command_kind&&x->command_pointer!=last_command){
        last_command=x->command_pointer;++commands;
        if(x->command_kind==11){memcpy(x->device,x->input+x->context_bytes,2*x->context_bytes);unsigned state=x->pending_address?2:1;put32(x->device+12,(state<<27)|(state==2?7:0));}
        else if(x->command_kind==12){assert((get32(x->input+x->context_bytes)&(15u<<20))==(3u<<20));memcpy(x->device,x->input+x->context_bytes,x->context_bytes);put32(x->device+12,(3u<<27)|7);}
        event(33,x->command_pointer,1,0,0,1);return;
    }
    for(unsigned i=0;i<3;++i){struct a733_xhci_endpoint *e=&x->endpoints[i];
        if(e->pending!=2||!e->pending_trb||e->pending_trb==last_transfer[i])continue;
        if(i&&hold_bulk)return;
        last_transfer[i]=e->pending_trb;++transfers;unsigned residual=0;
        if(!i){if(e->kind==2)memcpy(request,x->setup,8);else if(e->kind==3&&e->direction){control_data(e->bounce,e->length);if(short_descriptor&&request[1]==6&&request[3]==1)residual=1;}}
        else if(!e->direction){assert(e->length==31);memcpy(cbw,e->bounce,31);}
        else if(e->length==13){memset(e->bounce,0,13);put32(e->bounce,0x53425355);put32(e->bounce+4,get32(cbw+4)+(bad_status?1:0));}
        else if(cbw[15]==0x25){assert(e->length==8);const uint8_t capacity[]={0,0,0,127,0,0,2,0};memcpy(e->bounce,capacity,8);}
        else if(cbw[15]==0x28){assert(e->length==512);for(unsigned j=0;j<512;++j)e->bounce[j]=(uint8_t)(j*17+3);if(short_data)residual=1;}
        else{assert(cbw[15]==3||cbw[15]==0x12);memset(e->bounce,0,e->length);}
        event(32,e->pending_trb,residual?13:1,residual,e->number,x->slot);return;
    }
}
static int cn(void *v,uint64_t *out){(void)v;*out=m.now;return 1;}
static int live(void *v,uint64_t g,uint64_t d){(void)v;return a733_usb2_platform_live(&n.platform,g,d);}
static int rr(void *v,uint32_t off,uint32_t *out){(void)v;assert(off<0x8000&&!(off&3));*out=registers[off/4];return 1;}
static int ww(void *v,uint32_t off,uint32_t value)
{
    (void)v;assert(off<0x8000&&!(off&3));registers[off/4]=value;
    if(off==0x40){if(value&2){registers[off/4]=0;registers[0x44/4]=1;}else if(value&1)registers[0x44/4]=0;}
    if(off==0x440){registers[off/4]=1|(1u<<9);if(value&(1u<<4))registers[off/4]|=2|(3u<<10);}
    return 1;
}
static int w64(void *v,uint32_t off,uint64_t value){assert(!(off&7));return ww(v,off,(uint32_t)value)&&ww(v,off+4,(uint32_t)(value>>32));}
static int barrier(void *v){(void)v;return 1;}
static int out(void *v,const void *p,size_t z){(void)v;return pmic_model_outside(&m,p,z);}
static void start_core(void)
{
    struct device_memory_extent e;
    assert(device_memory_pool_init(&n.private_pool,(uintptr_t)dma,0x79000000,sizeof(dma)));
    assert(device_memory_acquire(&n.private_pool,99,sizeof(dma),&n.memory,&e));
    const struct a733_xhci_ops ops={cn,current,live,rr,ww,w64,barrier,out,a733_usb2_host_notice};
    registers[0]=0x01100040;registers[1]=(1u<<24)|(1u<<8)|1;
    registers[4]=(0x20u<<16)|1|4|8;registers[5]=0x2000;registers[6]=0x1000;
    registers[0x80/4]=0x02000002;registers[0x84/4]=0x20425355;registers[0x88/4]=0x101;
    registers[0x44/4]=1;registers[0x48/4]=1;registers[0x440/4]=1;
    n.core_started=1;assert(a733_xhci_init(&n.core,&ops,&n,&n.private_pool,n.memory,71,2000000,10000000));
    assert(a733_usb2_host_bind(&n));n.stack_started=1;
}
static int poll(void)
{m.now+=100;observe_role();wire();return a733_usb2_host_poll(&n);}
static void mounted(void)
{for(unsigned i=0;i<10000&&!n.mounted&&!n.failed;++i)poll();assert(n.mounted&&!n.failed&&n.blocks==128&&n.block_bytes==512&&commands==4);}
int main(int argc,char **argv)
{
    const char *mode=argc>1?argv[1]:"read";if(!strcmp(mode,"admission")){usb2_native_mapping_check();puts("PASS exact native NC mapping admission accepts shareability2/3 and rejects invalid spans");return 0;}setup();
    if(!strcmp(mode,"mutex-loss")){
        while(n.platform.phase<8){observe_role();assert(a733_usb2_platform_poll(&n.platform)!=A733_USB2_RETAINED);assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);m.now+=20;}
        lose_on_lock=1;assert(a733_usb2_platform_poll(&n.platform)==A733_USB2_RETAINED);
        assert(!platform_writes&&reg_fault&&n.platform.clock.id&&n.platform.rail.id);puts("PASS Type-C revocation during register acquisition prevents all USB MMIO writes");return 0;}
    prepare();assert(platform_writes&&*pmic_model_word(&m,0x02003364)==0x81000003);
    uint32_t reference[2];usb2_source_reference(reference);
    assert(*pmic_model_word(&m,0x06a0c12c)==reference[0]&&*pmic_model_word(&m,0x06a0c630)==reference[1]);
    start_core();
    if(!strcmp(mode,"bad-config")||!strcmp(mode,"short-descriptor")){
        bad_config=!strcmp(mode,"bad-config");short_descriptor=!strcmp(mode,"short-descriptor");
        for(unsigned i=0;i<10000&&!n.failed;++i)poll();
        assert(n.failed&&n.core.failed&&!n.mounted);printf("PASS %s refused before TinyUSB parser\n",mode);return 0;}
    mounted();uint8_t result[512];memset(result,0x5a,sizeof(result));
    assert(a733_usb2_read_sector(&n,7,m.now+500000));
    if(!strcmp(mode,"bad-status"))bad_status=1;
    if(!strcmp(mode,"short-data"))short_data=1;
    if(!strcmp(mode,"cancel")){
        hold_bulk=1;for(unsigned i=0;i<10;++i)assert(poll()>=0);
        assert(a733_xhci_abort(&n.core,n.address,2));assert(n.core.failed);
    }else if(!strcmp(mode,"detach")){a733_typec_revoke(&role);assert(a733_usb2_host_poll(&n)<0);}
    else for(unsigned i=0;i<10000&&!n.read_complete&&!n.failed;++i)poll();
    if(strcmp(mode,"read")){
        (void)a733_usb2_host_poll(&n);assert(n.failed&&n.core.failed);
        assert(a733_usb2_read_result(&n,result)<0);for(unsigned j=0;j<512;++j)assert(result[j]==0x5a);
        enum device_memory_state s;struct device_memory_extent e;assert(device_memory_describe(&n.private_pool,n.memory,&e,&s)&&s==DEVICE_MEMORY_RETAINED);
        printf("PASS %s retains DMA and refuses caller output\n",mode);return 0;}
    assert(n.read_complete&&!n.failed&&a733_usb2_read_result(&n,result)==1);
    for(unsigned j=0;j<512;++j)assert(result[j]==(uint8_t)(j*17+3));
    assert(n.core.endpoints[0].cycle==0); /* Enumeration crosses the control Link TRB. */
    printf("PASS actual PMIC/clock/role observation, xHCI%u commands/%u transfers, TinyUSB MSC enumeration and sector read\n",commands,transfers);
    return 0;
}
