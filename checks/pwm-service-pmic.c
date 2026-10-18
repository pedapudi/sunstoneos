/* Execute only new RPIO parent/profile behavior through the actual PMIC core. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
static struct pmic_model m;
static void ready_bus(void)
{
 for(unsigned i=0;i<16&&a733_pmic_live(&m.provider,&m.lifetime)!=A733_PMIC_READY;++i)
  assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
 assert(a733_pmic_live(&m.provider,&m.lifetime)==A733_PMIC_READY);
 assert(m.provider.clock.shared==(A733_CLOCK_APBS0|A733_CLOCK_APBS1|A733_CLOCK_REF|A733_CLOCK_OSC));
}
static struct a733_pmic_request submit(void)
{
 struct a733_rail_token t={0};
 assert(a733_rail_acquire(&m.rails,42,A733_RAIL_BIT(A733_RAIL_DCDC1),0,900000,&t)==A733_RAIL_PENDING);
 for(unsigned i=0;i<16&&a733_rail_status(&m.rails,&t)!=A733_RAIL_READY;++i)
  assert(a733_rail_poll(&m.rails,&t)!=A733_RAIL_RETAINED);
 struct a733_pmic_request q={0};assert(a733_pmic_submit(&m.provider,42,&t,A733_RAIL_DCDC1,900000,&q)==A733_PMIC_PENDING);
 return q;
}
int main(void)
{
 for(unsigned source=0;source<=4;source+=4){
  pmic_model_setup(&m);*pmic_model_word(&m,0x0701000c)=(source<<24)|31;
  *pmic_model_word(&m,0x02002000)=0xf8000000u|(99u<<16)|(99u<<8);
  ready_bus();assert(m.provider.apbs0==((source<<24)|31));
  struct a733_clock_token change={0};
  assert(a733_clock_acquire(&m.clocks,99,0,A733_CLOCK_APBS0,900000,&change)==A733_CLOCK_BUSY);
  struct a733_pmic_request q=submit();(void)q;unsigned n=m.writes;
  *pmic_model_word(&m,0x0701000c)^=1;
  assert(pmic_model_step(&m)==A733_PMIC_RETAINED&&m.writes==n&&m.retained);
 }
 for(unsigned source=1;source<=3;++source){
  pmic_model_setup(&m);*pmic_model_word(&m,0x0701000c)=source<<24;
  assert(pmic_model_step(&m)==A733_PMIC_PENDING);
  assert(pmic_model_step(&m)==A733_PMIC_RETAINED&&!m.writes&&!m.provider.pin.id);
 }
 pmic_model_setup(&m);*pmic_model_word(&m,0x0701000c)=4u<<24;
 assert(pmic_model_step(&m)==A733_PMIC_PENDING);
 assert(pmic_model_step(&m)==A733_PMIC_RETAINED&&!m.writes&&!m.provider.pin.id); /* REF not ready. */
 assert(a733_pmic_native_address(0x0701000c,0)&&!a733_pmic_native_address(0x0701000c,1));
 puts("PASS: actual PMIC reserves APBS0, validates source/divider+REF before RPIO, retains changed/unsupported profile");
}
