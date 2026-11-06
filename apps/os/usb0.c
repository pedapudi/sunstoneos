/* The OS owns USB0. Application requests cross a task-notified mailbox; no
 * application code runs in the privileged USB owner. */
#include "os.h"
#include "application.h"
#include "../board/application.h"
#include "../board/session.h"
#include "boot.h"
#include "platform.h"
#include "tusb.h"
#include "device/dcd.h"
#include <string.h>
static _Noreturn void stop(void){board_fail(NULL);for(;;)vTaskSuspend(NULL);}
/* The product ID names the admitted lifetime to the host: 0xefea resident, 0xefe8 supervised. */
#define USB0_PID 0xefe8
#define USB0_PID_SELECT() (board_session_lifetime()==A733_LIFETIME_RESIDENT?0xefea:0xefe8)
#define USB0_PRODUCT "sunstoneos native OS"
#define USB0_SERIAL "sunstoneos-a733-board"
#define USB0_LIVE() board_live(NULL)
#define USB0_PROVIDERS (&providers.shared)
#include "../board/usb0_service.h"
#ifdef SUNSTONE_A733_FAULT_PAGE
#include "a733_fault.h"
#endif
#include "thermal-report.h"
#include "perf.h"
static _Alignas(8) uint8_t frame[1160];
static struct ss_host_request request;
static unsigned used,frame_bytes,sent,transferred,waiting,app_frame,app_success;
void tud_mount_cb(void){}
void tud_umount_cb(void){}
void tud_suspend_cb(bool wake){(void)wake;}
void tud_vendor_rx_cb(uint8_t interface,const uint8_t *buffer,uint32_t bytes)
{(void)buffer;(void)bytes;if(interface)stop();}
void tud_vendor_tx_complete_cb(uint8_t interface){if(interface)stop();}
void tud_vendor_tx_cb(uint8_t interface,uint32_t bytes)
{
    if(interface||!board_live(NULL)||bytes>sent-transferred||(app_success&&os_application_export_live()<0))stop();
    transferred+=bytes;
    if(transferred==frame_bytes&&sent==frame_bytes){if(app_frame)os_host_delivered();frame_bytes=0;app_frame=0;}
}
static void queue(unsigned bytes){frame_bytes=bytes;sent=transferred=0;}
static int os_request(void)
{
    if(request.op==SS_HOST_RESULT_OP){
        if(request.flags||!request.count||request.count>SS_HOST_PAYLOAD_BYTES||request.argument)stop();
        memset(frame,0,sizeof(frame));struct ss_host_frame *h=(void *)frame;
        size_t total=sizeof(struct ss_result)+ss_result_capacity;
        size_t n=0;unsigned ready=os_application_finished();
        if(ready&&request.offset<total){n=total-request.offset;if(n>request.count)n=request.count;
            memcpy(frame+64,ss_result_record+request.offset,n);}
        *h=(struct ss_host_frame){.magic=SS_FRAME_MAGIC,.version=1,.state=ready,.bytes=n,
            .total=total,.offset=request.offset,.op=request.op,.epoch=board.session.epoch,.generation=board.session.generation};
        queue(64+n);return 1;
    }
#ifdef SUNSTONE_A733_FAULT_PAGE
    if(request.op==5){if(request.flags||request.offset||request.count||request.argument)stop();
        memcpy(frame,&a733_fault_previous,sizeof(a733_fault_previous));queue(sizeof(a733_fault_previous));return 1;}
#endif
    /* Operation 6 exists for a session that grants the NPU. */
    if(request.op==6&&os_granted(OS_GRANT_NPU)){if(request.flags||request.offset||request.count||request.argument)stop();
        os_npu_report_copy(frame);queue(OS_NPU_REPORT_BYTES);return 1;}
    /* Operations 7 and 8 exist for a session that grants the CPU clock, and the second for one that grants perf. */
    if(request.op==7&&os_granted(OS_GRANT_CPU_CLOCK)){if(request.flags||!request.count||request.count>1024)stop();
        memset(frame,0,sizeof(frame));size_t copied=0;uint32_t total=0;
        int r=os_thermal_report_copy(request.argument,request.offset,request.count,frame+64,&copied,&total);
        if(r!=1)copied=0;
        struct ss_host_frame *h=(void *)frame;
        h->magic=SS_FRAME_MAGIC;h->version=1;h->state=r<0?2:r?1:0;h->bytes=copied;h->total=total;
        h->offset=request.offset;h->op=request.op;h->epoch=board.session.epoch;h->generation=board.session.generation;
        queue(64+copied);return 1;}
    if(request.op==8&&os_perf_enabled()){unsigned command=request.flags;
        if(request.count||command>OS_PERF_SWITCH||(command!=OS_PERF_SWITCH&&(request.offset||request.argument))||
           (request.offset&~OS_PERF_SELECTION_MASK))stop();
        os_perf_usb(command,request.offset,request.argument,frame);queue(OS_PERF_REPORT_BYTES);return 1;}
    return 0;
}
static void exchange(void)
{
    BOARD_PROGRESS(BOARD_SERVICE_USB0);
    if(frame_bytes){if(!board_live(NULL))stop();
        if(app_success){int live=os_application_export_live();if(live<0)stop();if(!live)return;}
        sent+=tud_vendor_write(frame+sent,frame_bytes-sent);tud_vendor_flush();return;}
    if(waiting){
        struct ss_host_reply reply;
        if(!os_host_reply(&reply,frame+64))return;
        if(reply.state==1){int live=os_application_export_live();if(live<0)stop();if(!live)return;}
        struct ss_host_frame *h=(void *)frame;
        *h=(struct ss_host_frame){.magic=SS_FRAME_MAGIC,.version=reply.version,.state=reply.state,.bytes=reply.bytes,
            .total=reply.total,.offset=request.offset,.storage=atomic_load_explicit(&board.result,memory_order_acquire),
            .op=request.op,.epoch=board.session.epoch,.generation=board.session.generation};
        memcpy(h->tag,reply.tag,16);app_frame=1;app_success=reply.state==1;waiting=0;queue(64+reply.bytes);return;
    }
    unsigned available=tud_vendor_available();if(!available)return;
    unsigned n=sizeof(request)-used;if(n>available)n=available;
    unsigned got=tud_vendor_read((uint8_t *)&request+used,n);if(!got||got>n)stop();used+=got;
    if(used!=sizeof(request))return;used=0;
    app_success=0;if(os_request())return;
    if(!os_host_offer(&request))stop();waiting=1;
}
void board_usb0_task(void *unused)
{
    (void)unused;
#ifdef SUNSTONE_A733_OS_CPU_RETURN
    /* This return profile admits no native USB ownership. */
    os_application_usb_ready();for(;;)vTaskSuspend(NULL);
#else
    usb0_service_start();os_application_usb_ready();usb0_service_run(exchange);
#endif
}
