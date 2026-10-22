/* SPDX-License-Identifier: GPL-2.0-only */
/* Display PCK domain provider over modeled registers. For DE_SYS (1) and VO1
 * (10) an OFF domain receives the pinned sun60iw2 delay row and one ON8
 * request at its own block, preserving other command bits; an ON domain is
 * adopted without writes. Other domains, incoherent initial state and a
 * status that stays OFF past the 10 ms bound are refused. */
#include "a733_de_sys_power.h"
#include <assert.h>
#include <stdio.h>

static struct { uintptr_t address; uint32_t value; } trace[16];
static unsigned trace_count,status_delay,authorize_busy;
static uint32_t command,status,delays[5];
static uintptr_t base;
static uint64_t now_us,step_us;

static int authorize(void *v){(void)v;if(authorize_busy){--authorize_busy;return 0;}return 1;}
static int now(void *v,uint64_t *out){(void)v;now_us+=step_us;*out=now_us;return 1;}
static int enter(void *v,uint64_t d){(void)v;assert(d>now_us);return 1;}
static void leave(void *v){(void)v;}
static uint32_t *legacy_word(uintptr_t a)
{
    static const unsigned offsets[]={0x170,0x174,0xc00,0xc04,0xc10};
    assert(a>=base && a<base+0x1000);
    if(a==base)return &command;
    if(a==base+8)return &status;
    for(unsigned i=0;i<5;++i)if(a==base+offsets[i])return &delays[i];
    assert(0);return NULL;
}
static int legacy_read(void *v,uintptr_t a,uint32_t *out)
{
    (void)v;
    if(a==base+8 && (command&15)==8 && (status&15)==0 && !status_delay--)status=(status&~15u)|8;
    *out=*legacy_word(a);return 1;
}
static int legacy_write(void *v,uintptr_t a,uint32_t value)
{
    (void)v;assert(a!=base+8 && trace_count<16);
    trace[trace_count].address=a;trace[trace_count++].value=value;*legacy_word(a)=value;return 1;
}
static const struct a733_de_sys_power_ops ops={authorize,now,enter,leave,legacy_read,legacy_write};

static void reset(unsigned domain,uint32_t initial_command,uint32_t initial_status)
{
    base=UINT32_C(0x07060000)+domain*UINT32_C(0x1000);
    trace_count=0;status_delay=0;authorize_busy=0;now_us=100;step_us=1;
    command=initial_command;status=initial_status;
    for(unsigned i=0;i<5;++i)delays[i]=0;
}
static int run(struct a733_de_sys_power *p)
{
    int r=0;
    for(unsigned i=0;i<100000 && !r;++i)r=a733_de_sys_power_poll(p);
    return r;
}
int main(void)
{
    _Static_assert(A733_DE_SYS_PCK_BASE==UINT32_C(0x07061000),"DE_SYS block");
    _Static_assert(A733_PCK_DOMAIN_BASE(10)==UINT32_C(0x0706a000),"VO1 block");
    static const unsigned domains[]={1,10};
    for(unsigned i=0;i<2;++i){
        /* OFF: delay row, then ON8 with bit 8 of the command word preserved. */
        struct a733_de_sys_power p={0};
        reset(domains[i],0x100,0);status_delay=3;authorize_busy=2;
        assert(a733_de_sys_power_init_domain(&p,&ops,NULL,1000000,domains[i]) && p.base==base);
        assert(run(&p)==1 && a733_de_sys_power_ready(&p) && !p.inherited_on && p.write_attempted);
        const uint32_t expected[][2]={{0x170,0x1f1f1f},{0x174,0x1f1f},{0xc00,0x08080808},
            {0xc04,0x0808},{0xc10,8},{0,0x108}};
        assert(trace_count==6);
        for(unsigned j=0;j<6;++j)
            assert(trace[j].address==base+expected[j][0] && trace[j].value==expected[j][1]);
        /* ON: adopted without any write. */
        struct a733_de_sys_power q={0};
        reset(domains[i],8,8);
        assert(a733_de_sys_power_init_domain(&q,&ops,NULL,1000000,domains[i]));
        assert(run(&q)==1 && a733_de_sys_power_ready(&q) && q.inherited_on && !trace_count);
        /* Incoherent initial command/status is refused before any write. */
        struct a733_de_sys_power r={0};
        reset(domains[i],8,0);status_delay=UINT32_MAX;
        assert(a733_de_sys_power_init_domain(&r,&ops,NULL,1000000,domains[i]));
        assert(run(&r)==-1 && !trace_count && !a733_de_sys_power_ready(&r));
        /* Status still OFF after 10 ms fails without a second request. */
        struct a733_de_sys_power s={0};
        reset(domains[i],0,0);status_delay=UINT32_MAX;step_us=50;
        assert(a733_de_sys_power_init_domain(&s,&ops,NULL,1000000,domains[i]));
        assert(run(&s)==-1 && trace_count==6 && now_us<20000);
        assert(a733_de_sys_power_poll(&s)==-1 && trace_count==6);
    }
    /* Only domains 1 and 10 are accepted; the old initializer is DE_SYS. */
    for(unsigned d=0;d<16;++d)if(d!=1 && d!=10){
        struct a733_de_sys_power p={0};
        assert(!a733_de_sys_power_init_domain(&p,&ops,NULL,1000000,d) && !p.self);
    }
    struct a733_de_sys_power p={0};
    assert(a733_de_sys_power_init(&p,&ops,NULL,1000000) && p.base==A733_DE_SYS_PCK_BASE);
    puts("PASS display PCK domains 1 and 10: ON sequence, adoption and refusals");
    return 0;
}
