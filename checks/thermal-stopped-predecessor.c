/* Actual driver reinitialization over OS -> CPU rail helper -> GPU registers.
 * This models software register sequencing, not analog conversion/filter drain. */
#define main preserved_thermal_main
#include "a733-thermal.c"
#undef main

static void successor(struct model *next,const struct model *previous,unsigned cold,unsigned helper)
{
    setup(next,cold,0);
    if (helper) {
        struct a733_thermal_config config=next->driver.config;
        struct a733_thermal_ops ops=next->driver.ops;
        struct a733_clock_token token=next->driver.token;
        config.alarm_mask=config.critical_mask=0;
        memset(config.limits,0,sizeof(config.limits));
        memset(&next->driver,0,sizeof(next->driver));
        assert(a733_thermal_init(&next->driver,&config,&ops,next,&next->registry,&token));
    }
    /* Copy hardware state only. Each driver/lease ledger is freshly initialized. */
    memcpy(next->ccu,previous->ccu,sizeof(next->ccu));
    memcpy(next->ths,previous->ths,sizeof(next->ths));
    memcpy(next->sid,previous->sid,sizeof(next->sid));next->osc=previous->osc;
}
static void stopped(struct model *m)
{
    assert(a733_thermal_stop(&m->driver,100000)==A733_THERMAL_STOPPED);
    assert(!((m->ths[4/4]|m->ths[0x10/4]|m->ths[0x14/4]|m->ths[0x18/4])&31));
    assert((m->ccu[0xfc0/4]&0x8700001f)==0x80000000u);
    assert((m->ccu[0xfe4/4]&0x10001)==0x10001);
}
static void warm_started(struct model *m)
{
    assert(start(m)==A733_THERMAL_READY);
    unsigned clears=0;
    for (unsigned i=0;i<m->writes;++i) {
        assert(m->addresses[i]!=CCU+0xfc0 && m->addresses[i]!=CCU+0xfe4);
        if (m->addresses[i]>=THS+0x20 && m->addresses[i]<=THS+0x2c) {
            assert(m->values[i]==31);++clears;
        }
        if (m->addresses[i]==THS+4 && (m->values[i]&31)) assert(clears==4 && i+1==m->writes);
    }
    assert(clears==4 && m->ths[4/4]==31 && m->ths[0]==0x01df002f);
    assert((m->ths[8/4]&0xfffff000)==(28u<<12) && (m->ths[0x30/4]&7)==5);
    struct a733_thermal_sample sample;
    assert(a733_thermal_read(&m->driver,100000,&sample)==A733_THERMAL_AGAIN);
    /* A newly asserted flag is a model observation, not analog provenance. */
    m->ths[0x20/4]=31;for(unsigned i=0;i<5;++i)m->ths[(0xc0+4*i)/4]=2400+i;
    assert(a733_thermal_read(&m->driver,100000,&sample)==A733_THERMAL_SAMPLE);
    assert(sample.valid==31 && sample.sequence==1);
}
int main(void)
{
    struct model os,helper,gpu;
    setup(&os,1,0);os.osc=0x983fb0f7;os.ccu[0]=0xf8675f00;
    assert(start(&os)==A733_THERMAL_READY);stopped(&os);
    /* A predecessor conversion can finish late and leave all pending flags. */
    for(unsigned i=0x20/4;i<=0x2c/4;++i)os.ths[i]=31;
    successor(&helper,&os,0,1);warm_started(&helper);
    assert(!helper.ths[0x14/4] && !helper.ths[0x18/4]);stopped(&helper);
    for(unsigned i=0x20/4;i<=0x2c/4;++i)helper.ths[i]=31;
    successor(&gpu,&helper,0,0);warm_started(&gpu);
    assert(gpu.ths[0x14/4]==31 && gpu.ths[0x18/4]==31);
    assert(!memcmp(os.driver.calibration.correction,gpu.driver.calibration.correction,
                   sizeof(os.driver.calibration.correction)));
    for(unsigned bad=0;bad<9;++bad) {
        successor(&gpu,&helper,bad==0,0);
        if(bad==1)gpu.ccu[0xfc0/4]^=1; /* Divider. */
        if(bad==2)gpu.ccu[0xfc0/4]|=1u<<24; /* Raw26 is not normalized24. */
        if(bad==3)gpu.ccu[0xfc0/4]&=~(1u<<31);
        if(bad==4)gpu.ccu[0]&=~0x10000000u; /* REF lock lost. */
        if(bad>=5)gpu.ths[(bad==5?4:bad==6?0x10:bad==7?0x14:0x18)/4]|=1;
        assert(start(&gpu)==A733_THERMAL_RETAINED && !gpu.writes);inert(&gpu);
    }
    puts("PASS OS STOP -> helper mode0 STOP -> GPU mode0; stale W1C before enable; no clock/reset writes; 9 unsafe predecessors refused; analog drain unmodeled");
}
