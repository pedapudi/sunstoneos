#include "a733_spi_platform.h"
#include "../drivers/power/a733_pmic.c"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static int setup_now(void *v,uint64_t *n){(void)v;*n=1;return 1;}
static int setup_current(void *v){(void)v;return 1;}
static int setup_authorize(void *v,const struct a733_spi_platform_config *c,const struct a733_clock_token *t,enum a733_spi_platform_admission p){(void)v;(void)c;(void)t;(void)p;return 1;}
static int setup_enter(void *v,uint64_t d){(void)v;(void)d;return 1;}
static void setup_leave(void *v){(void)v;}
static int setup_read32(void *v,uintptr_t a,uint32_t *n){(void)v;(void)a;(void)n;assert(0);return 0;}
static int setup_write32(void *v,uintptr_t a,uint32_t n){(void)v;(void)a;(void)n;assert(0);return 0;}
static int setup_read8(void *v,uintptr_t a,uint8_t *n){(void)v;(void)a;(void)n;assert(0);return 0;}
static int setup_write8(void *v,uintptr_t a,uint8_t n){(void)v;(void)a;(void)n;assert(0);return 0;}
int main(int argc,char **argv)
{
 struct a733_clock_leases clocks={0};assert(a733_clock_leases_init(&clocks,setup_now,NULL));
 struct a733_pins pins={0};struct a733_pin_handoff handoff={0};handoff.authorized[A733_PIN_E]=15;
 struct a733_pins_operations pin_ops={setup_enter,setup_leave,setup_now,setup_read32,setup_write32};assert(a733_pins_init_banks(&pins,&pin_ops,NULL,&handoff));
 struct a733_spi_platform_config config={.owner=23,.handoff=91,.deadline=6000000,.route=A733_SPI3_PE,.format={3,0,0,0,0,500000,2,2,0,0}};
 for(unsigned i=0;i<4;++i){config.expected[i]=(struct a733_pin_config){15,0,0};config.desired[i]=(struct a733_pin_config){5,0,0};}
 struct a733_spi_platform_ops ops={setup_now,setup_current,setup_authorize,setup_enter,setup_leave,setup_leave,setup_read32,setup_write32,setup_read8,setup_write8};struct a733_spi_platform platform={0};
 assert(a733_spi_platform_init(&platform,&config,&ops,NULL,&clocks,&pins));
 assert(platform.route.port==3&&platform.route.bank==A733_PIN_E&&platform.route.clock==1&&platform.route.mosi==2&&platform.route.miso==3&&platform.route.cs==0&&platform.route.function==5);
 assert(platform.spi.plan.base==0x02543000&&platform.spi.plan.clock==0x02002f20&&platform.spi.plan.reset==0x02002f24&&platform.spi.plan.intid==59&&platform.spi.plan.actual_hz==500000);
 struct profile p;assert(profile(A733_RAIL_BLDO1,&p)&&p.enable==0x20&&p.bit==6&&p.selector==0x2a&&p.mask==31);
 struct a733_pmic_snapshot s;
 for(unsigned i=0;i<32;++i){assert(a733_pmic_decode(A733_RAIL_BLDO1,64,(uint8_t)i,&s));assert(s.rail==A733_RAIL_BLDO1&&s.enabled==1&&s.decoded==(i<=29)&&s.programmed_uv==(i<=29?(500+100*i)*1000:0));}
 assert(a733_pmic_decode(A733_RAIL_BLDO1,0,13,&s)&&!s.enabled&&s.programmed_uv==1800000);
 assert(argc==2);FILE *f=fopen(argv[1],"w");assert(f);
 assert(fprintf(f,"{\"spi_scope\":%llu,\"parent_scope\":%llu,\"rail_index\":%u,\"actual_hz\":%u,\"format_valid\":true}\n",(unsigned long long)A733_CLOCK_SPI3,(unsigned long long)(A733_CLOCK_OSC|A733_CLOCK_REF|A733_CLOCK_FABRIC),A733_RAIL_BLDO1,platform.spi.plan.actual_hz)>0&&!fclose(f));
 puts("PASS actual SPI3 platform/route/clock/pin nominal initialization and additive BLDO1 decode; no MMIO");
}
