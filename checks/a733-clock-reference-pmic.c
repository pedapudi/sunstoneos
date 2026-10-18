#include "pmic-parent-fixture.h"
static void actual(struct pmic_model *m,unsigned source,unsigned divider)
{
 parent(m,source,pll(96,104));*pmic_model_word(m,0x07090160)=0x983fb0f7;
 *pmic_model_word(m,0x07083014)=divider;
}
int main(void)
{
 static struct pmic_model m;
 actual(&m,4,0x28);ready_parent(&m);
 assert(!m.provider.apbs0&&m.provider.apbs1==0x04000000&&m.provider.dcxo_hz==26000000);
 struct a733_rail_token token={0};struct a733_pmic_request request;struct a733_pmic_snapshot result={0};
 submit(&m,&token,&request);
 for(unsigned i=0;i<150&&a733_pmic_result(&m.provider,&request,&result)==A733_PMIC_PENDING;++i)assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
 assert(a733_pmic_result(&m.provider,&request,&result)==A733_PMIC_COMPLETE&&result.programmed_uv==3300000);
 assert(*pmic_model_word(&m,0x07083014)==0x28&&*pmic_model_word(&m,0x07010010)==0x04000000);
 actual(&m,0,0x28);assert(pmic_model_step(&m)==A733_PMIC_PENDING);assert(pmic_model_step(&m)==A733_PMIC_RETAINED&&!m.writes);held(&m);
 actual(&m,0,0x30);ready_parent(&m);assert(m.provider.dcxo_hz==26000000);
 for(unsigned loss=0;loss<3;++loss){
  actual(&m,0,0x30);ready_parent(&m);token=(struct a733_rail_token){0};submit(&m,&token,&request);
  if(loss==0)*pmic_model_word(&m,0x07090160)&=~(3u<<14); /*26->24,still valid baud*/
  else if(loss==1)*pmic_model_word(&m,0x07083014)=0x38; /*still valid baud*/
  else *pmic_model_word(&m,0x07010010)=0x04000000;
  assert(pmic_model_step(&m)==A733_PMIC_RETAINED&&!m.writes);held(&m);
 }
 puts("PASS actual26MHz PMIC REF/raw baud paths, over400k refusal, retained frequency/divisor/source drift");
}
