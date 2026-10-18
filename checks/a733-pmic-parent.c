/* Only the APBS1 inherited-parent addition; the actual shared provider runs. */
#define PMIC_MODEL_NO_MAIN 1
#include "a733-pmic.c"
static uint32_t pll(unsigned n,unsigned m)
{return UINT32_C(0xf8000000)|((n-1)<<8)|((m-1)<<16);}
static void parent(struct pmic_model *m,unsigned source,uint32_t reference)
{pmic_model_setup(m);*pmic_model_word(m,0x07010010)=source<<24;*pmic_model_word(m,0x02002000)=reference;}
static void ready_parent(struct pmic_model *m)
{
    for(unsigned i=0;i<16 && a733_pmic_live(&m->provider,&m->lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(m)!=A733_PMIC_RETAINED);
    assert(a733_pmic_live(&m->provider,&m->lifetime)==A733_PMIC_READY);
}
static void submit(struct pmic_model *m,struct a733_rail_token *token,struct a733_pmic_request *request)
{
    assert(a733_rail_acquire(&m->rails,10,A733_RAIL_BIT(A733_RAIL_DCDC1),0,1000000,token)==A733_RAIL_PENDING);
    enum a733_rail_result r=A733_RAIL_PENDING;
    for(unsigned i=0;i<16 && r!=A733_RAIL_READY;++i)r=a733_rail_poll(&m->rails,token);
    assert(r==A733_RAIL_READY);
    assert(a733_pmic_submit(&m->provider,10,token,A733_RAIL_DCDC1,1000000,request)==A733_PMIC_PENDING);
}
static void held(struct pmic_model *m)
{
    assert(m->provider.failed && m->retained && m->i2c_held && m->provider.clock.id);
    unsigned reads=m->reads,writes=m->writes;
    assert(pmic_model_step(m)==A733_PMIC_RETAINED && m->reads==reads && m->writes==writes);
}
int main(void)
{
    static struct pmic_model m;
    parent(&m,4,pll(100,100));ready_parent(&m);
    assert(m.provider.apbs1==0x04000000 && m.provider.divider==0x28);
    struct a733_rail_token token={0};struct a733_pmic_request request;struct a733_pmic_snapshot result={0};
    submit(&m,&token,&request);
    for(unsigned i=0;i<150 && a733_pmic_result(&m.provider,&request,&result)==A733_PMIC_PENDING;++i)
        assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
    assert(a733_pmic_result(&m.provider,&request,&result)==A733_PMIC_COMPLETE && result.programmed_uv==3300000);
    assert(*pmic_model_word(&m,0x07010010)==0x04000000 && *pmic_model_word(&m,0x02002000)==pll(100,100) &&
        *pmic_model_word(&m,0x07083014)==0x28 && !m.retained);
    for(unsigned i=0;i<m.trace_count;++i)assert(m.trace[i].address==0x07083008 || m.trace[i].address==0x0708300c);
    /* Legacy source0 needs no running PLL_REF. Both accepted sources are fixed. */
    parent(&m,0,0);ready_parent(&m);assert(!m.provider.apbs1);
    for(unsigned source=0;source<=4;source+=4) {
        parent(&m,source,pll(100,100));ready_parent(&m);
        token=(struct a733_rail_token){0};submit(&m,&token,&request);
        *pmic_model_word(&m,0x07010010)=(source?0u:4u)<<24;
        assert(pmic_model_step(&m)==A733_PMIC_RETAINED && !m.writes);held(&m);
        assert(m.provider.pin.id && a733_pmic_result(&m.provider,&request,&result)==A733_PMIC_RETAINED);
    }
    const uint32_t bad[]={pll(100,100)&~(1u<<31),pll(100,100)&~(1u<<28),
        pll(100,101),pll(79,79),pll(117,117)};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        parent(&m,4,bad[i]);assert(pmic_model_step(&m)==A733_PMIC_PENDING);
        assert(pmic_model_step(&m)==A733_PMIC_RETAINED && !m.provider.pin.id && !m.writes);held(&m);
    }
    /* Loss of the admitted reference is refused before a later transaction. */
    parent(&m,4,pll(100,100));ready_parent(&m);token=(struct a733_rail_token){0};submit(&m,&token,&request);
    *pmic_model_word(&m,0x02002000)=pll(100,99);
    assert(pmic_model_step(&m)==A733_PMIC_RETAINED && !m.writes);held(&m);
    assert(a733_pmic_native_address(0x02002000,0) && !a733_pmic_native_address(0x02002000,1) &&
        !a733_pmic_native_address(0x07010010,1) && !a733_pmic_native_address(0x07083014,1));
    puts("PASS actual PMIC source4 adoption, fixed parent, 24MHz PLL refusals and unchanged source0; no clock/divisor writes");
    return 0;
}
