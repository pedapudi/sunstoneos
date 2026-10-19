#include "../board/application.h"
#include "application.h"
#include "assets.h"
#include "keyboard.h"
#include <string.h>
#include "host/usbh.h"
#include "class/hid/hid_host.h"
#include "boot.h"
#include "aarch64_device_memory.h"
static atomic_uint attached;
static atomic_uint_fast64_t observed_until;
static struct a733_typec_token attachment;
int mimus_board_session_live(void)
{return board_live(NULL)&&atomic_load_explicit(&board.result,memory_order_acquire)!=2;}
/* EL0-safe: no provider calls, MMIO, locks or privileged register accesses. */
int mimus_board_live(void)
{return board_live(NULL)&&atomic_load_explicit(&board.result,memory_order_acquire)!=2&&
    a733_smp_counter()/24<atomic_load_explicit(&observed_until,memory_order_acquire);}
/* Called only by the privileged USB0 owner. */
int mimus_board_export_live(void)
{
    if(!board_live(NULL)||atomic_load_explicit(&board.result,memory_order_acquire)==2)return -1;
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
int board_application_task_privileged(void (*entry)(void *),void *context)
{(void)entry;(void)context;return 0;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    if(!pins||(board.session.workload!=0&&board.session.workload!=4))return 0;
    for(unsigned i=0;i<3;++i)for(uintptr_t page=mimus_asset_address[i]&~(uintptr_t)4095;
        page<mimus_asset_address[i]+mimus_asset_bytes[i];page+=4096){
        struct aarch64_translation t;
        if(!aarch64_device_translate(page,0,&t)||t.physical!=page||t.attributes!=0xff||t.shareability!=3)return 0;
    }
    for(uintptr_t page=0x46000000;page<0x47000000;page+=4096){
        struct aarch64_translation t;
        if(!aarch64_device_translate(page,1,&t)||t.physical!=page||t.attributes!=0xff||t.shareability!=3)return 0;
    }
    return 1;
}
int board_application_start(void){return (board.session.workload==0||board.session.workload==4)&&mimus_board_start();}

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
