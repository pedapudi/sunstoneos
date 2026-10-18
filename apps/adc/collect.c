#include "adc.h"
#include <string.h>
int adc_collect(const struct a733_adc_sample *s)
{
 if(!s||!s->valid||(s->valid&~15u)||s->sequence<=adc.last_sequence||adc.sets>=2)return 0;
 adc.last_sequence=s->sequence;
 for(unsigned i=0;i<4;++i)if(s->valid&(1u<<i)){
  if(s->code[i]>4095||!s->observed_us[i]||s->observed_us[i]<=adc.last_times[i])return 0;
  adc.last_times[i]=s->observed_us[i];uint8_t *p=adc.payload+32+64*adc.sets+16*i;
  memcpy(p,&s->observed_us[i],8);memcpy(p+8,&s->code[i],2);p[10]=(uint8_t)i;p[11]=0;memcpy(p+12,&s->sequence,4);
  adc.valid|=1u<<i;
 }
 if(adc.valid==15){++adc.sets;adc.valid=0;}return 1;
}
