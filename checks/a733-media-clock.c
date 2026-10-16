#include "a733_media_clock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CCU 0x02002000u
#define BIT(n) (1u<<(n))
struct model {
    uint32_t ccu[0x2000/4],domain[10][3],osc,pck;
    uint64_t time;
    unsigned held,retained,reads,writes,authorize,setup,reenter,fail_write,retain_busy,expire_write;
    unsigned lock_busy,auth_pending;
    uintptr_t address[64];uint32_t values[64];
    struct a733_media_clock driver;
    struct a733_clock_leases registry;
    struct a733_gpu_resource_manager manager;
};
static int now(void *v,uint64_t *out) {struct model *m=v;*out=++m->time;return 1;}
static uint32_t *reg(struct model *m,uintptr_t a)
{
    if (a>=CCU && a<CCU+0x2000 && !(a&3)) return &m->ccu[(a-CCU)/4];
    if (a==0x07090160) return &m->osc;
    if (a==0x070101ac) return &m->pck;
    if (a>=0x07060000 && a<=0x07069008) {
        unsigned d=(unsigned)(a-0x07060000)/0x1000,o=(unsigned)(a-0x07060000)%0x1000;
        if ((d<=3 || d==8 || d==9) && (o==0 || o==8)) return &m->domain[d][o/4];
    }
    assert(!"unexpected modeled register");return NULL;
}
static int read32(void *v,uintptr_t a,uint32_t *out)
{
    struct model *m=v;assert(m->setup || (m->held && !m->retained));++m->reads;
    if (m->reenter) {m->reenter=0;assert(a733_media_clock_poll(&m->driver)==A733_MEDIA_BUSY);}
    *out=*reg(m,a);return 1;
}
static int write32(void *v,uintptr_t a,uint32_t val)
{
    struct model *m=v;assert(m->held && !m->retained && m->authorize);assert(m->writes<64);
    uint32_t *r=reg(m,a),mask=0;
    switch (a-CCU) {
    case 0x1884:case 0xa74:case 0x16e4:mask=0x10000;break;
    case 0x1844:case 0xa04:case 0x16c4:case 0x150c:case 0x158c:mask=0x10001;break;
    case 0x58c:case 0x5b4:mask=0x10007;break;
    case 0x5e4:mask=0x300;break;
    case 0x1840:case 0x1860:case 0xa00:case 0x1588:mask=0x8700001f;break;
    case 0x1804:case 0x1808:mask=0x87001f1f;assert(!(val&BIT(31)));break;
    default:assert(!"write beyond owned clocks");
    }
    assert(!((*r^val)&~mask));
    m->address[m->writes]=a;m->values[m->writes]=val;++m->writes;*r=val;
    if (m->expire_write) m->time=m->driver.config.deadline_us;
    if (m->writes==m->fail_write) {
        if (m->retain_busy) atomic_store(&m->registry.busy,1);
        return 0;
    }
    return 1;
}
static int enter(void *v,uint64_t deadline)
{
    struct model *m=v;assert(deadline>m->time && !m->held);
    if (m->lock_busy) {m->lock_busy=0;return 0;}m->held=1;return 1;
}
static void leave(void *v)
{
    struct model *m=v;assert(m->held && !m->retained);m->held=0;
    /* An unrelated authorized writer changes neighboring fields between polls. */
    m->ccu[0x5e4/4]^=BIT(20);m->ccu[0x158c/4]^=BIT(5);
}
static void retain(void *v) {struct model *m=v;assert(m->held && !m->retained);m->retained=1;}
static int auth(void *v,const struct a733_media_clock_config *c,const struct a733_clock_token *t,
    const struct a733_gpu_resource_lease *p)
{
    struct model *m=v;assert(c->handoff==99 && t->id && t->owner==c->owner && p->token);
    assert(a733_clock_token_status(&m->registry,t)==A733_CLOCK_OK);
    if (m->auth_pending) {m->auth_pending=0;return 0;}++m->authorize;return 1;
}
static int gpu_update(void *v,uintptr_t a,uint32_t required,uint32_t set,uint64_t deadline)
{(void)v;(void)a;(void)required;(void)set;(void)deadline;assert(!"PCK gate already enabled");return 0;}
static int gpu_rail(void *v,struct a733_gpu_resource_rail *r,uint64_t deadline)
{(void)v;(void)r;(void)deadline;assert(!"PCK-only acquire does not read rails");return 0;}
static void setup(struct model *m,unsigned groups)
{
    memset(m,0,sizeof(*m));m->setup=1;m->pck=1;
    m->ccu[0]=0xf8000000u|(99u<<8)|(99u<<16);
    m->ccu[0xa0/4]=0xfe000000u|(99u<<8)|(1u<<20)|(2u<<16)|(4u<<2);
    m->ccu[0x120/4]=0xf8000000u|(53u<<8)|(3u<<20);
    m->ccu[0x2e0/4]=0xf4000000u|(99u<<8)|(3u<<16);
    m->ccu[0x1908/4]=0x0fff0fff;m->ccu[0x1988/4]=0x0fff0000;
    m->ccu[0x1910/4]=0x00770077;m->ccu[0x1990/4]=0x00770000;
    m->ccu[0x1928/4]=0x00030003;m->ccu[0x19a8/4]=0x00030000;
    m->ccu[0x500/4]=0x03000003;m->ccu[0x580/4]=0x83000001;m->ccu[0x588/4]=0x81000001;
    m->ccu[0x5c0/4]=0x010000ff;m->ccu[0x5e0/4]=0x41055803;m->ccu[0x5e4/4]=0x40302;
    m->ccu[0x1804/4]=0x01000201;m->ccu[0x1808/4]=0x02000401;
    for (unsigned i=0;i<10;++i) m->domain[i][0]=m->domain[i][2]=8;
    assert(a733_clock_leases_init(&m->registry,now,m));
    const struct a733_gpu_resource_ops go={now,read32,gpu_update,gpu_rail};
    assert(a733_gpu_resources_init(&m->manager,&go,m));
    uint64_t token;struct a733_gpu_power_resources ready;
    assert(a733_gpu_resources_acquire(&m->manager,2,A733_GPU_RESOURCE_PCK,1000000,&token,&ready)==A733_GPU_RESOURCE_OK);
    struct a733_gpu_resource_lease lease=m->manager.leases[2];
    struct a733_media_clock_config c={.owner=5,.handoff=99,.deadline_us=1000000,.groups=groups,
        .camera_mclk_mask=(groups&A733_MEDIA_VI)?6:0,
        .fabric={0x03000003,0x83000001,0x81000001},.rate_hz={150000000,300000000,300000000}};
    const struct a733_media_clock_ops ops={now,auth,enter,leave,retain,read32,write32};
    assert(a733_media_clock_init(&m->driver,&c,&ops,m,&m->registry,&m->manager,2,&lease));
    m->setup=0;m->reads=0;
}
static enum a733_media_clock_result run(struct model *m)
{
    enum a733_media_clock_result r=A733_MEDIA_PENDING;
    for (unsigned i=0;i<70 && (r==A733_MEDIA_PENDING || r==A733_MEDIA_BUSY);++i) r=a733_media_clock_poll(&m->driver);
    return r;
}
static void terminal(struct model *m,enum a733_media_clock_result result)
{
    unsigned reads=m->reads,writes=m->writes;uint64_t time=m->time;
    assert(a733_media_clock_poll(&m->driver)==result);
    assert(reads==m->reads && writes==m->writes && time==m->time);
}
static void registry_cases(void)
{
    struct model m;setup(&m,A733_MEDIA_ALL);
    const uint32_t parents[]={A733_CLOCK_PLL_VIDEO0,A733_CLOCK_PLL_DE,A733_CLOCK_PLL_VE0,A733_CLOCK_PLL_VE1};
    struct a733_clock_token t,u;
    for (unsigned i=0;i<4;++i) {
        assert(a733_clock_acquire(&m.registry,10,parents[i],0,999999,&t)==A733_CLOCK_OK);
        assert(t.shared==(parents[i]|A733_CLOCK_REF|A733_CLOCK_OSC));
        assert(a733_clock_acquire(&m.registry,11,0,A733_CLOCK_REF,999999,&u)==A733_CLOCK_BUSY);
        assert(a733_clock_release(&m.registry,&t,999999)==A733_CLOCK_OK);
    }
    assert(a733_clock_acquire(&m.registry,10,A733_CLOCK_MEDIA_LOCALS,0,999999,&t)==A733_CLOCK_INVALID);
    assert(a733_clock_acquire(&m.registry,10,0,A733_CLOCK_VO0,999999,&t)==A733_CLOCK_OK);
    assert(t.shared==(A733_CLOCK_DISPLL1|A733_CLOCK_OSC));
    assert(a733_clock_acquire(&m.registry,11,0,A733_CLOCK_DISPLL1,999999,&u)==A733_CLOCK_BUSY);
    assert(a733_clock_release(&m.registry,&t,999999)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&m.registry,10,0,A733_CLOCK_DISPLL1,999999,&t)==A733_CLOCK_OK);
    assert(t.shared==A733_CLOCK_OSC);
    assert(a733_clock_acquire(&m.registry,11,0,A733_CLOCK_VO0,999999,&u)==A733_CLOCK_BUSY);
    assert(a733_clock_acquire(&m.registry,11,A733_CLOCK_OSC,0,999999,&u)==A733_CLOCK_OK);
    assert(a733_clock_release(&m.registry,&t,999999)==A733_CLOCK_OK);
    assert(a733_clock_release(&m.registry,&u,999999)==A733_CLOCK_OK);
    assert(a733_clock_acquire(&m.registry,10,A733_CLOCK_AUDIO5,0,999999,&t)==A733_CLOCK_OK);
    assert(t.shared==(A733_CLOCK_AUDIO5|A733_CLOCK_AUDIO1|A733_CLOCK_REF|A733_CLOCK_OSC));
    assert(A733_CLOCK_LOCALS==0x3fc0 && A733_CLOCK_AUDIO_CHANGE==0x3ff8);
}
int main(void)
{
    struct model m;registry_cases();
    const unsigned profiles[]={A733_MEDIA_ALL,A733_MEDIA_VI,A733_MEDIA_DE,A733_MEDIA_VO0,A733_MEDIA_IOMMU};
    for (unsigned i=0;i<5;++i) {
        setup(&m,profiles[i]);m.auth_pending=1;m.lock_busy=1;m.reenter=1;
        assert(run(&m)==A733_MEDIA_PREPARED && !m.held && !m.retained && !m.registry.failed);
        assert(a733_clock_token_status(&m.registry,&m.driver.token)==A733_CLOCK_OK);
        terminal(&m,A733_MEDIA_PREPARED);
        if (!i) {
            printf("{\"ref\":%u,\"peri\":%u,\"video\":%u,\"de\":%u,\"writes\":[",m.ccu[0],m.ccu[0xa0/4],m.ccu[0x120/4],m.ccu[0x2e0/4]);
            for (unsigned j=0;j<m.writes;++j) printf("%s[%lu,%u]",j?",":"",(unsigned long)m.address[j],m.values[j]);
            puts("]}");
        }
    }
    /* Separate no-auto, software gate and effective-status faults for each
     * selected parent. Inherited callbacks cannot override native observations. */
    const struct {unsigned offset,mask;} bad[]={
        {0x1908,BIT(11)},{0x1908,BIT(27)},{0x1988,BIT(27)},
        {0x1910,1},{0x1910,BIT(16)},{0x1990,BIT(16)},
        {0x1928,2},{0x1928,BIT(17)},{0x19a8,BIT(17)},
        {0x5c0,BIT(3)},{0x5e0,BIT(0)},{0x5e4,BIT(18)}};
    for (unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        setup(&m,A733_MEDIA_ALL);m.ccu[bad[i].offset/4]&=~bad[i].mask;
        assert(run(&m)==A733_MEDIA_RETAINED && !m.writes && m.retained && m.registry.failed);
        terminal(&m,A733_MEDIA_RETAINED);
    }
    const unsigned live[]={0x1840,0xa00,0x1588,0x1804};
    for (unsigned i=0;i<4;++i) {
        setup(&m,A733_MEDIA_ALL);m.ccu[live[i]/4]|=BIT(31);
        assert(run(&m)==A733_MEDIA_RETAINED && !m.writes);
    }
    setup(&m,A733_MEDIA_IOMMU);m.domain[8][2]=0;
    assert(run(&m)==A733_MEDIA_RETAINED && !m.writes);
    setup(&m,A733_MEDIA_ALL);m.manager.leases[2].token++;
    assert(run(&m)==A733_MEDIA_RETAINED && !m.reads && !m.writes);
    setup(&m,A733_MEDIA_ALL);m.ccu[0x120/4]^=BIT(8);
    assert(run(&m)==A733_MEDIA_RETAINED && !m.writes);
    const unsigned points[]={1,12,24};
    for (unsigned i=0;i<3;++i) {
        setup(&m,A733_MEDIA_ALL);m.fail_write=points[i];
        assert(run(&m)==A733_MEDIA_RETAINED && m.writes==points[i] && m.retained);
        terminal(&m,A733_MEDIA_RETAINED);
    }
    setup(&m,A733_MEDIA_ALL);m.fail_write=1;m.retain_busy=1;
    while (!m.retained) assert(a733_media_clock_poll(&m.driver)==A733_MEDIA_PENDING || m.retained);
    unsigned reads=m.reads,writes=m.writes;uint64_t time=m.time;
    assert(a733_media_clock_poll(&m.driver)==A733_MEDIA_BUSY && m.reads==reads && m.writes==writes && m.time==time);
    atomic_store(&m.registry.busy,0);
    assert(a733_media_clock_poll(&m.driver)==A733_MEDIA_RETAINED && m.registry.failed);
    terminal(&m,A733_MEDIA_RETAINED);
    setup(&m,A733_MEDIA_ALL);m.expire_write=1;
    assert(run(&m)==A733_MEDIA_RETAINED && m.writes==1 && m.retained);
    puts("PASS media preparation, shared ownership, parent gates, quiescence refusal and retained faults");
    return 0;
}
