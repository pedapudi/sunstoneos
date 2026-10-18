#include "application.h"
static int fault(void)
{
    /* The designated task alone advances native retention. No later call
     * releases a clock, rail, PCK lease, mutex, or inherited memory owner. */
    (void)a733_hdmi_fail(&app.hdmi.controller.io);
    atomic_store_explicit(&app.state,HDMI_APP_FAULT_RETAINED,memory_order_release);
    return -1;
}
int hdmi_output_step(void)
{
    if(xTaskGetCurrentTaskHandle()!=app.handles[1])return -1;
    if(atomic_load_explicit(&app.state,memory_order_acquire)==HDMI_APP_FAULT_RETAINED)
        return fault(); /* A BUSY retention retry is metadata-only. */
    if(!hdmi_live(NULL))return fault();
    uint64_t now;
    if(a733_counter_clock_now(&app.counter,&now)!=1||now<app.last_now||now>=app.deadline)return fault();
    app.last_now=now;
    if(atomic_load_explicit(&app.state,memory_order_acquire)==HDMI_APP_STOPPED_RETAINED)return 1;
    struct a733_hdmi *h=&app.hdmi.controller;
    if(now>=app.stop_at||atomic_load_explicit(&app.cancel,memory_order_acquire)){
        if(h->state==A733_HDMI_ACTIVE||h->state==A733_HDMI_PREPARED||h->state==A733_HDMI_STARTING){
            if(a733_hdmi_stop(h,app.deadline)!=1)return fault();
            app.stage=4;
        }else if(h->state!=A733_HDMI_STOPPING&&h->state!=A733_HDMI_STOPPED_RETAINED)return fault();
    }
    if(!atomic_load_explicit(&app.pmic_ready,memory_order_acquire))return 0;
    int r;
    switch(app.stage){
    case 0:
        if(a733_hdmi_prepare(h,app.deadline)!=1)return fault();app.stage=1;return 0;
    case 1:
        r=a733_hdmi_poll(h);if(r<0)return fault();if(!r)return 0;
        if(h->state!=A733_HDMI_PREPARED)return fault();
        if(a733_hdmi_start(h,app.deadline)!=1)return fault();app.stage=2;return 0;
    case 2:
        r=a733_hdmi_poll(h);if(r<0)return fault();if(!r)return 0;
        if(h->state!=A733_HDMI_ACTIVE)return fault();
        atomic_store_explicit(&app.state,HDMI_APP_ACTIVE,memory_order_release);app.stage=3;return 1;
    case 3:
        r=a733_hdmi_service(h,app.deadline);if(r<0)return fault();
        if(h->state==A733_HDMI_STOPPING)app.stage=4;
        else if(h->state!=A733_HDMI_ACTIVE)return fault();return r;
    case 4:
        r=a733_hdmi_poll(h);if(r<0)return fault();if(!r)return 0;
        if(h->state!=A733_HDMI_STOPPED_RETAINED)return fault();
        atomic_store_explicit(&app.state,HDMI_APP_STOPPED_RETAINED,memory_order_release);return 1;
    default:return fault();
    }
}
void hdmi_output_task(void *unused)
{(void)unused;for(;;){(void)hdmi_output_step();vTaskDelay(1);}}
