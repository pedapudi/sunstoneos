#include "a733_spi_native.h"

static int valid(const struct a733_spi_native *n)
{
    struct a733_spi_plan p;
    return n && n->lease && n->authenticate && n->now_us &&
        n->format.cs<4 && n->format.mode<4 && n->format.lsb_first<2 &&
        n->format.active_high<2 && !a733_spi_plan(n->format.port,n->format.max_hz,&p) &&
        n->plan.base==p.base && n->plan.clock==p.clock && n->plan.reset==p.reset &&
        n->plan.clock_word==p.clock_word && n->plan.divisor==p.divisor &&
        n->plan.actual_hz==p.actual_hz && n->plan.intid==p.intid;
}
static uint32_t load(uintptr_t a)
{
    uint32_t v=*(volatile uint32_t *)a;
    __asm__ volatile("dsb sy" ::: "memory");return v;
}
static int admit(void *ctx,const struct a733_spi_format *f,const struct a733_spi_plan *p)
{
    const struct a733_spi_native *n=ctx;
    if(!valid(n)||!f||!p||f->port!=n->format.port||f->cs!=n->format.cs||
       f->mode!=n->format.mode||f->lsb_first!=n->format.lsb_first||
       f->active_high!=n->format.active_high||f->max_hz!=n->format.max_hz||
       f->setup_us!=n->format.setup_us||f->hold_us!=n->format.hold_us||
       p->base!=n->plan.base||p->clock!=n->plan.clock||p->reset!=n->plan.reset||
       p->clock_word!=n->plan.clock_word||p->divisor!=n->plan.divisor||
       p->actual_hz!=n->plan.actual_hz||p->intid!=n->plan.intid||
       n->authenticate(n->provider,n)!=1)return 0;
    uint32_t bit=1u<<(n->plan.intid%32u);
    uintptr_t word=4u*(n->plan.intid/32u);
    if(load(n->plan.clock)!=n->plan.clock_word ||
       (load(n->plan.reset)&0x10001u)!=0x10001u ||
       (load(0x03400000u)&0x80000000u) ||
       (load(0x03400100u+word)&bit) || (load(0x03400300u+word)&bit))return 0;
    return n->authenticate(n->provider,n)==1;
}
static int offset(void *ctx,uintptr_t a,unsigned *o)
{
    const struct a733_spi_native *n=ctx;
    if(!valid(n)||a<n->plan.base||a-n->plan.base>0x400u)return 0;
    *o=(unsigned)(a-n->plan.base);return 1;
}
static int read32(void *ctx,uintptr_t a,uint32_t *v)
{
    unsigned o;if(!v||!offset(ctx,a,&o))return 0;
    switch(o) {
    case 0x04:case 0x08:case 0x14:case 0x18:case 0x1c:case 0x400:
        *v=load(a);return 1;
    default:return 0;
    }
}
static int write32(void *ctx,uintptr_t a,uint32_t v)
{
    unsigned o;if(!offset(ctx,a,&o))return 0;
    switch(o) {
    case 0x04:case 0x08:case 0x10:case 0x14:case 0x18:case 0x20:
    case 0x30:case 0x34:case 0x38:case 0x40:
        *(volatile uint32_t *)a=v;__asm__ volatile("dsb sy" ::: "memory");return 1;
    default:return 0;
    }
}
static int read8(void *ctx,uintptr_t a,uint8_t *v)
{
    unsigned o;if(!v||!offset(ctx,a,&o)||o!=0x300)return 0;
    *v=*(volatile uint8_t *)a;__asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int write8(void *ctx,uintptr_t a,uint8_t v)
{
    unsigned o;if(!offset(ctx,a,&o)||o!=0x200)return 0;
    *(volatile uint8_t *)a=v;__asm__ volatile("dsb sy" ::: "memory");return 1;
}
static int now(void *ctx,uint64_t *t)
{
    const struct a733_spi_native *n=ctx;
    return valid(n)&&n->now_us(n->provider,t)==1;
}
int a733_spi_native_ops(const struct a733_spi_native *n,struct a733_spi_ops *ops)
{
    if(!ops||!valid(n))return -1;
    *ops=(struct a733_spi_ops){admit,read32,write32,read8,write8,now};return 0;
}
