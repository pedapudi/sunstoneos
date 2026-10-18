#include "a733_pcie_native.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int pcie_gpu_init(struct a733_gpu_resource_manager *,const struct a733_gpu_resource_ops *,void *);
static int pcie_pins_init(struct a733_pins *,const struct a733_pins_operations *,void *,const struct a733_pin_handoff *);
#define PMIC_MODEL_NO_MAIN
#define a733_gpu_resources_init pcie_gpu_init
#define a733_pins_init_banks pcie_pins_init
#include "a733-pmic.c"
#undef a733_gpu_resources_init
#undef a733_pins_init_banks
static struct pmic_model model;
static struct a733_pcie port;
static struct { uintptr_t address; unsigned width; uint32_t value; } trace[2048];
static unsigned trace_count,reg_retained,stall_phy,stall_atu,bad_caps,late_fault,raise_before_adopt;
static uint64_t phy_done,reset_rise;
static int pcie_gpu_read(void *v,uintptr_t a,uint32_t *out)
{struct pmic_model *m=v;*out=*pmic_model_word(m,a);return 1;}
static int pcie_gpu_update(void *v,uintptr_t a,uint32_t required,uint32_t set,uint64_t deadline)
{
    struct pmic_model *m=v;assert(a==0x070101ac&&set==1);
    assert(!m->register_held&&!m->i2c_held&&m->now<deadline);
    uint32_t *word=pmic_model_word(m,a);assert((*word&required)==required);*word|=set;return 1;
}
static int pcie_gpu_init(struct a733_gpu_resource_manager *g,const struct a733_gpu_resource_ops *ignored,void *v)
{
    (void)ignored;const struct a733_gpu_resource_ops o={pmic_model_now,pcie_gpu_read,pcie_gpu_update,pmic_model_gpu_rail};
    return a733_gpu_resources_init(g,&o,v);
}
static int pcie_pin_write(void *v,uintptr_t a,uint32_t value)
{
    struct pmic_model *m=v;
    if(a==0x02000214||a==0x02000218) {
        uint32_t *data=pmic_model_word(m,0x02000210);
        if(a==0x02000214)*data|=value;else *data&=~value;
        if(a==0x02000214&&(value&(1u<<22))) {
            reset_rise=m->now;
            assert(phy_done&&reset_rise-phy_done>=100000);
        }
    }
    return pmic_model_pin_write(v,a,value);
}
static int pcie_pin_enter(void *v,uint64_t deadline)
{
    if(raise_before_adopt&&port.supply_index==2&&!port.pin.id) {
        *pmic_model_word(v,0x02000210)|=1u<<22;raise_before_adopt=0;
    }
    return pmic_model_pin_enter(v,deadline);
}
static int pcie_pins_init(struct a733_pins *p,const struct a733_pins_operations *source,void *v,const struct a733_pin_handoff *h)
{struct a733_pins_operations o=*source;o.write=pcie_pin_write;o.enter=pcie_pin_enter;return a733_pins_init_banks(p,&o,v,h);}
static int pcie_current(void *v){return !((struct pmic_model *)v)->worker;}
static int pcie_admit(void *v,const struct a733_pcie_config *c)
{assert(v==&model&&c->identity==88&&c->topology==12&&!trace_count);return 1;}
static int pcie_enter(void *v,uint64_t deadline)
{
    struct pmic_model *m=v;assert(!m->worker);
    if(reg_retained||m->register_held||m->now>=deadline)return 0;
    m->register_held=1;return 1;
}
static void pcie_leave(void *v)
{struct pmic_model *m=v;assert(m->register_held&&!reg_retained);m->register_held=0;}
static void pcie_retain(void *v)
{assert(((struct pmic_model *)v)->register_held);reg_retained=1;}
static int pcie_read(void *v,uintptr_t a,unsigned width,uint32_t *out)
{
    struct pmic_model *m=v;assert(m->register_held&&!reg_retained);
    assert(a733_pcie_native_address(a,width,0));
    uint32_t word=*pmic_model_word(m,a&~(uintptr_t)3);
    *out=word>>((a&3)*8);
    if(width==1)*out&=255;if(width==2)*out&=65535;
    if(a==0x20000070&&bad_caps)*out=0x00707010;
    if(late_fault&&a==0x02003b28&&port.endpoint.vendor) {
        atomic_store_explicit(&m->clocks.failed,1,memory_order_release);late_fault=0;
    }
    return 1;
}
static int pcie_write(void *v,uintptr_t a,unsigned width,uint32_t value)
{
    struct pmic_model *m=v;assert(m->register_held&&!reg_retained);
    assert(a733_pcie_native_address(a,width,1)&&trace_count<2048);
    trace[trace_count++]=(typeof(trace[0])){a,width,value};
    uint32_t *word=pmic_model_word(m,a&~(uintptr_t)3);
    uint32_t mask=width==4?UINT32_MAX:(width==2?65535u:255u)<<((a&3)*8);
    *word=(*word&~mask)|(value<<((a&3)*8));
    if(a==0x03400194)*pmic_model_word(m,0x03400114)&=~value;
    if(a==0x06c02100&&(value&1)&&!stall_phy)*pmic_model_word(m,0x06c02900)=1;
    if(a==0x06c02004&&(value&(1u<<28)))phy_done=m->now;
    if(a==0x06400c00&&(value&1)) {
        assert(reset_rise&&m->now>=reset_rise);
        *pmic_model_word(m,0x06400e0c)=3;
        *pmic_model_word(m,0x06000080)=0x00110000;
    }
    if(a==0x06300004&&stall_atu)*word=0;
    return 1;
}
static void setup(void)
{
    memset(&port,0,sizeof(port));trace_count=reg_retained=stall_phy=stall_atu=bad_caps=late_fault=raise_before_adopt=0;
    phy_done=reset_rise=0;pmic_model_setup(&model);
    model.pmic[0x20]=0x80;model.pmic[0x2b]=13;
    *pmic_model_word(&model,0x02002000)=0xf8636300;
    *pmic_model_word(&model,0x020020a0)=0xfe106300;
    *pmic_model_word(&model,0x02003908)=0x0a060a06;
    *pmic_model_word(&model,0x02003988)=0x0a060000;
    *pmic_model_word(&model,0x02002580)=0x83000002;
    *pmic_model_word(&model,0x02002588)=0x84000001;
    *pmic_model_word(&model,0x020025c0)=1u<<8;
    *pmic_model_word(&model,0x020025e0)=1u<<28;
    *pmic_model_word(&model,0x020033c0)=0x81000005;
    *pmic_model_word(&model,0x020033c4)=1u<<16;
    *pmic_model_word(&model,0x02003b28)=0x20000;
    *pmic_model_word(&model,0x07067000)=8;*pmic_model_word(&model,0x07067008)=8;
    *pmic_model_word(&model,0x02000208)=0x01000000;
    *pmic_model_word(&model,0x06000004)=1u<<20;
    *pmic_model_word(&model,0x06000034)=0x70;
    *pmic_model_word(&model,0x06000070)=0x00420010;
    *pmic_model_word(&model,0x20000000)=0x56781234;
    *pmic_model_word(&model,0x20000004)=1u<<20;
    *pmic_model_word(&model,0x20000008)=0xff000001;
    *pmic_model_word(&model,0x20000034)=0x70;
    *pmic_model_word(&model,0x20000070)=0x00029010;
    *pmic_model_word(&model,0x20000090)=5;
    struct a733_pcie_config c={.owner=99,.identity=88,.topology=12,.deadline=900000,
        .lifetime_deadline=10000000,.pck_client=2,.inherited={{0,0,0},{0,0,0},{1,0,0},{0,0,0}},
        .fabric={0,0x83000002,0x84000001},.fabric_hz={24000000,200000000,200000000}};
    const struct a733_pcie_ops ops={pmic_model_now,pcie_current,pcie_admit,pcie_enter,pcie_leave,
        pcie_retain,pcie_read,pcie_write,pmic_model_outside};
    assert(a733_pcie_init(&port,&c,&ops,&model,&model.clocks,&model.pins,&model.rails,&model.provider,&model.gpu));
}
static enum a733_pcie_result step(void)
{
    enum a733_pcie_result r=a733_pcie_poll(&port);
    if(r!=A733_PCIE_RETAINED)assert(pmic_model_step(&model)!=A733_PMIC_RETAINED);
    model.now+=100;return r;
}
static enum a733_pcie_result complete(void)
{
    for(unsigned i=0;i<10000;++i) {
        enum a733_pcie_result r=step();
        if(r==A733_PCIE_RETAINED)fprintf(stderr,"retained phase=%u value=%08x now=%llu writes=%u pins=%llu\n",
            port.phase,port.last_value,(unsigned long long)model.now,trace_count,(unsigned long long)port.pin.id);
        if(r==A733_PCIE_READY||r==A733_PCIE_RETAINED)return r;
    }
    assert(0);return A733_PCIE_INVALID;
}
static void retained(void)
{
    assert(complete()==A733_PCIE_RETAINED&&port.failed&&port.clock.id&&port.rail.id);
    unsigned writes=trace_count;assert(a733_pcie_poll(&port)==A733_PCIE_RETAINED&&trace_count==writes);
}
static void decode(void)
{
    const unsigned codes[]={0,13,29,30,31};
    for(unsigned i=0;i<5;++i)for(unsigned enable=0;enable<3;++enable) {
        uint8_t raw=enable==0?0:enable==1?0x80:0x7f;
        struct a733_pmic_snapshot s;
        assert(a733_pmic_decode(A733_RAIL_BLDO2,raw,codes[i],&s));
        assert(s.enable==raw&&s.selector==codes[i]&&s.enabled==(enable==1));
        assert(s.decoded==(codes[i]<=29)&&s.programmed_uv==(codes[i]<=29?(500+100*codes[i])*1000:0));
    }
}
static void scopes(void)
{
    struct a733_clock_leases m={0};struct a733_clock_token a={0},b={0},out={0};
    assert(a733_clock_leases_init(&m,pmic_model_now,&model));
    assert(a733_clock_acquire(&m,1,A733_CLOCK_PCIE0,0,10000000,&a)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&m,1,A733_CLOCK_COMB1,0,10000000,&a)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&m,1,0,UINT64_C(1)<<63,10000000,&a)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&m,1,A733_CLOCK_SERDES_COMMON|A733_CLOCK_USB_PCIE_DOMAIN,
        A733_CLOCK_PCIE0|A733_CLOCK_COMB1|A733_CLOCK_SDC3,10000000,&a)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&m,2,A733_CLOCK_SERDES_COMMON|A733_CLOCK_USB_PCIE_DOMAIN,0,10000000,&b)==A733_CLOCK_OK);
    const a733_clock_mask_t conflict[]={A733_CLOCK_SERDES_COMMON,A733_CLOCK_USB_PCIE_DOMAIN,A733_CLOCK_PCIE0,A733_CLOCK_COMB1};
    for(unsigned i=0;i<4;++i)assert(a733_clock_acquire(&m,3,0,conflict[i],10000000,&out)==A733_CLOCK_BUSY&&!out.id);
    assert(a733_clock_retain(&m,&a)==A733_CLOCK_FAULT&&atomic_load_explicit(&m.failed,memory_order_acquire));
    assert(a733_clock_token_status(&m,&b)==A733_CLOCK_FAULT);
    assert(a733_clock_publish_stable(&m,&a,A733_CLOCK_PLL_NPU,10000000,&out)==A733_CLOCK_FAULT&&!out.id);
}
static void pin_conditions(void)
{
    setup();
    struct a733_pin_request_banked q={.bank=A733_PIN_D,.pin=22,.expected={1,0,0},.desired={1,0,0}};
    struct a733_pin_lease_banked token={0};
    struct a733_pin_output_condition c={.levels={[A733_PIN_D]=1u<<22}};
    assert(a733_pins_acquire_banked_checked(&model.pins,55,&q,1,&c,&token,900000)==A733_PINS_INVALID&&!token.id);
    c=(struct a733_pin_output_condition){.masks={[A733_PIN_D]=1u<<21}};
    assert(a733_pins_acquire_banked_checked(&model.pins,55,&q,1,&c,&token,900000)==A733_PINS_INVALID&&!token.id);
    assert(a733_pins_acquire_banked_checked(&model.pins,55,&q,1,
        (const struct a733_pin_output_condition *)&model.pins,&token,900000)==A733_PINS_INVALID&&!token.id);
    c=(struct a733_pin_output_condition){.masks={[A733_PIN_D]=1u<<22}};
    *pmic_model_word(&model,0x02000210)=1u<<22;
    assert(a733_pins_acquire_banked_checked(&model.pins,55,&q,1,&c,&token,900000)==A733_PINS_CONFLICT&&!token.id);
    assert(*pmic_model_word(&model,0x02000210)==(1u<<22)&&!model.pins.pins[A733_PIN_D][22].id);
}
int main(int argc,char **argv)
{
    setup();assert(complete()==A733_PCIE_READY);
    assert(port.supplies[1].rail==A733_RAIL_BLDO2&&port.supplies[1].programmed_uv==1800000);
    assert(port.supplies[0].programmed_uv==800000&&reset_rise-phy_done>=100000);
    if(argc==2&&!strcmp(argv[1],"--trace")) {
        for(unsigned i=0;i<trace_count;++i)if(a733_pcie_phy_address(trace[i].address,trace[i].width,1)||
            (trace[i].address>=0x06300000&&trace[i].address<=0x06300018))
            printf("%08lx %u %08x\n",(unsigned long)trace[i].address,trace[i].width,trace[i].value);
    }
    assert(argc==1||(argc==2&&!strcmp(argv[1],"--trace")));
    struct a733_pcie_endpoint id;assert(a733_pcie_identity(&port,&id)==A733_PCIE_READY&&id.vendor==0x1234&&id.device==0x5678);
    uint32_t value=0;assert(a733_pcie_config_read(&port,0,0,model.now+10000,&value)==A733_PCIE_READY&&value==0x56781234);
    assert(a733_pcie_config_read(&port,1,0,model.now+10000,&value)==A733_PCIE_INVALID);
    assert(a733_pcie_config_read(&port,0,4096,model.now+10000,&value)==A733_PCIE_INVALID);
    assert(a733_pcie_config_read(&port,0,0,model.now+10000,(uint32_t *)&model.clocks)==A733_PCIE_INVALID);
    assert(!a733_pcie_native_address(0x20000000,4,1)&&!a733_pcie_native_address(0x020033c4,4,1)&&
        !a733_pcie_native_address(0x07067000,4,1)&&!a733_pcie_native_address(0x06300104,4,1));
    decode();scopes();pin_conditions();
    setup();model.pmic[0x20]=0;retained();assert(!phy_done&&!port.pin.id);
    setup();*pmic_model_word(&model,0x02000210)=1u<<22;retained();assert(!phy_done&&!port.pin.id);
    setup();raise_before_adopt=1;retained();
    assert(!raise_before_adopt&&!phy_done&&!port.pin.id&&(*pmic_model_word(&model,0x02000210)&(1u<<22)));
    setup();*pmic_model_word(&model,0x02003908)&=~2u;retained();assert(!phy_done);
    setup();*pmic_model_word(&model,0x02003988)&=~(1u<<25);retained();assert(!phy_done);
    setup();stall_phy=1;retained();assert(!reset_rise);
    setup();bad_caps=1;retained();
    setup();stall_atu=1;retained();
    setup();late_fault=1;retained();assert(!late_fault);
    puts("PASS: actual provider/rail/PCK/pin/clock composition, PCIe PHY/link/config and retained failures");
}
