#include "application.h"
#ifdef SUNSTONE_BOARD_USB2_IRQ
#include "host_irq.h"
#endif
#include <string.h>
static void put32(uint8_t *p,uint32_t n)
{for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n)
{put32(p,(uint32_t)n);put32(p+4,(uint32_t)(n>>32));}
static void hid_append(uint8_t batch[512],const struct a733_usb2_hid_report *report,unsigned reports)
{
    if(!reports){
        put32(batch,UINT32_C(0x52444948));put32(batch+4,1);
        put32(batch+12,report->address);put64(batch+16,report->generation);
        batch[24]=report->speed;batch[25]=report->interface;
        batch[26]=report->protocol;batch[27]=report->endpoint;
    }
    uint8_t *entry=batch+32+reports*80;
    put64(entry,report->sequence);put32(entry+8,report->bytes);
    memcpy(entry+16,report->data,report->bytes);put32(batch+8,reports+1);
}
/* Final attachment check before publication. A busy provider observation is
 * repeated for at most 2 ms, as inside the host drivers, and never past the
 * lifetime deadline; refusal or a longer BUSY ends the session. */
static int attached(struct a733_usb2_native *n)
{
    uint64_t start,now;int r;
    if(n->core.ops.now(n->core.context,&start)!=1)return 0;
    while(!(r=n->core.ops.live(n->core.context,n->core.generation,n->platform.config.lifetime_deadline)))
        if(n->core.ops.now(n->core.context,&now)!=1||now<start||now-start>=2000)return 0;
    return r==1;
}
#ifndef SUNSTONE_BOARD_USB2_IRQ
/* Owner-visible progress of one poll: platform startup, core state, consumed
 * controller events, mount state and this task's request state. */
