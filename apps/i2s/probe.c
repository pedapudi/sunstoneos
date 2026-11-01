#include "probe.h"
#include "retire.h"
#include "sunstone_probe.h"
#include "a733_audio_leases.h"
#include "a733_i2s_platform_native.h"
#include "a733_dma_native.h"
#include "a733_platform_locks.h"
#include "dma_service_i2s_irq.h"
#include "platform.h"
#include "boot.h"
#include <string.h>
struct i2s_probe_input sunstone_probe_input __attribute__((section(".data.i2s_probe_input")))={0};
SUNSTONE_PROBE_IMAGE(I2S_PROBE_PAYLOAD,sizeof(struct i2s_probe_report),
    (uintptr_t)&sunstone_probe_input,sizeof(sunstone_probe_input),20);
#define OUT ((volatile struct i2s_probe_report *)(uintptr_t)I2S_PROBE_PAYLOAD)
#define CCU UINT32_C(0x02002000)
#define DMA UINT32_C(0x04601000)
#define I2S UINT32_C(0x02532000)
static struct dma_service service;
static struct a733_dma_native native;
static struct a733_clock_leases clocks;
static struct a733_platform_locks locks;
static struct a733_audio_leases leases;
static struct a733_audio_parent parent;
static struct a733_i2s_platform binding;
static struct a733_pins pins;
static struct dma_service_i2s_irq irq;
static struct dma_service_i2s_stream_profile profiles[2];
static struct a733_dma_segment segments[2][I2S_PROBE_PERIODS];
static struct a733_dma_mapping mappings[2][I2S_PROBE_PERIODS];
static _Alignas(64) uint8_t descriptors[2][I2S_PROBE_PERIODS*64];
static _Alignas(64) uint32_t samples[2][I2S_PROBE_PERIODS][I2S_PROBE_FRAMES];
static uint32_t copied[I2S_PROBE_FRAMES],retained[I2S_PROBE_FRAMES];
static StaticTask_t client_tcb;
static StackType_t client_stack[4096];
static TaskHandle_t client;
static uint64_t work_deadline;
static unsigned ready,closed,worker_stopped,irq_count;
static struct { uintptr_t address;uint32_t value,kind; } parent_io;
static struct { uintptr_t address;uint32_t value; } dma_io;
static _Noreturn void traced_fault(uint64_t site,uint64_t value,uintptr_t caller,uintptr_t address)
{
    register uint64_t op __asm__("x0")=0x735;
    register uint64_t code __asm__("x1")=site;
    register uint64_t operand __asm__("x2")=value;
    register uint64_t origin __asm__("x3")=caller;
    register uint64_t context __asm__("x4")=address;
    __asm__ volatile("smc #0":"+r"(op):"r"(code),"r"(operand),"r"(origin),"r"(context):"memory");
    for(;;)__asm__ volatile("wfe");
}
void a733_dma_fault_observed(const struct a733_dma_channel *ch,uintptr_t caller)
{
    unsigned channel=(unsigned)(ch-service.controller.channels);
    /* D4/5 retain both operands of a suspect package-counter transition.
     * Other checks retain the last callback's address and value as D0/1. */
    if(dma_io.address==DMA+0x130+channel*0x40)
        traced_fault(0xd4+channel,dma_io.value,caller,ch->package_last);
    traced_fault(0xd0+channel,dma_io.value,caller,dma_io.address);
}
void a733_i2s_fault_observed(const struct a733_i2s_stream *s,uintptr_t caller)
{
    traced_fault(0xe0+s->channel,parent_io.value,caller,parent_io.address);
}
static uint32_t dma_read(void *cookie,uintptr_t address)
{
    uint32_t value=a733_dma_native_operations.read(cookie,address);
    dma_io.address=address;dma_io.value=value;return value;
}
static void dma_write(void *cookie,uintptr_t address,uint32_t value)
{
    a733_dma_native_operations.write(cookie,address,value);
    dma_io.address=address;dma_io.value=value;
}
static void check(int,unsigned,uint64_t);
static void bus_empty(void)
{
    /* Manual 13.1.7.3: channel idle can precede global MBUS FIFO empty. The
     * next ring reuse and final return both wait for this completion. */
    uint64_t deadline=counter()+COUNTER_HZ/10;
    for(;;){uint32_t status=*(volatile uint32_t *)(DMA+0x30);
        check(counter()<deadline,0x6c,status);if(!(status&0x8000ffffu))return;}
}
static _Noreturn void fail(unsigned site,uint64_t detail)
{
    OUT->fault=site;OUT->detail=detail;
    /* The retained ESR/detail and caller identify the check and its input. */
    traced_fault(site,(uint32_t)detail,(uintptr_t)__builtin_return_address(0),detail>>32);
}
static void check(int ok,unsigned site,uint64_t detail){if(!ok)fail(site,detail);}
static uint32_t pattern(unsigned frame){return UINT32_C(0x5a000000)|(frame%I2S_PROBE_SAMPLES);}
static int now(void *unused,uint64_t *value)
{(void)unused;*value=counter()/24;return counter()<work_deadline;}
static int enter(void *unused,uint64_t deadline)
{(void)unused;return a733_platform_lock_enter(&locks,A733_PLATFORM_REGISTER_LOCK,deadline);}
static void leave(void *unused)
{(void)unused;a733_platform_lock_leave(&locks,A733_PLATFORM_REGISTER_LOCK);}
static int read_reg(void *unused,uintptr_t address,uint32_t *value)
{
    (void)unused;int ok;
    if(address>=0x02000000&&address<0x02000200){*value=*(volatile uint32_t *)address;ok=1;}
    else ok=a733_audio_native_read(NULL,address,value);
    parent_io.address=address;parent_io.value=ok?*value:0;parent_io.kind=ok?1:3;return ok;
}
static int write_reg(void *unused,uintptr_t address,uint32_t value)
{
    (void)unused;int ok;
    if(address>=0x02000000&&address<0x02000200){*(volatile uint32_t *)address=value;__asm__ volatile("dsb sy":::"memory");ok=1;}
    else ok=a733_audio_native_write(NULL,address,value);
    parent_io.address=address;parent_io.value=value;parent_io.kind=ok?2:4;return ok;
}
__attribute__((noinline,noreturn)) static void parent_fault(void)
{
    /* Record the first failing poll's stage and last register operand before
     * another poll can reduce it to the parent's already-failed refusal.
     * A0..A7 retain the last successful callback input; a timing or lease
     * failure may occur without MMIO. B0..B7 are read refusal, C0..C7 write refusal. */
    uint64_t site=(parent_io.kind==3?0xb0:parent_io.kind==4?0xc0:0xa0)+parent.stage;
    traced_fault(site,parent_io.value,(uintptr_t)__builtin_return_address(0),parent_io.address);
}
static _Noreturn void memory_fault(void *unused){(void)unused;fail(0x51,0);}
static struct dma_service_reply request(enum dma_service_operation op,unsigned job,uint64_t timeout,uint64_t period)
{
    struct dma_service_request r={op,job,timeout,period};struct dma_service_reply reply;uint64_t ticket;
    check(dma_service_send(&service,&r,0,&ticket),0x52,op);
    while(!dma_service_collect(&service,ticket,&reply,0)){
        check(counter()<work_deadline,0x53,((uint64_t)op<<32)|job);vTaskDelay(1);
    }
    check(reply.result==DMA_SERVICE_OK||reply.result==DMA_SERVICE_BUSY,0x54,
        ((uint64_t)op<<48)|((uint64_t)reply.result<<32)|((uint64_t)reply.phase<<16)|reply.outcome);
    return reply;
}
static void accepted(enum dma_service_operation op,unsigned job,uint64_t timeout,uint64_t ticket)
{
    struct dma_service_reply r=request(op,job,timeout,ticket);check(r.result==DMA_SERVICE_OK,0x55,op);
}
static void phase(unsigned job,enum dma_service_phase wanted)
{
    for(;;){struct dma_service_reply r=request(DMA_SERVICE_STATUS,job,0,0);
        if(r.phase==wanted)return;
        check(r.phase!=DMA_SERVICE_RECOVERY&&counter()<work_deadline,0x56,((uint64_t)wanted<<32)|r.phase);
        vTaskDelay(1);
    }
}
static void lease_release(unsigned index)
{
    struct a733_clock_token *t=&leases.tokens[index];if(!t->id)return;
    enum a733_clock_result r=a733_clock_release(&clocks,t,work_deadline/24);
    check(r==A733_CLOCK_OK,0x57,r);memset(t,0,sizeof(*t));
}
static int prepared(void *unused,uintptr_t base,uint16_t channels)
{(void)unused;return ready&&base==DMA&&channels==3;}
static int rearm(void *unused)
{
    if(unused!=&irq)return -1;
    if(irq.masked){i2s_probe_irq_enable();irq.masked=0;}return 1;
}
static void setup(void)
{
    const struct a733_audio_operations backend={.enter=enter,.leave=leave,.now_us=now,.read=read_reg,.write=write_reg};
    check(a733_clock_leases_init(&clocks,now,NULL)&&a733_platform_locks_init(&locks,now,NULL),0x58,0);
    check(a733_audio_leases_init(&leases,&clocks,7,&backend,NULL),0x59,0);
    unsigned cold=!(i2s_probe_saved.audio&((1u<<31)|(1u<<29)|(1u<<27)));
    check(a733_audio_parent_init(&parent,&leases.operations,&leases,cold,work_deadline/24),0x5a,cold);
    const struct a733_pins_operations pops={enter,leave,now,read_reg,write_reg};
    check(a733_pins_init(&pins,&pops,NULL,(1u<<5)|(1u<<6)|(1u<<7)|(1u<<8)),0x5b,0);
    struct a733_pin_request pin[4];const unsigned pb[]={5,6,8,7};
    for(unsigned i=0;i<4;++i){unsigned n=pb[i];struct a733_pin_fields f;
        check(a733_pin_fields(n,&f),0x5b,n);
        uint32_t mux=*(volatile uint32_t *)f.mux_address;
        uint32_t drive=*(volatile uint32_t *)f.drive_address;
        uint32_t pull=*(volatile uint32_t *)f.pull_address;
        pin[i]=(struct a733_pin_request){.pb_pin=n,.expected={
            .function=(mux>>f.mux_shift)&15,.pull=(pull>>f.pull_shift)&3,.drive=(drive>>f.drive_shift)&15},
            .desired={3,0,0}};
    }
    check(a733_i2s_platform_duplex_init(&binding,&parent,&pins,&leases,7,pin,4),0x5c,0);
    for(unsigned j=0;j<2;++j)profiles[j]=(struct dma_service_i2s_stream_profile){
        .stream={{24576000,16000,0,32,32,1,0},a733_i2s_platform_stream_operations(),&binding,
            j?A733_I2S_TRANSMIT:A733_I2S_RECEIVE,10000,1,A733_I2S_ROUTE_I2S0},.duplex=1};
    const struct dma_memory_window windows[]={
        {(uintptr_t)descriptors,(uintptr_t)descriptors,sizeof(descriptors),DMA_MEMORY_ROLE(A733_DMA_DESCRIPTORS)},
        {(uintptr_t)samples[0],(uintptr_t)samples[0],sizeof(samples[0]),DMA_MEMORY_ROLE(A733_DMA_WRITE)},
        {(uintptr_t)samples[1],(uintptr_t)samples[1],sizeof(samples[1]),DMA_MEMORY_ROLE(A733_DMA_READ)}};
    const struct dma_fifo_port ports[]={{A733_I2S0_RXFIFO,3,1u<<4,1u<<8,A733_DMA_READ},
        {A733_I2S0_TXFIFO,3,1u<<4,1u<<8,A733_DMA_WRITE}};
    check(a733_dma_native_init(&native,DMA,DMA,windows,3,ports,2,memory_fault,NULL),0x5d,0);
    struct dma_service_job jobs[2];
    for(unsigned j=0;j<2;++j){
        for(unsigned i=0;i<I2S_PROBE_PERIODS;++i){uintptr_t at=(uintptr_t)samples[j][i];
            mappings[j][i]=(struct a733_dma_mapping){(void *)at,at,I2S_PROBE_BYTES,j?A733_DMA_READ:A733_DMA_WRITE};
            struct a733_dma_endpoint ram={at,0,4,8},fifo={j?A733_I2S0_TXFIFO:A733_I2S0_RXFIFO,3,4,8};
            segments[j][i]=(struct a733_dma_segment){j?ram:fifo,j?fifo:ram,I2S_PROBE_BYTES};}
        jobs[j]=(struct dma_service_job){.kind=DMA_SERVICE_I2S_STREAM,.channel=j,.stream=&profiles[j],
            .transfer={{descriptors[j],(uintptr_t)descriptors[j],sizeof(descriptors[j]),A733_DMA_DESCRIPTORS},
                segments[j],I2S_PROBE_PERIODS,mappings[j],I2S_PROBE_PERIODS}};
    }
    const struct dma_service_protected protected[]={
        {(uintptr_t)&service,(uintptr_t)&service,sizeof(service)},
        {(uintptr_t)&native,(uintptr_t)&native,sizeof(native)},
        {(uintptr_t)&binding,(uintptr_t)&binding,sizeof(binding)},
        {(uintptr_t)&parent,(uintptr_t)&parent,sizeof(parent)},
        {(uintptr_t)&clocks,(uintptr_t)&clocks,sizeof(clocks)},
        {(uintptr_t)&locks,(uintptr_t)&locks,sizeof(locks)},
        {(uintptr_t)&pins,(uintptr_t)&pins,sizeof(pins)}};
    const struct dma_service_activation activation={client,prepared,NULL};
    struct a733_dma_operations dma_ops=a733_dma_native_operations;dma_ops.read=dma_read;dma_ops.write=dma_write;
    check(dma_service_init_deferred(&service,&dma_ops,&native,DMA,3,COUNTER_HZ/10,
        jobs,2,protected,7,5,1,&activation),0x5e,0);
    irq=(struct dma_service_i2s_irq){&service,{0x03400000,143},1};
    const struct dma_service_irq interrupt={rearm,&irq,1,COUNTER_HZ/configTICK_RATE_HZ};
    check(dma_service_bind(&service,0,client,3)&&dma_service_bind_irq(&service,&interrupt),0x5f,0);
}
extern void i2s_probe_platform_irq(uint64_t);
void vApplicationFPUSafeIRQHandler(uint64_t interrupt)
{
    if(interrupt==143){++irq_count;dma_service_i2s_irq_handler(&irq);}
    else i2s_probe_platform_irq(interrupt);
}
static struct a733_i2s_period get(unsigned job)
{
    for(;;){struct dma_service_reply r=request(DMA_SERVICE_PERIOD_GET,job,0,0);
        if(r.result==DMA_SERVICE_OK)return r.period;
        check(counter()<work_deadline,0x60,job);vTaskDelay(1);
    }
}
static void begin_round(void)
{
    closed=0;
    for(unsigned j=0;j<2;++j){accepted(DMA_SERVICE_OPEN,j,100000,0);phase(j,DMA_SERVICE_READY);}
    /* Manual 17.1.8.1: LOOP connects DOUT to DIN. Configure overwrites CTL,
     * so set it after both opens and before either direction starts. */
    struct a733_i2s_stream_operations o=a733_i2s_platform_stream_operations();uint32_t ctl;
    check(o.health(&binding,counter()/24+10000)==A733_I2S_RESOURCE_READY&&
        o.io.read(&binding,I2S,&ctl)&&o.io.write(&binding,I2S,ctl|8),0x61,0);
    for(unsigned n=0;n<I2S_PROBE_PERIODS;++n){
        struct a733_i2s_period p=get(1);check(p.kind==A733_I2S_TX_FILL&&p.slot==n,0x62,p.kind);
        for(unsigned i=0;i<I2S_PROBE_FRAMES;++i)samples[1][n][i]=pattern(n*I2S_PROBE_FRAMES+i);
        accepted(DMA_SERVICE_PERIOD_PUT,1,0,p.ticket);
    }
    accepted(DMA_SERVICE_SUBMIT,0,COUNTER_HZ,0);accepted(DMA_SERVICE_SUBMIT,1,COUNTER_HZ,0);
}
static void retire(uint64_t held)
{
    OUT->stage=6;
    struct i2s_retirement retirement={.service=&service,.held={held,0},.jobs=2};
    int r;
    while(!(r=i2s_retirement_step(&retirement))){check(counter()<work_deadline,0x63,retirement.stage);vTaskDelay(1);}
    check(r==1,0x64,((uint32_t)retirement.job<<24)|(retirement.stage<<16)|
        ((uint32_t)retirement.reply.phase<<8)|retirement.reply.result);
    for(unsigned j=0;j<2;++j)check(service.channels[j].phase==DMA_SERVICE_CLOSED&&
        !service.channels[j].holds_buffers&&!service.controller.channels[j].claimed,0x65,j);
    bus_empty();closed=1;++OUT->closed;sunstone_probe_progress();
}
static int run_round(unsigned overrun)
{
    begin_round();OUT->stage=5;uint64_t first=0,last=0;unsigned matched=0;uint64_t previous=0;
    unsigned count=sunstone_probe_input.periods;
    for(unsigned n=0;n<count;++n){struct a733_i2s_period p=get(0);
        check(p.kind==A733_I2S_CAPTURED_COPY&&p.frames==I2S_PROBE_FRAMES&&p.first_frame==(uint64_t)n*I2S_PROBE_FRAMES,
            0x66,((uint64_t)p.kind<<32)|p.frames);
        check(dma_service_copy_period(&service,0,p.ticket,copied,sizeof(copied)),0x67,p.ticket);
        if(overrun){
            memcpy(retained,copied,sizeof(copied));vTaskDelay(pdMS_TO_TICKS(160));
            struct dma_service_reply r=request(DMA_SERVICE_STATUS,0,0,0);
            check(r.phase==DMA_SERVICE_RESULT&&r.outcome==DMA_SERVICE_FAILED,0x68,((uint64_t)r.phase<<32)|r.outcome);
            check(dma_service_copy_period(&service,0,p.ticket,copied,sizeof(copied))&&
                !memcmp(retained,copied,sizeof(copied)),0x69,p.ticket);
            retire(p.ticket);return 1;
        }
        for(unsigned i=0;i<I2S_PROBE_FRAMES;++i){uint32_t value=copied[i];
            if(!matched){if((value&0xfffff800u)!=0x5a000000u)continue;first=value;matched=1;}
            else if(value!=pattern((unsigned)last+1)){
                fail(0x6a,((uint64_t)pattern((unsigned)last+1)<<32)|value);
            }
            last=value;OUT->first=first;OUT->last=last;++OUT->frames;
        }
        check(p.ticket>previous,0x6b,p.ticket);previous=p.ticket;++OUT->copies;
        /* Retire with the final exact staging ticket still held. */
        if(n+1==count){OUT->rx_packages=service.streams[0].frames/I2S_PROBE_FRAMES;
            OUT->tx_packages=service.streams[1].frames/I2S_PROBE_FRAMES;retire(p.ticket);break;}
        accepted(DMA_SERVICE_PERIOD_PUT,0,0,p.ticket);sunstone_probe_progress();
    }
    return matched;
}
uint64_t sunstone_probe_quiesce(void)
{
    if(!sunstone_probe_input.mode)return !service.initialized?SUNSTONE_PROBE_READY:0;
    if(!closed||!worker_stopped||binding.pin_lease.id||binding.local.id)return 0;
    for(unsigned j=0;j<2;++j)if(service.channels[j].phase!=DMA_SERVICE_CLOSED||
        service.channels[j].holds_buffers||service.controller.channels[j].claimed)return 0;
    /* Channel idle may precede the shared MBUS FIFO empty indication. */
    bus_empty();
    return SUNSTONE_PROBE_READY;
}
static void task(void *unused)
{
    (void)unused;OUT->mode=sunstone_probe_input.mode;
    if(!sunstone_probe_input.mode){vTaskDelay(2);memcpy((void *)OUT->result,"PASS I2S control",17);platform_exit(0);}
    unsigned stage=0;
    while(!a733_audio_parent_ready(&parent)){
        enum a733_i2s_resource_result p=a733_audio_parent_poll(&parent);
        if(p!=A733_I2S_RESOURCE_PENDING&&p!=A733_I2S_RESOURCE_BUSY&&p!=A733_I2S_RESOURCE_READY)parent_fault();
        if(parent.stage!=stage){stage=parent.stage;sunstone_probe_progress();}vTaskDelay(1);
    }
    ready=1;check(dma_service_activate(&service)==DMA_SERVICE_OK,0x6e,0);
    int passed=1;
    for(unsigned i=0;i<sunstone_probe_input.rounds&&passed;++i)passed=run_round(sunstone_probe_input.mode==2);
    /* All copied requests have returned and every channel is CLOSED. On one
     * core suspension retires the only worker before resources are restored. */
    taskENTER_CRITICAL();
    i2s_probe_irq_disable();vTaskSuspend(service.worker);worker_stopped=1;
    taskEXIT_CRITICAL();OUT->interrupts=irq_count;
    check(irq_count>0,0x6f,0);
    lease_release(A733_AUDIO_STABLE-1);lease_release(A733_AUDIO_REFERENCE-1);
    for(unsigned i=0;i<4;++i)check(!leases.tokens[i].id,0x70,i);
    OUT->stage=7;
    const char *verdict=passed?(sunstone_probe_input.mode==2?"PASS I2S overrun":"PASS I2S loopback"):"FAIL I2S continuity";
    memcpy((void *)OUT->result,verdict,strlen(verdict)+1);
    platform_exit(!passed);
}
void kernel_main(void)
{
    platform_init();memset((void *)OUT,0,sizeof(*OUT));work_deadline=counter()+15*COUNTER_HZ;
    /* Bound diagnostic loops; the runner supplies the contract and extents. */
    check(sunstone_probe_input.mode<=2&&sunstone_probe_input.periods>=3&&sunstone_probe_input.periods<=64&&
        sunstone_probe_input.rounds>=1&&sunstone_probe_input.rounds<=3&&!sunstone_probe_input.reserved,0x71,sunstone_probe_input.mode);
    client=xTaskCreateStatic(task,"i2s-probe",4096,NULL,4,client_stack,&client_tcb);check(client!=NULL,0x72,0);
    if(sunstone_probe_input.mode)setup();
    vTaskStartScheduler();fail(0x73,0);
}
void vApplicationStackOverflowHook(TaskHandle_t handle,char *name)
{(void)handle;fail(0x74,(uintptr_t)name);}
