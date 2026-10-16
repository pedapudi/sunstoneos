#define _GNU_SOURCE
#include "a733_spi_native.h"
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

static unsigned authentications;
static int authority(void *p,const struct a733_spi_native *n)
{assert(p==(void*)1&&n->lease==17);authentications++;return 1;}
static int now(void *p,uint64_t *v){assert(p==(void*)1);*v=1;return 1;}
static void map(uintptr_t a,size_t n)
{
    assert(mmap((void*)a,n,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)a);
}
static void put(uintptr_t a,uint32_t v){*(volatile uint32_t*)a=v;}
int main(void)
{
    map(0x02001000,0x1000);map(0x07010000,0x1000);map(0x03400000,0x1000);
    for(unsigned port=0;port<6;port++) {
        struct a733_spi_native n={.format={port,0,0,0,0,500000,3,5},
            .lease=17,.provider=(void*)1,.authenticate=authority,.now_us=now};
        assert(!a733_spi_plan(port,500000,&n.plan));
        put(n.plan.clock,n.plan.clock_word);put(n.plan.reset,0x10001);
        struct a733_spi_ops ops;assert(!a733_spi_native_ops(&n,&ops));
        assert(ops.admit(&n,&n.format,&n.plan)==1);
        for(unsigned field=0;field<8;field++) {
            struct a733_spi_format f=n.format;
            switch(field) {
            case 0:f.port++;break;case 1:f.cs++;break;case 2:f.mode++;break;
            case 3:f.lsb_first++;break;case 4:f.active_high++;break;
            case 5:f.max_hz++;break;case 6:f.setup_us++;break;case 7:f.hold_us++;break;
            }
            unsigned before=authentications;assert(!ops.admit(&n,&f,&n.plan));
            assert(authentications==before); /* Refuse before provider or MMIO. */
        }
        struct a733_spi_plan wrong=n.plan;wrong.clock_word^=1;
        assert(!ops.admit(&n,&n.format,&wrong));
        put(n.plan.clock,n.plan.clock_word^1);assert(!ops.admit(&n,&n.format,&n.plan));
        put(n.plan.clock,n.plan.clock_word);
        for(unsigned i=0;i<2;i++) {
            unsigned mask=i?0x10000u:1u;
            put(n.plan.reset,0x10001^mask);assert(!ops.admit(&n,&n.format,&n.plan));
        }
        put(n.plan.reset,0x10001);
        uintptr_t word=4*(n.plan.intid/32);uint32_t bit=1u<<(n.plan.intid%32);
        put(0x03400100+word,bit);assert(!ops.admit(&n,&n.format,&n.plan));put(0x03400100+word,0);
        put(0x03400300+word,bit);assert(!ops.admit(&n,&n.format,&n.plan));put(0x03400300+word,0);
        put(0x03400000,0x80000000);assert(!ops.admit(&n,&n.format,&n.plan));put(0x03400000,0);
        assert(ops.admit(&n,&n.format,&n.plan)==1);
        uint32_t v;assert(!ops.read32(&n,n.plan.base+0x200,&v));
        assert(!ops.write32(&n,n.plan.base+0x400,0));
        assert(!ops.write8(&n,n.plan.base+0x300,0));
    }
    puts("Native SPI binding, CCU/GIC admission and access allowlists passed in host memory");
}
