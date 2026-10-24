#include "../board/application.h"
#include "application.h"
#include "assets.h"
#include "keyboard.h"
#include <string.h>
#include "host/usbh.h"
#include "class/hid/hid_host.h"
#include "boot.h"
#include "a733_mapping_check.h"
static atomic_uint attached,usb0_started;
int mimus_board_staged(void)
{
#if defined(SUNSTONE_A733_OS) && defined(SUNSTONE_BOARD_SUPERVISED) && !defined(SUNSTONE_A733_OS_AUDIO) && !defined(SUNSTONE_A733_OS_HEADSET) && !defined(SUNSTONE_A733_OS_RADIO)
    return board.session.workload==2;
#else
    return 0;
#endif
}
/* The USB0 owner publishes only after provider and device-stack startup. */
void mimus_board_usb0_started(void)
{atomic_store_explicit(&usb0_started,1,memory_order_release);}
static atomic_uint_fast64_t observed_until;
static struct a733_typec_token attachment;
int mimus_board_session_live(void)
{return board_live(NULL)&&atomic_load_explicit(&board.result,memory_order_acquire)!=2;}
/* EL0-safe: no provider calls, MMIO, locks or privileged register accesses. */
int mimus_board_live(void)
{
    if(!mimus_board_session_live())return 0;
    if(mimus_board_staged())return atomic_load_explicit(&board.result,memory_order_acquire)==3&&
        atomic_load_explicit(&usb0_started,memory_order_acquire);
    return a733_smp_counter()/24<atomic_load_explicit(&observed_until,memory_order_acquire);
}
/* Called only by the privileged USB0 owner. */
int mimus_board_export_live(void)
{
    if(!board_live(NULL)||atomic_load_explicit(&board.result,memory_order_acquire)==2)return -1;
    if(mimus_board_staged())return mimus_board_live()?1:0;
    if(atomic_load_explicit(&attached,memory_order_acquire)!=1)return 0;
    if(!mimus_board_live())return -1;
    struct a733_typec_token fresh;
    int r=a733_typec_snapshot(&board.typec.provider,&fresh,board.session.deadline_us-10000);
    if(r!=1)return r;
    return fresh.provider==attachment.provider&&fresh.owner==attachment.owner&&
        fresh.generation==attachment.generation&&fresh.polarity==attachment.polarity?1:-1;
}
/* Only the privileged USB2 owner accesses its mutable controller state. */
void mimus_board_host_observed(int ready)
{
    BOARD_PROGRESS(BOARD_SERVICE_USB2);
    if(ready&&a733_usb2_device_kind(&board.host)!=(board.session.workload==4?A733_USB2_DEVICE_DUAL:A733_USB2_DEVICE_MSC)){board_fail(NULL);return;}
    if(!ready)return;
    int live=board.host.core.ops.live?board.host.core.ops.live(board.host.core.context,
        board.host.core.generation,board.host.platform.config.lifetime_deadline):-1;
    if(live<0){atomic_store_explicit(&observed_until,0,memory_order_release);return;}
    if(!live)return; /* Publication busy preserves the previous bounded token. */
    if(!atomic_load_explicit(&attached,memory_order_acquire)){
        attachment=board.host.platform.attachment;
        atomic_store_explicit(&attached,1,memory_order_release);
    }
    atomic_store_explicit(&observed_until,board.host.platform.attachment.observed_us+A733_TYPEC_FRESH_US,memory_order_release);
}
void mimus_board_fail(void){board_fail(NULL);}
/* EL0-safe: one atomic store. A resident image lists the recognizer as its
 * first service after the board's own. */
void mimus_board_progress(void){BOARD_PROGRESS(BOARD_SERVICES);}
/* Named entries for the integrated OS start list (apps/os/start.c); this
 * image alone binds them to the single board application hooks below. */
int mimus_board_task_privileged(void (*entry)(void *),void *context)
{(void)entry;(void)context;return 0;}
int mimus_board_pins(struct a733_pin_handoff *pins)
{
    if(!pins||(board.session.workload!=0&&board.session.workload!=4))return 0;
    /* configure.py places each nonempty asset at a fixed page-aligned address. */
    static const struct a733_mapping_rule readable={A733_MAPPING_READ,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1},
        writable={A733_MAPPING_WRITE,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    for(unsigned i=0;i<3;++i)
        if(!a733_mapping_check_pages(aarch64_device_translate,mimus_asset_address[i],mimus_asset_address[i],
               mimus_asset_bytes[i],&readable))return 0;
    return a733_mapping_check_pages(aarch64_device_translate,0x46000000,0x46000000,0x01000000,&writable);
}
int mimus_board_application_start(void){return (board.session.workload==0||board.session.workload==4)&&mimus_board_start();}
#ifndef SUNSTONE_A733_OS
int board_application_task_privileged(void (*entry)(void *),void *context)
{return mimus_board_task_privileged(entry,context);}
int board_application_pins(struct a733_pin_handoff *pins){return mimus_board_pins(pins);}
int board_application_start(void){return mimus_board_application_start();}
#endif

int mimus_board_keyboard(void){return board.session.workload==4;}
int mimus_board_trigger_step(void)
{
    if(!mimus_board_live())return -1;
    return mimus_board_keyboard()?mimus_keyboard_step():1;
}
int mimus_board_trigger_copy(uint8_t out[128])
{
    if(!mimus_board_keyboard()||!mimus_board_live())return -1;
    int r=mimus_keyboard_copy(out);return mimus_board_live()?r:-1;
}
/* Called exclusively by the existing USB2 owner, never by an EL0 task. */
int mimus_board_keyboard_poll(void)
{
    struct a733_usb2_native *n=&board.host;
    if(!n->hid_mounted||!tuh_hid_mounted((uint8_t)n->hid_address,0)||n->hid.subclass!=1||n->hid.protocol!=1||
       tuh_hid_get_protocol((uint8_t)n->hid_address,0)!=HID_PROTOCOL_BOOT)return 0;
    if(n->hid.pending){
        struct a733_usb2_hid_report raw;int r=a733_usb2_hid_result(n,&raw);
        if(r<0)return 0;
        if(r==1){
            if(raw.generation!=n->core.generation||raw.generation!=n->platform.attachment.generation||raw.bytes!=8)return 0;
            struct mimus_keyboard_report p={.generation=raw.generation,.sequence=raw.sequence,
                .address=raw.address,.speed=raw.speed,.interface=raw.interface,.protocol=raw.protocol,
                .endpoint=raw.endpoint,.subclass=1,.mode=HID_PROTOCOL_BOOT,.bytes=8};
            memcpy(p.data,raw.data,8);if(mimus_keyboard_offer(&p)!=1)return 0;
        }
    }
    if(!n->hid.pending&&mimus_keyboard_empty()&&!a733_usb2_hid_receive(n,n->platform.config.lifetime_deadline))return 0;
    return 1;
}

int mimus_board_rearm(void){return mimus_board_live()&&mimus_keyboard_rearm();}
