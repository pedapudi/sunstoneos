/* SPDX-License-Identifier: Apache-2.0 */
#define PMIC_MODEL_NO_MAIN
#include "a733-pmic.c"

static void decodes(void)
{
    const enum a733_rail rails[]={A733_RAIL_DCDC8,A733_RAIL_DCDC9};
    for(unsigned r=0;r<2;++r)for(unsigned raw=0;raw<256;++raw)
        for(unsigned enabled=0;enabled<2;++enabled) {
            const unsigned bit=r?0:7,enable=enabled?(1u<<bit):(255u^(1u<<bit));
            const unsigned code=raw&127;
            struct a733_pmic_snapshot s;
            assert(a733_pmic_decode(rails[r],enable,raw,&s));
            assert(s.rail==rails[r] && s.enable==enable && s.selector==raw);
            assert(s.enabled==enabled && s.decoded==(code<=87));
            assert(s.programmed_uv==(code>87?0:code<=70?500000+10000*code:1200000+20000*(code-70)));
            printf("dcdc%u %u %u %u %u\n",r+8,raw,s.enabled,s.decoded,s.programmed_uv);
        }
}

static struct pmic_model m;
static void snapshots(void)
{
    pmic_model_setup(&m);
    m.pmic[0x11]=1;m.pmic[0x19]=0xe4;m.pmic[0x1a]=0xe4;
    m.pmic[0x2e]=m.pmic[0x2f]=m.pmic[0x31]=13;
    for(unsigned i=0;i<16 && a733_pmic_live(&m.provider,&m.lifetime)!=A733_PMIC_READY;++i)
        assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
    assert(a733_pmic_live(&m.provider,&m.lifetime)==A733_PMIC_READY);
    const enum a733_rail rails[]={A733_RAIL_DCDC1,A733_RAIL_DCDC8,A733_RAIL_DCDC9,
        A733_RAIL_BLDO5,A733_RAIL_CLDO1,A733_RAIL_CLDO3};
    const unsigned enables[]={0x10,0x10,0x11,0x21,0x21,0x21};
    const unsigned selectors[]={0x12,0x19,0x1a,0x2e,0x2f,0x31};
    const unsigned uv[]={3300000,0,0,1800000,1800000,1800000};
    a733_rail_mask_t mask=0;
    for(unsigned i=0;i<6;++i)mask|=A733_RAIL_BIT(rails[i]);
    struct a733_rail_token t={0};
    assert(a733_rail_acquire(&m.rails,10,mask,0,1000000,&t)==A733_RAIL_PENDING);
    assert(a733_rail_poll(&m.rails,&t)==A733_RAIL_READY);
    for(unsigned r=0;r<6;++r) {
        struct a733_pmic_request q={0};struct a733_pmic_snapshot s={0};
        m.trace_count=0;
        assert(a733_pmic_submit(&m.provider,10,&t,rails[r],1000000,&q)==A733_PMIC_PENDING && q.id);
        assert(a733_rail_release(&m.rails,&t,1000000)==A733_RAIL_BUSY);
        for(unsigned i=0;i<150 && a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_PENDING;++i)
            assert(pmic_model_step(&m)!=A733_PMIC_RETAINED);
        assert(a733_pmic_result(&m.provider,&q,&s)==A733_PMIC_COMPLETE);
        assert(s.rail==rails[r] && s.enabled && s.decoded==!!uv[r] && s.programmed_uv==uv[r]);
        assert(s.enable==m.pmic[enables[r]] && s.selector==m.pmic[selectors[r]]);
        assert(!m.i2c_held && !s.page_before && s.chip==3 && !s.revision && !s.page_after);
        const unsigned registers[]={0xff,0x0e,0x0f,enables[r],selectors[r],0xff};
        unsigned data=0;
        for(unsigned i=0;i<m.trace_count;++i)if(m.trace[i].address==0x07083008) {
            assert(data<18);
            const unsigned expected=data%3==0?0x6c:data%3==1?registers[data/3]:0x6d;
            assert(m.trace[i].value==expected);++data;
        }
        assert(data==18); /* Address/index/address only: no regulator data write. */
        assert(a733_pmic_reap(&m.provider,&q)==A733_PMIC_COMPLETE);
    }
    assert(a733_rail_release(&m.rails,&t,1000000)==A733_RAIL_READY);
    assert(!m.i2c_held && !m.retained);
}

int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"--decode")){decodes();return 0;}
    assert(argc==1);snapshots();
    puts("PASS complete raw supply snapshots, disputed-voltage refusal and read-only request cleanup");
}
