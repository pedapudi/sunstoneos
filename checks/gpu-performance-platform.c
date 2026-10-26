/* SPDX-License-Identifier: Apache-2.0 */
/* Actual GPU platform/PMIC/rail/domain providers over the existing register
 * model. Firmware quiescence is supplied by the separate clock protocol case. */
#define main fixed_rate_regression_main
#include "a733-gpu-second-on.c"
#undef main
static unsigned rail_writes, mux_writes, bad_parent, stuck_mux;
static uint64_t written_at;
static int perf_read(void *v,uintptr_t a,uint32_t *out)
{
    switch(a) {
    case 0x02002000:*out=0xf8000000u|(99u<<16)|(99u<<8);return 1;
    case 0x020020a0:*out=0xfe000000u|(1u<<20)|(2u<<16)|(99u<<8)|(4u<<2);return 1;
    case 0x020020a8:case 0x020020ac:*out=0;return 1;
    case 0x02003908:case 0x02003988:*out=bad_parent ? 0 : 0x0a000a00;return 1;
    default:return res_read(v,a,out);
    }
}
static void rail_written(void *v,uint8_t reg,uint8_t value)
{
    (void)v;if(reg!=0x15)return;
    ++rail_writes;written_at=m.now;
    assert((value&127)==30 || (value&127)==46);
    if((value&127)==30)assert((d.ccu_b20&0x8700000f)==0x82000000);
}
static int perf_update(void *v,enum a733_gpu_power_bank b,uint32_t off,uint32_t c,uint32_t s,uint64_t end)
{
    if(b==A733_GPU_CCU && off==0xb20 && (d.ccu_b20&0x80000000) && c==0x0700000f) {
        ++mux_writes;
        if(!(s&0x07000000))assert((m.pmic[0x15]&127)==46 && m.now>written_at+1200);
    }
    int r=gpu_update(v,b,off,c,s,end);
    if(stuck_mux && b==A733_GPU_CCU && off==0xb20)d.ccu_b20|=0x08000000;
    return r;
}
static void setup(unsigned low,unsigned verifier_first)
{
    domains_model_setup(0x28,23);d.ack_us=0;d.top5_gates=1;
    d.ppu[5][0]|=8;d.ppu[5][2]|=8;d.ccu_5c0=0x110103ff;d.ccu_5e0=0xf1055803;d.ccu_b20=d.ccu_b24=0;
    memset(core,0,sizeof(core));core[0]=8;core[8]=A733_GPU_CORE_RESET_PWCR;core_pending=1;core_written=0;core_misr=0;
    rail_writes=mux_writes=bad_parent=stuck_mux=0;written_at=0;
    m.pmic[0x15]=0x80|30;m.pmic_written=rail_written;
    m.gpu.ops.read32=perf_read;m.gpu.ops.update32=res_update;m.gpu.ops.rail=res_rail;
    memset(&gpu,0,sizeof(gpu));memset(&native,0,sizeof(native));native.initialized=1;
    static const struct a733_gpu_platform_ops ops={gpu_now,gpu_read,perf_update};
    assert(a733_gpu_platform_init(&gpu,&m.gpu,3,&ops,0));
    assert(a733_gpu_platform_bind_pmic(&gpu,&native,&m.provider,&m.lifetime,9));
    assert(a733_gpu_platform_performance_enable(&gpu,low?A733_GPU_LOW_HZ:A733_GPU_CORE_HZ));
    if(verifier_first)domains_boot();
    session_deadline=m.now+1000000;
    assert(a733_gpu_platform_begin(&gpu,session_deadline));
    if(!verifier_first) {
        for(unsigned i=0;i<8;++i)assert(a733_gpu_platform_poll(&gpu)==0);
        assert(!gpu.pmic.rail.id && !rail_writes);
    }
    assert(step(a733_gpu_platform_poll,&gpu)==1);
    assert(gpu.pmic.rail.id && gpu.perf.parent.id);
    assert(gpu.perf.current_hz==(low?A733_GPU_LOW_HZ:A733_GPU_CORE_HZ));
    assert(gpu.perf.current_uv==(low?A733_GPU_LOW_UV:A733_GPU_SUPPLY_UV));
}
static int supply_end(void *p){return a733_gpu_platform_supply_end(p);}
static int performance_poll(void *p){return a733_gpu_platform_performance_poll(p);}
static int change(enum a733_perf_action action,uint32_t value)
{
    assert(a733_gpu_platform_performance_begin(&gpu,action,value,m.now+50000));
    return step(performance_poll,&gpu);
}
int main(void)
{
    unsigned cases=0;
    for(unsigned first=0;first<2;++first) {
        setup(0,first);const uint64_t rail=gpu.pmic.rail.id,parent=gpu.perf.parent.id;
        assert(change(A733_PERF_RATE,600)==1 && change(A733_PERF_VOLTAGE,800)==1);
        assert(m.now>written_at+1200 && gpu.perf.current_uv==800000);
        assert(change(A733_PERF_READ,0)==1);
        assert(change(A733_PERF_VOLTAGE,960)==1 && change(A733_PERF_RATE,1008)==1);
        assert(gpu.pmic.rail.id==rail && gpu.perf.parent.id==parent && rail_writes==3 && mux_writes==2);
        ++cases;
    }
    setup(1,0);const uint64_t rail=gpu.pmic.rail.id;
    assert(a733_gpu_platform_off_begin(&gpu,m.now+1000000) && step(off_poll,&gpu)==1);
    assert(gpu.pmic.rail.id==rail && !(d.ccu_b20&0x8700000f));
    assert(step(supply_end,&gpu)==1 && !gpu.pmic.request.id && !gpu.pmic_observed);
    assert(a733_gpu_platform_begin(&gpu,m.now+1000000) && step(a733_gpu_platform_poll,&gpu)==1);
    assert(gpu.perf.current_hz==600000000 && gpu.perf.current_uv==800000 && gpu.pmic.rail.id==rail);++cases;
    setup(0,1);assert(change(A733_PERF_VOLTAGE,800)==-1 && rail_writes==1 && gpu.pmic.rail.id && gpu.token);++cases;
    setup(1,1);assert(change(A733_PERF_RATE,1008)==-1 && gpu.pmic.rail.id && gpu.token);++cases;
    setup(0,1);bad_parent=1;assert(change(A733_PERF_RATE,600)==-1 && !mux_writes && gpu.pmic.rail.id);++cases;
    setup(0,1);stuck_mux=1;assert(change(A733_PERF_RATE,600)==-1 && mux_writes==1 && gpu.pmic.rail.id && gpu.token);++cases;
    setup(0,1);assert(change(A733_PERF_RATE,600)==1);m.fail_phase=2;m.fail_status=0x38;
    assert(change(A733_PERF_VOLTAGE,800)==-1 && gpu.pmic.rail.id && gpu.token && gpu.supply_request.id);++cases;
    setup(0,1);assert(change(A733_PERF_RATE,600)==1);m.pmic[0x15]=0x80|45;
    assert(change(A733_PERF_VOLTAGE,800)==-1 && rail_writes==1 && gpu.pmic.rail.id && gpu.supply_request.id);++cases;
    setup(0,1);assert(change(A733_PERF_RATE,600)==1);
    assert(a733_gpu_platform_performance_begin(&gpu,A733_PERF_VOLTAGE,800,m.now+50000));
    for(unsigned i=0;!gpu.perf.settled;++i) {
        assert(i<100 && a733_gpu_platform_performance_poll(&gpu)==0);
        service_step();m.now+=TICK_US;
    }
    assert((m.pmic[0x15]&127)==30 && gpu.perf.current_uv==960000);
    m.now=gpu.perf.deadline;
    assert(a733_gpu_platform_performance_poll(&gpu)==-1 && gpu.pmic.rail.id && gpu.token);++cases;
    printf("PASS GPU performance platform: %u cases; actual PMIC low admission, both verifier orders, high/low order, settled readback, low restart, retained faults\n",cases);
    return 0;
}
