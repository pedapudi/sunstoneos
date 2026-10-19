/* Pure profile/actual-planner boundary; no device callbacks. */
#include "../apps/dsi-scanout/profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv)
{
 if(argc==2&&!strcmp(argv[1],"--factors")){struct a733_dsi_pll_plan p;assert(a733_dsi_pll_plan(480000000,80000000,&p));printf("%u\n",p.factors);return 0;}
 assert(argc==2);struct dsi_session s,original;FILE *f=fopen(argv[1],"rb");assert(f&&fread(&s,1,sizeof(s),f)==sizeof(s)&&fgetc(f)==EOF&&!fclose(f));original=s;
 assert(dsi_profile_valid(&s));assert(dsi_frame_bytes(&s)>2*1024*1024);
 s.values[DSI_PITCH]=UINT32_MAX;assert(!dsi_profile_valid(&s));s=original;
 s.values[DSI_WIDTH]=0x10001;assert(!dsi_profile_valid(&s));s=original;
 s.values[DSI_PADDING]=UINT32_MAX;assert(!dsi_profile_valid(&s));s=original;
 s.pins[9]=s.pins[0];assert(!dsi_profile_valid(&s));s=original;
 s.pins[0].desired[0]=2;assert(!dsi_profile_valid(&s));s=original;
 s.steps[0].offset=1535;s.steps[0].bytes=250;assert(!dsi_profile_valid(&s));s=original;
 s.steps[0].phase=1;assert(!dsi_profile_valid(&s));s=original;
 s.values[DSI_PAYLOAD_BYTES]=1537;assert(!dsi_profile_valid(&s));s=original;
 s.values[DSI_HS]++;assert(!dsi_profile_valid(&s));s=original;
 s.reserved[511]=1;assert(!dsi_profile_valid(&s));s=original;
 s.supplies[0].rail=A733_RAIL_DCDC1;assert(!dsi_profile_valid(&s));
 puts("PASS real profile producer geometry and11 malformed profile/range/phase/pin/PLL cases; no callbacks");return 0;
}
