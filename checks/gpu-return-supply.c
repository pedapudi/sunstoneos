/* Actual platform, PMIC provider and ownership ledgers; modeled registers. */
#define main prior_main
#include "a733-gpu-second-on.c"
#undef main
int main(void)
{
    for(unsigned which=0;which<7;++which){
        assert(run(0)==1);
        /* A normal thermal park retains the sample across a new OPEN bound. */
        assert(a733_gpu_platform_off_begin(&gpu,m.now+1000000) && step(off_poll,&gpu)==1);
        const uint64_t request=gpu.pmic.request.id,rail=gpu.pmic.rail.id;
        assert(a733_gpu_platform_begin(&gpu,m.now+2000000) && step(a733_gpu_platform_poll,&gpu)==1);
        assert(gpu.pmic.request.id==request && gpu.pmic.rail.id==rail);
        assert(a733_gpu_platform_off_begin(&gpu,m.now+1000000) && step(off_poll,&gpu)==1);
        m.now=session_deadline+100;
        const uint64_t end=m.now+1000000;
        const unsigned reads=m.reads,writes=m.writes;
        if(which==1)gpu.pmic.request.id++;
        if(which==2)gpu.pmic.rail.id++;
        if(which==3)m.provider.failed=1;
        if(which==4)gpu.pmic.armed=1;
        if(which==5)m.now=end;
        if(which==6){
            /* Legacy refresh still rejects a distinct deadline. */
            assert(a733_pmic_consumer_refresh(&gpu.pmic,end)==-1);
        }else{
            int r=a733_gpu_platform_park(&gpu,end);
            assert(r==(which ? -1 : 1));
            if(!which){
                assert(!gpu.pmic.request.id && !gpu.pmic_observed && gpu.pmic.rail.id==rail);
                for(unsigned i=0;i<A733_PMIC_REQUESTS;++i)assert(!m.provider.slots[i].state && !m.provider.slots[i].borrow.id);
            }else assert(gpu.pmic.request.id && gpu.pmic.rail.id);
        }
        assert(m.reads==reads && m.writes==writes);
    }
    puts("PASS GPU final sample retirement: seven actual-provider cases; zero bus operations; thermal reopen preserved");
}
