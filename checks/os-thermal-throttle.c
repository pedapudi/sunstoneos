/* Actual OS policy publication, dispatch and patched recognition checkpoints.
 * Sensor reads, scheduler sleeps, board admission and GPU OPEN are modeled. */
#define a733_thermal_poll modeled_thermal_poll
#define a733_thermal_read modeled_thermal_read
#include "../apps/os/thermal.c"
#undef a733_thermal_poll
#undef a733_thermal_read
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
#include "../apps/os/dispatch.c"
#endif

struct board_application board;
struct board_provider providers;
volatile unsigned os_check_result;
static unsigned mode,up=1,progress,sleeps,fails,mask_calls,fail_on_sleep;
static unsigned current=1;
static uint64_t time_us=100;
static struct a733_thermal_sample next_sample;
static enum a733_thermal_result next_result=A733_THERMAL_AGAIN;
void mimus_native_checkpoint(void);
static void check(int ok,unsigned code)
{if(!ok){os_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
static void done(void){os_check_result=1;__asm__ volatile("brk #0");for(;;){}}
int board_live(void *v){(void)v;return up;}
void board_fail(void *v){(void)v;up=0;++fails;}
int mimus_board_live(void){return up;}
void mimus_board_fail(void){up=0;++fails;}
void mimus_board_progress(void){check(up,2);++progress;}
uint64_t a733_smp_counter(void){return time_us*24;}
int a733_counter_clock_now(void *v,uint64_t *out){(void)v;*out=time_us;return 1;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (TaskHandle_t)(uintptr_t)current;}
void vTaskEnterCritical(void){}
void vTaskExitCritical(void){}
int gicv3_spi_mask(const struct gicv3_spi *irq){(void)irq;++mask_calls;return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *irq){(void)irq;return 1;}
enum a733_thermal_result modeled_thermal_poll(struct a733_thermal *d)
{(void)d;return A733_THERMAL_READY;}
enum a733_thermal_result modeled_thermal_read(struct a733_thermal *d,uint64_t until,
                                             struct a733_thermal_sample *sample)
{(void)d;check(until>time_us,3);*sample=next_sample;return next_result;}
void vTaskDelay(TickType_t ticks)
{
    check(ticks==1,4);
    /* A sleep must not create progress before it completes. */
    ++sleeps;time_us+=1000;
    if(fail_on_sleep)up=0;
}
void vTaskSuspend(TaskHandle_t task)
{
    check(!task&&!up&&fails==1&&!progress,5);
    if(mode==3)check(!sleeps,6);
    else if(mode==4)check(sleeps==1,7);
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
    else if(mode==6){
        check(os_dispatch_gpu_state()==OS_DISPATCH_GPU_FAULT,8);
        policy(&thermal,A733_THERMAL_NORMAL);
        check(!os_dispatch_place(0).n_gpu&&!os_dispatch_active,9);
    }
#endif
    else check(0,10);
    done();
}
static void start(void)
{
    board.session.deadline_us=60000000;
    thermal.handle=(TaskHandle_t)(uintptr_t)1;
    check(os_thermal_throttled(),11);
    check(step()==1&&!os_thermal_throttled()&&thermal.active,12);
}
static void sample(int32_t gpu,int32_t npu)
{
    current=1;time_us+=25000;next_result=A733_THERMAL_SAMPLE;
    next_sample.valid=31;next_sample.observed_us=time_us;++next_sample.sequence;
    for(unsigned i=0;i<5;++i)next_sample.millicelsius[i]=50000;
    next_sample.millicelsius[4]=gpu;next_sample.millicelsius[2]=npu;
}
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
int a733_gpu_mimus_open(void){return 1;}
#endif
void os_throttle_check(unsigned selected)
{
    mode=selected;
    if(mode==2){mimus_native_checkpoint();check(sleeps==1&&progress==1,13);done();}
    if(mode==3){up=0;mimus_native_checkpoint();check(0,14);}
    if(mode==4){fail_on_sleep=1;mimus_native_checkpoint();check(0,15);}
    start();
    if(mode==0){
        for(unsigned cycle=0;cycle<2;++cycle){
            sample(90000,50000);check(step()==1&&os_thermal_throttled(),16);
            sample(89000,50000);check(step()==1&&!os_thermal_throttled(),17);
        }
        sample(50000,110000);check(step()==-1&&os_thermal_throttled()&&!up&&fails,18);
        check(!sleeps&&!progress&&thermal.retaining&&thermal.masked,19);done();
    }
    if(mode==1){
        for(unsigned task=1;task<=2;++task){current=task;mimus_native_checkpoint();}
        check(!sleeps&&progress==2,20);
        sample(90000,50000);check(step()==1,21);
        for(unsigned task=1;task<=2;++task){current=task;mimus_native_checkpoint();}
        check(sleeps==2&&progress==4,22);
        sample(89000,50000);check(step()==1,23);
        for(unsigned task=1;task<=2;++task){current=task;mimus_native_checkpoint();}
        check(sleeps==2&&progress==6,24);done();
    }
#ifdef SUNSTONE_A733_OS_GPU_LINEAR
    if(mode==5||mode==6){
        check(os_dispatch_open()&&os_dispatch_place(0).n_gpu==16,25);
        if(mode==6){os_dispatch_failed(OS_DISPATCH_LOST);check(0,26);}
        os_dispatch_waiting(1);
        sample(90000,50000);check(step()==1&&!os_dispatch_place(0).n_gpu,27);
        check(os_dispatch_gpu_state()==OS_DISPATCH_GPU_WAITING,28);
        os_dispatch_waiting(0);
        sample(89000,50000);check(step()==1&&os_dispatch_place(0).n_gpu==16,29);
        os_dispatch_incomplete();
        sample(90000,50000);check(step()==1&&!os_dispatch_place(0).n_gpu,30);
        sample(89000,50000);check(step()==1&&!os_dispatch_place(0).n_gpu,31);
        check(os_dispatch_record.incomplete&&!os_dispatch_active,32);done();
    }
#endif
    if(mode==7){
        time_us+=1000001;check(step()==1&&os_thermal_throttled(),33);
        time_us+=4000000;check(step()==-1&&!up&&os_thermal_throttled()&&fails,34);done();
    }
    check(0,35);
}
