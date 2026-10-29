/* Real stream and DMA controller, modeled registers and drain. This does not
 * establish physical DMA visibility, interrupt latency or FIFO continuity. */
#include "dma_service_i2s_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define I2S3 UINT32_C(0x02535000)
#define DMA UINT32_C(0x04601000)
static struct a733_i2s_stream s;
static struct a733_dma_controller dma;
static struct a733_i2s_stream_profile p;
static struct a733_dma_transfer t;
static struct a733_dma_mapping maps[4];
static struct a733_dma_segment segments[4];
static _Alignas(4096) unsigned char descriptors[4096], pcm[4][4096];
static uint32_t ir[32], dr[128];
static uint64_t time_us;
static unsigned acquired, writes, reads, starts, drain_reads, drain_after, clock_fail, clock_back, clock_step;
static uintptr_t base;
static unsigned busy_after_drain,health_busy,dma_recovery_expiry,injected,injected_writes;
static uint32_t read_dma(void *v,uintptr_t a)
{
    assert(v==&dma && a>=DMA && a<DMA+sizeof(dr));++reads;
    if(dma_recovery_expiry && s.bridge_polls && a==DMA+0x34){
        time_us=s.bridge_until;injected=1;injected_writes=writes;return 0;
    }
    if(a==DMA+0x30 && !dr[0x100/4] && dma.channels[0].status==A733_DMA_STOPPING){
        ++drain_reads;
        if(drain_after && drain_reads>=drain_after){dr[0x30/4]=0;if(busy_after_drain)health_busy=1;}
    }
    return dr[(a-DMA)/4];
}
static void write_dma(void *v,uintptr_t a,uint32_t x)
{
    assert(v==&dma && a>=DMA+0x100 && a<DMA+0x140);++writes;
    unsigned off=a-DMA;
    if(off==0x100){assert(!(ir[28/4]&128));if(x){++starts;dr[0x30/4]=1;}}
    if(off==0x138)dr[off/4]&=~x;else dr[off/4]=x;
}
static int mapping(void *v,const struct a733_dma_mapping *m)
{
    assert(v==&dma);
    if(m->access==A733_DMA_DESCRIPTORS){
        if(m->cpu==descriptors && m->bus==0x42000000 && m->bytes==4096)return 1;
        for(unsigned n=0;n<4;++n)if(m->cpu==descriptors+n*64 && m->bus==0x42000000+n*64 && m->bytes==64)return 1;
    }
    for(unsigned n=0;n<4;++n)if(m->cpu==pcm[n] && m->bus==0x42001000+n*4096 && m->bytes==4096 && m->access==A733_DMA_READ)return 1;
    return 0;
}
static void cache(void *v,enum a733_dma_cache op,void *cpu,size_t bytes)
{
    struct a733_dma_mapping m={cpu,0,bytes,A733_DMA_DESCRIPTORS};
    assert(op==A733_DMA_CLEAN);
    for(unsigned n=0;n<4;++n){
        if(cpu==descriptors+n*64)m.bus=0x42000000+n*64;
        if(cpu==pcm[n]){m.bus=0x42001000+n*4096;m.access=A733_DMA_READ;}
    }
    assert(mapping(v,&m));
}
static void barrier(void *v){assert(v==&dma);}
static uint64_t ticks(void *v){assert(v==&dma);return time_us;}
static int fifo(void *v,const struct a733_dma_endpoint *e,enum a733_dma_access role)
{return v==&dma && role==A733_DMA_WRITE && e->address==base+32 && e->request==(base==I2S3?6u:3u) && e->width==2 && e->burst==8;}
static enum a733_i2s_resource_result acquire(void *v,const struct a733_i2s_plan *plan,uint64_t deadline)
{
    assert(v==&s && !acquired && deadline>time_us);
    assert(plan->pb_mask==(base==I2S3?0u:((1u<<5)|(1u<<6)|(1u<<7))));
    acquired=1;return A733_I2S_RESOURCE_READY;
}
static enum a733_i2s_resource_result resource(void *v,uint64_t deadline)
{assert(v==&s && acquired && deadline>time_us);return health_busy?A733_I2S_RESOURCE_BUSY:A733_I2S_RESOURCE_READY;}
static int now(void *v,uint64_t *out)
{assert(v==&s);if(clock_fail)return 0;if(clock_back){*out=time_us-1;return 1;}time_us+=clock_step;*out=time_us;return 1;}
static int read_i2s(void *v,uintptr_t a,uint32_t *out)
{assert(v==&s && acquired && a>=base && a<base+sizeof(ir));++reads;*out=ir[(a-base)/4];return 1;}
static int write_i2s(void *v,uintptr_t a,uint32_t x)
{
    assert(v==&s && acquired && a>=base && a<base+sizeof(ir));++writes;
    unsigned off=a-base;assert(off!=16 && off!=32);
    if(off==28 && (x&128))assert(dr[0x100/4] && (ir[0]&1));
    if(off==12)ir[3]&=~x;
    else ir[off/4]=off==20?x&~(3u<<24):x;
    return 1;
}
static void setup(unsigned hdmi)
{
    memset(&s,0,sizeof(s));memset(&dma,0,sizeof(dma));memset(ir,0,sizeof(ir));memset(dr,0,sizeof(dr));
    acquired=writes=reads=starts=drain_reads=clock_fail=clock_back=clock_step=0;drain_after=1;time_us=1;
    busy_after_drain=health_busy=dma_recovery_expiry=injected=injected_writes=0;
    base=hdmi?I2S3:A733_I2S0_BASE;dr[0x34/4]=1;dr[0x108/4]=0x41000000;ir[24/4]=64u<<16;
    p=(struct a733_i2s_stream_profile){.format={24576000,48000,0,16,32,2,0},
        .operations={{acquire,resource,resource,now,read_i2s,write_i2s},resource},
        .cookie=&s,.direction=A733_I2S_TRANSMIT,.operation_us=10000,.route=hdmi?A733_I2S_ROUTE_HDMI3:A733_I2S_ROUTE_I2S0};
    for(unsigned n=0;n<4;++n){
        maps[n]=(struct a733_dma_mapping){pcm[n],0x42001000+n*4096,4096,A733_DMA_READ};
        segments[n]=(struct a733_dma_segment){{maps[n].bus,0,2,8},{base+32,hdmi?6u:3u,2,8},4096};
    }
    t=(struct a733_dma_transfer){{descriptors,0x42000000,4096,A733_DMA_DESCRIPTORS},segments,4,maps,4};
    const struct a733_dma_operations ops={read_dma,write_dma,cache,barrier,ticks,mapping,fifo};
    assert(a733_dma_controller_init(&dma,&ops,&dma,DMA,1,10000)==A733_DMA_CONTROL_OK);
}
static void start(void)
{
    assert(a733_i2s_stream_open(&s,&p,&t,&dma,0,10000)==A733_I2S_OPENING);
    assert(a733_i2s_stream_poll(&s)==A733_I2S_READY);
    assert(ir[4/4]==0x1f37 && ir[36/4]==0x50 && ir[48/4]==1);
    assert(ir[52/4]==0x110003 && ir[68/4]==0 && ir[72/4]==0x10);
    assert(ir[20/4]==(0x40075u|(base==I2S3?0x80000000u:0)));
    for(unsigned n=0;n<4;++n){struct a733_i2s_period lease;
        assert(a733_i2s_stream_get(&s,&lease)==1 && lease.slot==n && lease.kind==A733_I2S_TX_FILL);
        memset(pcm[n],n,4096);assert(a733_i2s_stream_put(&s,lease.ticket)==1);
    }
    assert(a733_i2s_stream_start(&s,100000)==A733_I2S_PREPARING);
    assert(a733_i2s_stream_poll(&s)==A733_I2S_ACTIVE);
    assert(a733_i2s_stream_poll(&s)==A733_I2S_ACTIVE && (ir[0]&7)==5);
}
static void complete(void)
{dr[0x138/4]=4;drain_reads=0;ir[40/4]+=s.active_segment.bytes/2;}
static void retained(void)
{
    assert(s.status==A733_I2S_RECOVERY && s.platform_held && s.dma_held && acquired);
    unsigned before=writes+reads;
    assert(a733_i2s_stream_poll(&s)==A733_I2S_RECOVERY);
    assert(a733_i2s_stream_cancel(&s)==A733_I2S_RECOVERY && writes+reads==before);
}
static void controlled_failure(void)
{
    assert(s.status==A733_I2S_STOPPING && s.platform_held && s.dma_held && acquired);
    assert(!ir[28/4] && !(ir[0]&6) && a733_dma_owns(&dma,&s.active));
    assert(!s.frames && !s.bridge_deadline && !s.bridge_until);
    drain_after=1;clock_step=0;
    assert(a733_i2s_stream_poll(&s)==A733_I2S_FAILED);
    assert(!a733_dma_owns(&dma,&s.active) && s.platform_held && s.dma_held && acquired);
    assert(!(ir[0]&7));
}
int main(int argc,char **argv)
{
    const char *selected=argc>1?argv[1]:"all";
#define CASE(name) if(!strcmp(selected,"all") || !strcmp(selected,name))
    CASE("wraps") {
        setup(1);start();
        /* Seventeen completions cross four full ring wraps. END uses a non-burst-
         * aligned 37-frame final prefix; full allocation cache ownership remains. */
        for(unsigned n=0;n<17;++n){
            complete();assert(a733_i2s_stream_poll(&s)==A733_I2S_ACTIVE);
            struct a733_i2s_period lease;assert(a733_i2s_stream_get(&s,&lease)==1);
            assert(lease.slot==n%4 && lease.first_frame==n*1024 && lease.frames==1024);
            assert(lease.kind==A733_I2S_TX_MEMORY_CONSUMED);
            if(n==16)assert(a733_i2s_stream_end(&s,lease.ticket,37)==1);
            else assert(a733_i2s_stream_put(&s,lease.ticket)==1);
    }
    for(unsigned n=0;n<4;++n){complete();assert(a733_i2s_stream_poll(&s)==A733_I2S_ACTIVE);}
    assert(s.frames==20*1024+37 && s.active_segment.bytes==148 && !a733_dma_owns(&dma,&s.active));
    ir[24/4]=128u<<16;assert(a733_i2s_stream_poll(&s)==A733_I2S_INPUT_ENDED);
    assert(s.platform_held && s.dma_held);
    puts("PASS four-period ring wraps and 37-frame END/FIFO observation");
    }
    CASE("delayed") {
        setup(1);start();drain_after=8;complete();
        assert(a733_i2s_stream_poll(&s)==A733_I2S_ACTIVE && s.bridge_polls==8 && starts==2);
        puts("PASS delayed drain completes in eight actual polls");
    }
    CASE("frozen") {
        setup(1);start();drain_after=0;complete();
        assert(a733_i2s_stream_poll(&s)==A733_I2S_STOPPING && s.bridge_polls==64 && drain_reads==64);
        controlled_failure();
        puts("PASS frozen clock reaches 64 actual polls, disables TX and retains allocation through drain");
    }
    CASE("time") {
        setup(1);start();drain_after=0;clock_step=2;complete();
        assert(a733_i2s_stream_poll(&s)==A733_I2S_STOPPING && s.bridge_polls<64 && time_us>=201);
        controlled_failure();puts("PASS 200-us handoff expiry disables TX before bounded drain to FAILED");
    }
    CASE("clock") {
        setup(1);start();clock_back=1;complete();assert(a733_i2s_stream_poll(&s)==A733_I2S_RECOVERY);retained();
        setup(1);start();clock_fail=1;complete();assert(a733_i2s_stream_poll(&s)==A733_I2S_RECOVERY);retained();
        puts("PASS backwards and failed clocks retain without later MMIO");
    }
    CASE("duplex") {
        setup(1);
        assert(a733_i2s_stream_duplex_open(&s,&p,&t,&dma,0,10000,NULL)==A733_I2S_INVALID);
        struct dma_service_i2s_stream_profile a={p,1},b={p,1};b.stream.direction=A733_I2S_RECEIVE;
        assert(!dma_service_i2s_stream_pair(&a,&b));
        assert(dma_service_i2s_stream_duplex_open(&s,&a,&t,&dma,0,10000,NULL)==A733_I2S_INVALID);
        assert(!acquired && !reads && !writes);
        puts("PASS direct and service I2S3 duplex refusal before resource acquisition");
    }
    CASE("i2s0") {
    setup(0);start();complete();assert(a733_i2s_stream_poll(&s)==A733_I2S_ACTIVE && starts==2);
    puts("PASS I2S0 finite-TX route compatibility");
    }
    CASE("fifo-deadline") {
        setup(1);start();drain_after=0;clock_step=1;ir[24/4]=125u<<16;complete();
        assert(a733_i2s_stream_poll(&s)==A733_I2S_STOPPING);
        assert(s.bridge_polls<64 && time_us<201);
        controlled_failure();puts("PASS original three-word FIFO occupancy deadline enters controlled stop");
    }
    CASE("busy") {
        setup(1);start();busy_after_drain=1;complete();
        assert(a733_i2s_stream_poll(&s)==A733_I2S_ACTIVE && s.bridge_polls==1);
        uint64_t until=s.bridge_until,original=s.bridge_deadline,operation=s.deadline;
        unsigned before=writes+reads;
        for(unsigned n=0;n<65 && s.status!=A733_I2S_RECOVERY;++n){
            (void)a733_i2s_stream_poll(&s);
            assert(s.bridge_until==until && s.bridge_deadline==original && s.deadline==operation);
            assert(writes+reads==before);
        }
        retained();puts("PASS health BUSY preserves one handoff budget across calls with frozen clock");
    }
    CASE("operation-expiry") {
        setup(1);start();busy_after_drain=1;complete();
        assert(a733_i2s_stream_poll(&s)==A733_I2S_ACTIVE && s.bridge_polls==1);
        unsigned before=writes+reads;
        time_us=s.deadline;health_busy=0;
        assert(a733_i2s_stream_poll(&s)==A733_I2S_RECOVERY && writes+reads==before);
        retained();puts("PASS expired operation authority prevents controlled-stop MMIO");
    }
    CASE("dma-recovery-expiry") {
        setup(1);start();dma_recovery_expiry=1;complete();
        assert(a733_i2s_stream_poll(&s)==A733_I2S_RECOVERY && injected);
        assert(dma.channels[0].status==A733_DMA_RECOVERY && writes==injected_writes);
        assert(time_us==s.bridge_until && a733_dma_owns(&dma,&s.active));
        retained();puts("PASS simultaneous DMA recovery and bridge expiry prohibits further writes");
    }
    CASE("starvation") {
        setup(1);start();
        for(unsigned n=0;n<3;++n){complete();assert(a733_i2s_stream_poll(&s)==A733_I2S_ACTIVE);}
        complete();assert(a733_i2s_stream_poll(&s)==A733_I2S_STOPPING);
        assert(a733_i2s_stream_poll(&s)==A733_I2S_FAILED && starts==4);
        assert(!ir[28/4] && !(ir[0]&7) && s.platform_held && s.dma_held);
        puts("PASS missing refill stops without replaying a consumed period");
    }
    CASE("underrun") {
        setup(1);start();ir[12/4]=0x40;
        assert(a733_i2s_stream_poll(&s)==A733_I2S_STOPPING);
        assert(a733_i2s_stream_poll(&s)==A733_I2S_FAILED && starts==1);
        assert(s.platform_held && s.dma_held);puts("PASS latched TX underrun stops and retains resources");
    }
    return 0;
}
