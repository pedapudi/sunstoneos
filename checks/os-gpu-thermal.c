/* Native OS GPU thermal ownership with modeled service replies and scheduler. */
#include "../apps/os/gpu.c"

struct board_application board;
struct board_provider providers;
volatile unsigned os_check_result;
static unsigned mode, throttled, sends, closes, opens, pending, polls;
static uint64_t time_us=100, model_epoch=1;
static TickType_t extra_ticks;
static unsigned model_phase=PVR_GPU_SERVICE_READY;
static void check(int ok,unsigned code)
{if(!ok){os_check_result=code;__asm__ volatile("brk #0");for(;;){}}}
uint64_t a733_smp_counter(void){return time_us*24;}
TickType_t xTaskGetTickCount(void){return (TickType_t)(time_us/1000)+extra_ticks;}
int board_live(void *unused)
{(void)unused;return !atomic_load(&board.fatal)&&time_us<board.session.deadline_us&&time_us<atomic_load(&board.live_until);}
TaskHandle_t xTaskGetCurrentTaskHandle(void)
{return (TaskHandle_t)(uintptr_t)(mode==4||mode==11?2:1);}
int os_thermal_throttled(void){return throttled;}
void vTaskDelay(TickType_t ticks)
{check(ticks==1,2);if(mode==14)extra_ticks+=ticks;else time_us+=1000;}
int pvr_gpu_service_send_epoch(struct pvr_gpu_service *s,uint64_t epoch,
    const struct pvr_gpu_service_request *request,TickType_t wait,uint64_t *ticket)
{
    if(mode==12&&pending)return 0;
    check(s==&gpu.service&&epoch==model_epoch&&wait==OS_GPU_WAIT_TICKS&&!pending,3);
    check(board_live(NULL)&&request->timeout_us<=OS_GPU_OPEN_US,4);
    ++sends;pending=request->operation+1;*ticket=sends;
    if(request->operation==PVR_GPU_SERVICE_CLOSE){
        check(model_phase==PVR_GPU_SERVICE_READY&&request->timeout_us,5);
        if(mode>=7)check(gpu.retired&&atomic_load(&gpu.waiting),26);
        ++closes;model_phase=PVR_GPU_SERVICE_STOPPING;polls=0;
    }else if(request->operation==PVR_GPU_SERVICE_OPEN){
        check(model_phase==PVR_GPU_SERVICE_STOPPED&&request->timeout_us,6);
        ++opens;++model_epoch;model_phase=PVR_GPU_SERVICE_READY;
        if(mode==1)throttled=1;
    }else{
        check(request->operation==PVR_GPU_SERVICE_STATUS&&!request->timeout_us,7);
        if(++polls==2&&mode!=14)model_phase=PVR_GPU_SERVICE_STOPPED;
    }
    return 1;
}
int pvr_gpu_service_collect(struct pvr_gpu_service *s,uint64_t ticket,
    struct pvr_gpu_service_reply *reply,TickType_t wait)
{
    check(s==&gpu.service&&ticket==sends&&wait==OS_GPU_WAIT_TICKS&&pending,8);
    if(mode==3||mode==9){time_us+=200000;return 0;}
    *reply=(struct pvr_gpu_service_reply){.ticket=ticket,.epoch=model_epoch,
        .result=PVR_GPU_SERVICE_OK,.phase=model_phase};
    if(mode==8){reply->result=PVR_GPU_SERVICE_BUSY;model_phase=PVR_GPU_SERVICE_READY;}
    if(mode==2||mode==10)atomic_store(&board.fatal,1);
    if(mode==6&&model_phase==PVR_GPU_SERVICE_READY)++reply->epoch;
    if(mode==13&&model_phase==PVR_GPU_SERVICE_STOPPED)++reply->epoch;
    pending=0;return 1;
}
void os_gpu_thermal_check(unsigned selected)
{
    mode=selected;os_gpu_client_task=(TaskHandle_t)(uintptr_t)1;
    gpu.started=gpu.opened=1;gpu.epoch=1;gpu.component=&gpu.session.component;
    board.session.deadline_us=60000000;atomic_store(&board.live_until,3000000);
    if(mode==0){
        for(unsigned i=0;i<2;++i){
            throttled=1;check(os_gpu_queue_thermal()==0&&gpu.parked&&!gpu.opened&&!atomic_load(&gpu.waiting),11);
            throttled=0;check(os_gpu_queue_thermal()==1&&!gpu.parked&&gpu.opened&&!atomic_load(&gpu.waiting),12);
        }
        check(opens==2&&closes==2&&gpu.epoch==3,13);
    }else if(mode==1||mode==2||mode==6){
        gpu.opened=0;gpu.parked=1;model_phase=PVR_GPU_SERVICE_STOPPED;
        int result=os_gpu_queue_thermal();check(opens==1,14);
        if(mode==1)check(result==0&&closes==1&&gpu.parked&&!atomic_load(&gpu.waiting),15);
        else{
            check(result==-1&&!closes&&gpu.failed&&atomic_load(&gpu.waiting),16);
            unsigned before=sends;atomic_store(&board.fatal,0);throttled=0;
            check(os_gpu_queue_thermal()==-1&&sends==before,17);
        }
    }else if(mode==3){
        throttled=1;check(os_gpu_queue_thermal()==-1&&sends==1&&closes==1&&gpu.failed,19);
        check(time_us==1000100&&atomic_load(&gpu.waiting),20);
    }else if(mode==4){check(os_gpu_queue_thermal()==-1&&!sends,21);}
    else if(mode==5){
        atomic_store(&board.live_until,time_us);check(os_gpu_queue_thermal()==-1&&!sends,22);
        atomic_store(&board.live_until,3000000);check(os_gpu_queue_thermal()==-1&&!sends&&gpu.failed,25);
    }else if(mode>=7&&mode<=14){
        if(mode==12)pending=PVR_GPU_SERVICE_STATUS+1;
        int retired=os_gpu_queue_retire();unsigned before=sends;
        if(mode==11)check(!retired&&!gpu.retired&&!gpu.failed&&!sends&&!atomic_load(&gpu.waiting),27);
        else if(mode==7){
            check(retired&&gpu.retired&&gpu.parked&&!gpu.opened&&!gpu.failed&&!atomic_load(&gpu.waiting),28);
            check(closes==1&&!opens&&!pending&&model_phase==PVR_GPU_SERVICE_STOPPED,29);
            throttled=1;check(os_gpu_queue_thermal()==0,30);
            throttled=0;check(os_gpu_queue_thermal()==0&&os_gpu_queue_retire()==1&&sends==before,31);
        }else{
            check(!retired&&gpu.retired&&gpu.failed&&atomic_load(&gpu.waiting)&&!gpu.parked&&gpu.opened,33);
            check(closes==(mode==12?0u:1u)&&!opens,34);
            if(mode==9)check(time_us==1000100&&pending,35);
            if(mode==12)check(pending==PVR_GPU_SERVICE_STATUS+1&&!sends,36);
            if(mode==14)check(extra_ticks==1000&&closes==1&&polls==999,37);
            atomic_store(&board.fatal,0);atomic_store(&board.live_until,3000000);
            check(!os_gpu_queue_retire()&&os_gpu_queue_thermal()==-1&&sends==before,38);
        }
    }else if(mode==15){
        gpu.component->owed=1;throttled=1;
        check(os_gpu_queue_thermal()==-1&&gpu.failed&&!sends&&gpu.component->owed,40);
    }else check(0,23);
    os_check_result=1;__asm__ volatile("brk #0");for(;;){}
}
