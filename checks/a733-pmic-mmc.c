#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"

static void outputs(int print)
{
    const enum a733_rail rails[]={A733_RAIL_DC1SW2,A733_RAIL_CLDO5};
    const char *names[]={"dc1sw2","cldo5"};
    const unsigned maximum[]={28,29},minimum[]={1000000,500000},bit[]={4,7};
    for(unsigned r=0;r<2;++r)for(unsigned raw=0;raw<256;++raw) {
        unsigned code=raw&31;
        for(unsigned e=0;e<3;++e) {
            unsigned enable=e==0 ? 0 : e==1 ? (1u<<bit[r]) : (255u^(1u<<bit[r]));
            struct a733_pmic_snapshot s={0};
            assert(a733_pmic_decode(rails[r],enable,raw,&s));
            assert(s.rail==rails[r] && s.enable==enable && s.selector==raw);
            assert(s.enabled==(e==1) && s.decoded==(code<=maximum[r]));
            assert(s.programmed_uv==(s.decoded ? minimum[r]+code*100000 : 0));
            if(print && e==1 && s.decoded)printf("%s %u %u\n",names[r],raw,s.programmed_uv/1000);
        }
    }
}
static struct pmic_model m;
static void snapshots(void)
{
    pmic_model_setup(&m);
    m.pmic[0x11]=1u<<4;m.pmic[0x21]=1u<<7;m.pmic[0x12]=23;m.pmic[0x33]=13;
    for(unsigned i=0;i<16 && a733_pmic_live(&m.provider,&m.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
    assert(a733_pmic_live(&m.provider,&m.lifetime)==A733_PMIC_READY);
    const a733_rail_mask_t mask=A733_RAIL_BIT(A733_RAIL_DCDC1)|
        A733_RAIL_BIT(A733_RAIL_DC1SW2)|A733_RAIL_BIT(A733_RAIL_CLDO5);
    struct a733_rail_token t;
    assert(a733_rail_acquire(&m.rails,10,mask,0,1000000,&t)==A733_RAIL_PENDING);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY);
    const enum a733_rail rail[]={A733_RAIL_DC1SW2,A733_RAIL_CLDO5};
    const unsigned enable[]={0x11,0x21},selector[]={0x12,0x33},uv[]={3300000,1800000};
    for(unsigned r=0;r<2;++r) {
        struct a733_pmic_request q={0};struct a733_pmic_snapshot s={0};
        m.trace_count=0;
        assert(a733_pmic_submit(&m.provider,10,&t,rail[r],1000000,&q)==A733_PMIC_PENDING && q.id);
        assert(a733_rail_release(&m.rails,&t,1000000)==A733_RAIL_BUSY);
        for(unsigned i=0;i<150 && a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_PENDING;++i)
            assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
        assert(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_COMPLETE);
        assert(s.rail==rail[r] && s.enabled && s.decoded && s.programmed_uv==uv[r]);
        assert(s.enable==m.pmic[enable[r]] && s.selector==m.pmic[selector[r]]);
        assert(!m.i2c_held && !s.page_before && s.chip==3 && !s.revision && !s.page_after);
        const unsigned registers[]={0xff,0x0e,0x0f,enable[r],selector[r],0xff};
        unsigned data=0;
        for(unsigned i=0;i<m.trace_count;++i)if(m.trace[i].address==0x07083008) {
            assert(data<18);
            unsigned expected=data%3==0 ? 0x6c : data%3==1 ? registers[data/3] : 0x6d;
            assert(m.trace[i].value==expected);++data;
        }
        assert(data==18);
        assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    }
    assert(a733_rail_release(&m.rails,&t,1000000)==A733_RAIL_READY);
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--decode")){outputs(1);return 0;}
    assert(argc==1);snapshots();
    puts("PASS: DC1SW2 and CLDO5 snapshots use exact registers under one real rail lease");
}
