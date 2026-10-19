/* Reuse the accepted register/memory producer; execute only changed IRQ cases. */
#define main unchanged_controller_cases
#include "npu-controller.c"
#undef main
int main(void)
{
    struct a733_npu_region region={0x51000000,1048576,0x10000000,A733_NPU_PAGE_4K,0};
    assert(a733_npu_mmu_build(tables,sizeof(tables),0x49000000,&region,1,&mmu));
    struct device d;struct a733_npu_controller c;
    fresh(&d,&c);unsigned calls=d.calls;
    assert(a733_npu_poll(&c)==A733_NPU_COLD && d.calls==calls);
    assert(a733_npu_open(&c,&ops,&d,&mmu,&boot,1000));
    d.now+=40;
    assert(a733_npu_advance(&c,0)==A733_NPU_BOOTING && !c.irq_seen && !d.finished);
    assert(d.reg[0x10/4]==1); /* Timer/idle progress must not steal the event. */
    assert(a733_npu_advance(&c,1)==A733_NPU_READY && d.finished==1);
    calls=d.calls;assert(a733_npu_poll(&c)==A733_NPU_READY && d.calls==calls);
    for(unsigned job=0;job<2;++job){
        assert(a733_npu_submit(&c,&task,1000));d.now+=20;
        assert(a733_npu_advance(&c,1)==A733_NPU_RUNNING && c.irq_seen==1);
        assert(d.finished==job+1);d.now+=20;
        assert(a733_npu_advance(&c,0)==A733_NPU_DONE && d.finished==job+2);
        void *token;int canceled;
        assert(a733_npu_reap(&c,&token,&canceled)&&!canceled&&token==task.token);
    }
    assert(a733_npu_close(&c));
    started(&d,&c);assert(a733_npu_submit(&c,&task,100));d.now+=100;
    calls=d.calls;assert(a733_npu_check_timeout(&c)==A733_NPU_FAULT && c.fault==A733_NPU_TIMEOUT);
    assert(d.calls==calls);retained(&d,&c);
    started(&d,&c);assert(a733_npu_submit(&c,&task,1000));
    assert(a733_npu_cancel(&c,200));d.now+=100;
    assert(a733_npu_advance(&c,0)==A733_NPU_CANCEL_WAIT);
    assert(a733_npu_advance(&c,0)==A733_NPU_CANCELED); /* No event IRQ required for cancel. */
    started(&d,&c);assert(a733_npu_submit(&c,&task,1000));d.now+=40;d.inject_late_fault=1;
    assert(a733_npu_advance(&c,1)==A733_NPU_FAULT && c.fault==A733_NPU_IRQ);
    assert(d.finished==1);retained(&d,&c);
    puts("PASS NPU interrupt controller: inactive polling preserved, notified event ownership, two jobs, idle-late error, deadline without MMIO and cancel without IRQ");
    return 0;
}
