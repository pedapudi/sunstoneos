/* New USB supply profiles through the actual shared PMIC provider. The
 * programmed model voltages are test inputs, not qualified board defaults. */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"
static struct pmic_model model;
static struct a733_pmic_snapshot snapshot(struct a733_rail_token *token,enum a733_rail rail)
{
    struct a733_pmic_request request={0};struct a733_pmic_snapshot out={0};
    assert(a733_pmic_submit(&model.provider,9001,token,rail,1000000,&request)==A733_PMIC_PENDING);
    for(unsigned i=0;i<150&&a733_pmic_result(&model.provider,&request,&out)==A733_PMIC_PENDING;++i)
        assert(pmic_model_step(&model)!=A733_PMIC_RETAINED);
    assert(a733_pmic_result(&model.provider,&request,&out)==A733_PMIC_COMPLETE);
    assert(out.rail==rail&&out.chip==3&&!out.revision&&!out.page_before&&!out.page_after);
    assert(a733_pmic_reap(&model.provider,&request)==A733_PMIC_COMPLETE);
    return out;
}
int main(void)
{
    pmic_model_setup(&model);model.pmic[0x11]=0x11;model.pmic[0x1a]=100;
    model.pmic[0x23]=8;model.pmic[0x3f]=16;
    for(unsigned i=0;i<16&&a733_pmic_live(&model.provider,&model.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&model)!=A733_PMIC_RETAINED);
    assert(a733_pmic_live(&model.provider,&model.lifetime)==A733_PMIC_READY);
    const enum a733_rail rails[]={A733_RAIL_DCDC1,A733_RAIL_DC1SW2,A733_RAIL_DCDC9,A733_RAIL_ELDO6};
    const unsigned uv[]={3300000,3300000,1800000,900000};
    a733_rail_mask_t mask=0;for(unsigned i=0;i<4;++i)mask|=A733_RAIL_BIT(rails[i]);
    struct a733_rail_token token={0};
    assert(a733_rail_acquire(&model.rails,9001,mask,0,1000000,&token)==A733_RAIL_PENDING);
    enum a733_rail_result r=A733_RAIL_PENDING;
    for(unsigned i=0;i<16&&r!=A733_RAIL_READY;++i)r=a733_rail_poll(&model.rails,&token);
    assert(r==A733_RAIL_READY);
    for(unsigned i=0;i<4;++i) {
        struct a733_pmic_snapshot s=snapshot(&token,rails[i]);
        assert(s.enabled==1&&s.decoded==1&&s.programmed_uv==uv[i]);
    }
    model.pmic[0x1a]=102;struct a733_pmic_snapshot s=snapshot(&token,A733_RAIL_DCDC9);
    assert(s.enabled==1&&!s.decoded);
    model.pmic[0x3f]=41;s=snapshot(&token,A733_RAIL_ELDO6);assert(s.enabled==1&&!s.decoded);
    model.pmic[0x11]&=~1u;model.pmic[0x1a]=100;
    s=snapshot(&token,A733_RAIL_DCDC9);assert(!s.enabled&&s.decoded&&s.programmed_uv==1800000);
    model.pmic[0x23]=0;model.pmic[0x3f]=16;
    s=snapshot(&token,A733_RAIL_ELDO6);assert(!s.enabled&&s.decoded&&s.programmed_uv==900000);
    puts("PASS: actual four-rail PMIC snapshots and new-profile reserved/disabled refusal inputs");
    return 0;
}
