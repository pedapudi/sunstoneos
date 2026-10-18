#include "thermal.h"
#include <string.h>
static void put16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i));}
int thermal_collect(const struct a733_thermal_sample *s)
{
 if(!s||thermal.sets>=2||!s->sequence||s->sequence<=thermal.last_sequence||!s->observed_us||
 ((s->valid|s->alarm|s->alarm_off|s->critical)&~31))return 0;
 uint8_t *w=thermal.working;
 if(!(w[0]|w[1]|w[2]|w[3]))put32(w,s->sequence);
 put32(w+4,s->sequence);thermal.last_sequence=s->sequence;
 w[9]|=s->alarm;w[10]|=s->alarm_off;w[11]|=s->critical;
 for(unsigned i=0;i<5;++i)if(s->valid&(1u<<i)){
  if(!s->code[i]||s->code[i]>4095)return 0;
  put64(w+16+8*i,s->observed_us);put16(w+56+2*i,s->code[i]);put32(w+72+4*i,(uint32_t)s->millicelsius[i]);
 }
 w[8]|=s->valid;
 if(w[8]==31){memcpy(thermal.payload+32+96*thermal.sets,w,96);++thermal.sets;memset(w,0,96);}
 return 1;
}
