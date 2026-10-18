#include "a733_rtc_boot.h"
#define GIC UINT32_C(0x03400000)
#define BIT (UINT32_C(1)<<4) /* INTID228: bank7 bit4. */
#define BUS UINT32_C(0x0701020c)
#define RTC UINT32_C(0x07090000)
static int rd(struct a733_rtc_boot *b,uintptr_t p,uint32_t *v)
{return b->ops.read(b->context,p,v)==1;}
static int wr(struct a733_rtc_boot *b,uintptr_t p,uint32_t v)
{return b->ops.write(b->context,p,v)==1;}
static int quiet(struct a733_rtc_boot *b)
{
 uint32_t v;
 const unsigned offsets[]={0x11c,0x21c,0x31c};
 for(unsigned i=0;i<3;++i)if(!rd(b,GIC+offsets[i],&v)||(v&BIT))return 0;
 return 1;
}
static int retain(struct a733_rtc_boot *b){b->state=3;return 0;}
int a733_rtc_boot_bus(struct a733_rtc_boot *b,const struct a733_rtc_boot_ops *ops,void *c)
{
 if(!b||b->state||!ops||!ops->read||!ops->write||!ops->route)return 0;
 b->ops=*ops;b->context=c;b->state=3;
 if(!quiet(b)||!rd(b,BUS,&b->bus_before))return 0;
 /* Release only: never assert reset. Preserve every other R_CCU field. */
 if(!wr(b,BUS,b->bus_before|0x10001)||!rd(b,BUS,&b->bus_after)||
    b->bus_after!=(b->bus_before|0x10001))return 0;
 const unsigned offsets[]={0x28,0x2c,0x30,0x50};uint32_t v;
 for(unsigned i=0;i<4;++i)if(!rd(b,RTC+offsets[i],&v)||(v&1))return 0;
 if(!quiet(b))return 0;
 b->state=1;return 1;
}
int a733_rtc_boot_route(struct a733_rtc_boot *b,uint64_t affinity)
{
 uint32_t v;
 if(!b||b->state!=1||(affinity&~UINT64_C(0xff00ffffff)))return 0;
 if(!quiet(b)||!rd(b,GIC,&v)||(v&0x73)!=0x33)return retain(b);
 /* NS Group1, level, priority0xe0. RMW shared words, write exact own route. */
 if(!rd(b,GIC+0x9c,&v)||!wr(b,GIC+0x9c,v|BIT)||
    !rd(b,GIC+0xd1c,&v)||!wr(b,GIC+0xd1c,v&~BIT)||
    !rd(b,GIC+0xc38,&v)||!wr(b,GIC+0xc38,v&~UINT32_C(0x300))||
    !rd(b,GIC+0x4e4,&v)||!wr(b,GIC+0x4e4,(v&~UINT32_C(255))|0xe0)||
    b->ops.route(b->context,GIC+0x6720,affinity)!=1)return retain(b);
 if(!rd(b,GIC+0x9c,&v)||!(v&BIT)||!rd(b,GIC+0xd1c,&v)||(v&BIT)||
    !rd(b,GIC+0xc38,&v)||(v&0x300)||!rd(b,GIC+0x4e4,&v)||(v&255)!=0xe0||
    !rd(b,GIC+0x6720,&v)||v!=(uint32_t)affinity||
    !rd(b,GIC+0x6724,&v)||v!=(uint32_t)(affinity>>32)||!quiet(b))return retain(b);
 b->state=2;return 1;
}