struct host_progress { unsigned platform,supply,core,event,cycle,mounted,issued,reports; };
static struct host_progress host_progress(unsigned issued,unsigned reports)
{
    const struct a733_usb2_native *n=&board.host;
    return (struct host_progress){n->platform.phase,n->platform.supply_index,n->core.phase,
        n->core.event_index,n->core.event_cycle,n->mounted,issued,reports};
}
#endif
static int dual_step(unsigned *issued,unsigned *reports,unsigned *sector_done)
{
    struct a733_usb2_native *n=&board.host;uint8_t *p=board.dual_result;
    if(!*issued){
        if(a733_usb2_device_kind(n)!=A733_USB2_DEVICE_DUAL)return 1;
        memset(p,0,sizeof(board.dual_result));
        if(!a733_usb2_read_sector(n,0,n->platform.config.lifetime_deadline)||
           !a733_usb2_hid_receive(n,n->platform.config.lifetime_deadline))return 0;
        *issued=1;
    }
    if(*issued==2)return 1;
    if(!*sector_done){
        int done=a733_usb2_read_result(n,p+64);if(done<0)return 0;
        if(done==1)*sector_done=1;
    }
    if(*reports<4){
        struct a733_usb2_hid_report report;int done=a733_usb2_hid_result(n,&report);
        if(done<0)return 0;
        if(done==1){
            hid_append(p+576,&report,*reports);++*reports;
            if(*reports<4&&!a733_usb2_hid_receive(n,n->platform.config.lifetime_deadline))return 0;
        }
    }
    if(*sector_done&&*reports==4){
        int live=n->core.ops.live(n->core.context,n->core.generation,n->platform.config.lifetime_deadline);
        if(live<0)return 0;if(!live)return 1;
        put32(p,UINT32_C(0x4c415544));put32(p+4,1);put32(p+8,1088);put32(p+12,2);
        put64(p+16,n->core.generation);p[24]=(uint8_t)n->address;
        p[25]=(uint8_t)a733_xhci_device_speed(&n->core,n->address);p[26]=0;p[27]=(uint8_t)n->msc_in;p[28]=(uint8_t)n->msc_out;
        put32(p+32,0);put32(p+36,64);put32(p+40,512);put32(p+44,576);put32(p+48,512);
        atomic_store_explicit(&board.result,1,memory_order_release);*issued=2;
    }
    return 1;
}
void board_host_task(void *unused)
{
    (void)unused;unsigned issued=0,reports=0,sector_done=0;
#ifndef SUNSTONE_BOARD_USB2_IRQ
    unsigned burst=0,idle=0;
#endif
    board.host.dual_leaf=board.session.workload==4;
    for(;;){
        if(!board_live(NULL))break;
#ifdef SUNSTONE_BOARD_USB2_IRQ
        int irq_ready=board_host_irq_before();if(irq_ready<0)break;
        if(!irq_ready){board_host_irq_wait();continue;}
#else
        const struct host_progress before=host_progress(issued,reports);
#endif
        enum a733_usb2_result r=a733_usb2_native_poll(&board.host);
        if(r==A733_USB2_RETAINED||r==A733_USB2_INVALID)break;
#ifdef SUNSTONE_BOARD_USB2_IRQ
        if(board_host_irq_after()<0)break;
#endif
        enum a733_usb2_device_kind kind=a733_usb2_device_kind(&board.host);
        if(r==A733_USB2_READY&&
           ((board.session.workload==8&&(kind!=A733_USB2_DEVICE_HID||board.host.core.speed!=4||board.host.hid.protocol!=1))||
            ((board.session.workload==6||board.session.workload==12)&&kind!=A733_USB2_DEVICE_MSC)||
            (board.session.workload==10&&(kind!=A733_USB2_DEVICE_HID||board.host.core.speed>3||board.host.hid.protocol!=1))))break;
        /* Removing the single device ends its request without output. Start
         * again when a later attachment mounts. The two-leaf workload still
         * ends, because the other leaf's request may remain outstanding. */
        if(board.session.workload!=4&&issued==1&&r!=A733_USB2_READY){issued=0;reports=0;}
        if(board.session.workload==4){
            if(!dual_step(&issued,&reports,&sector_done))break;
        }else if(r==A733_USB2_READY&&!issued){
            if(kind==A733_USB2_DEVICE_MSC){
                if(!a733_usb2_read_sector(&board.host,0,board.host.platform.config.lifetime_deadline))break;
            }else if(kind==A733_USB2_DEVICE_HID){
                memset(board.sector,0,sizeof(board.sector));
                if(!a733_usb2_hid_receive(&board.host,board.host.platform.config.lifetime_deadline))break;
            }else break;
            issued=1;
        }
        if(board.session.workload!=4&&issued==1&&kind==A733_USB2_DEVICE_MSC){
            int done=a733_usb2_read_result(&board.host,board.sector);
            if(done<0)break;
            if(done==1){
                if(board.session.workload==12&&!attached(&board.host))break;
                if(board.session.workload==6){
                    struct a733_usb2_native *n=&board.host;uint8_t *p=board.ss_result;
                    if(n->core.speed!=4||!attached(n))break;
                    memset(p,0,sizeof(board.ss_result));put32(p,UINT32_C(0x43534d53));put32(p+4,1);put32(p+8,544);
                    put64(p+16,n->core.generation);p[24]=(uint8_t)n->address;p[25]=4;p[27]=(uint8_t)n->msc_in;p[28]=(uint8_t)n->msc_out;
                    memcpy(p+32,board.sector,512);
                }
                atomic_store_explicit(&board.result,1,memory_order_release);issued=2;
            }
        }else if(issued==1&&kind==A733_USB2_DEVICE_HID){
            struct a733_usb2_hid_report report;
            int done=a733_usb2_hid_result(&board.host,&report);if(done<0)break;
            if(done==1){
                if(board.session.workload==8&&(report.speed!=4||report.protocol!=1))break;
                if(board.session.workload==10&&(report.speed<1||report.speed>3||report.protocol!=1))break;
                hid_append(board.sector,&report,reports);++reports;
                if(reports==4){
                    struct a733_usb2_native *n=&board.host;
                    if((board.session.workload==8||board.session.workload==10)&&!attached(n))break;
                    atomic_store_explicit(&board.result,1,memory_order_release);issued=2;
                }
                else if(!a733_usb2_hid_receive(&board.host,board.host.platform.config.lifetime_deadline))break;
            }
        }
        /* The published sector or four-report HID envelope is immutable.
         * Continue observing attachment, controller faults and lifetime. */
#ifdef SUNSTONE_BOARD_USB2_IRQ
        board_host_irq_wait();
#else
        /* While setup or a transfer is outstanding, poll again without
         * sleeping, up to the interrupt profile's 32-poll batch, until four
         * consecutive polls make no progress. Those polls give a request
         * issued by a progressing poll time to complete before the task
         * sleeps. Work that stays idle, such as a HID transfer waiting for
         * input or a PMIC supply result, costs one poll per tick. */
        const struct host_progress after=host_progress(issued,reports);
        idle=memcmp(&before,&after,sizeof(after)) ? 0 : idle<4 ? idle+1 : 4;
        if((r==A733_USB2_PENDING||issued==1)&&idle<4&&++burst<32)continue;
        burst=0;vTaskDelay(1);
#endif
    }
#ifdef SUNSTONE_BOARD_USB2_IRQ
    board_host_irq_stop();
#endif
    a733_usb2_native_lost(&board.host);
    a733_typec_revoke(&board.typec.provider);
    atomic_store_explicit(&board.result,2,memory_order_release);
    for(;;)vTaskDelay(1);
}
