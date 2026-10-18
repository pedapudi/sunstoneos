/* Reuse actual composed provider setup, not its prior protocol test entry. */
#define main previous_pcie_main_not_run
#include "a733-pcie.c"
#undef main
static void pd_setup(void)
{
 setup();model.pmic[0x20]=0x40;model.pmic[0x2a]=13;
 model.pmic[0x2b]=31; /* BLDO2 absent/undecodable must not substitute for PD. */
}
static void no_port_mutation(void)
{
 assert(!port.pin.id&&!port.pck_lease.token&&!trace_count&&!phy_done);
}
int main(int argc,char **argv)
{
 if(argc==2&&!strcmp(argv[1],"--preflight")){
  pd_setup();enum a733_pmic_status status=A733_PMIC_PENDING;
  for(unsigned i=0;i<10000&&status!=A733_PMIC_READY;++i){status=pmic_model_step(&model);assert(status!=A733_PMIC_RETAINED);model.now+=100;}
  assert(status==A733_PMIC_READY&&model.provider.apbs0==0);
  puts("PASS nominal current APBS0/PMIC/PCIe provider admission");return 0;
 }
 assert(argc==1);
 pd_setup();assert(complete()==A733_PCIE_READY);
 assert(port.supplies[0].rail==A733_RAIL_DCDC2&&port.supplies[0].programmed_uv==800000);
 assert(port.supplies[1].rail==A733_RAIL_BLDO1&&port.supplies[1].enabled&&port.supplies[1].programmed_uv==1800000);
 a733_rail_mask_t mask=A733_RAIL_BIT(A733_RAIL_DCDC2)|A733_RAIL_BIT(A733_RAIL_BLDO1);
 assert(port.rail.shared==mask&&!port.rail.exclusive);
 assert(model.pmic[0x20]==0x40&&model.pmic[0x2a]==13&&model.pmic[0x2b]==31);
 /* Old BLDO2-only nominal hardware cannot admit the PD bank. */
 pd_setup();model.pmic[0x20]=0x80;model.pmic[0x2b]=13;
 assert(complete()==A733_PCIE_RETAINED&&!port.pin.id&&!phy_done);
 pd_setup();model.pmic[0x2a]=12;
 assert(complete()==A733_PCIE_RETAINED&&!port.pin.id&&!phy_done);
 pd_setup();model.pmic[0x20]=0;
 assert(complete()==A733_PCIE_RETAINED&&!port.pin.id&&!phy_done);
 /* BLDO1 shares with unchanged PD/PL/PE readers; writers remain exclusive. */
 pd_setup();struct a733_rail_token other={0};
 assert(a733_rail_acquire(&model.rails,555,A733_RAIL_BIT(A733_RAIL_BLDO1),0,900000,&other)==A733_RAIL_PENDING);
 assert(a733_rail_poll(&model.rails,&other)==A733_RAIL_READY);
 assert(complete()==A733_PCIE_READY&&a733_rail_status(&model.rails,&other)==A733_RAIL_READY);
 pd_setup();other=(struct a733_rail_token){0};
 assert(a733_rail_acquire(&model.rails,555,0,A733_RAIL_BIT(A733_RAIL_BLDO1),900000,&other)==A733_RAIL_PENDING);
 assert(a733_rail_poll(&model.rails,&other)==A733_RAIL_READY);
 for(unsigned i=0;i<20;++i)assert(step()==A733_PCIE_PENDING);
 no_port_mutation();assert(!port.rail.id&&a733_rail_status(&model.rails,&other)==A733_RAIL_READY);
 puts("PASS BLDO1 PD reservation/snapshot, BLDO2-only/wrong/disabled refusal, compatible reader and exclusive-writer exclusion");return 0;
}
