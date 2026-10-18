#include "a733_adc.h"
#include "a733_thermal.h"
#include "a733_pmic.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
 uint32_t words[3]={250,0,0xabcd1200};const unsigned codes[]={2420,2400,2500,1769,1770};
 for(unsigned i=0;i<5;++i)for(unsigned j=0;j<12;++j)if(codes[i]&(1u<<j)){unsigned b=12+12*i+j;words[b/32]|=1u<<(b%32);}
 struct a733_thermal_calibration c;assert(a733_thermal_calibrate(words,&c));
 struct a733_pmic_snapshot s;assert(a733_pmic_decode(A733_RAIL_CLDO3,32,13,&s)&&s.enabled&&s.decoded&&s.programmed_uv==1800000);
 printf("{\"rail_index\":%u,\"factory_tenth_c\":%u,\"factory_codes\":[",A733_RAIL_CLDO3,c.factory_tenth_c);
 for(unsigned i=0;i<5;++i)printf("%s%u",i?",":"",c.factory_code[i]);printf("],\"corrections\":[");
 for(unsigned i=0;i<5;++i)printf("%s%u",i?",":"",c.correction[i]);puts("]}");
}
