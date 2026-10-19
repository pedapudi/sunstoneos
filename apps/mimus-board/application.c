#include "../board/application.h"
#include "application.h"
#include "assets.h"
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
    if(ready&&a733_usb2_device_kind(&board.host)!=A733_USB2_DEVICE_MSC){board_fail(NULL);return;}
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
    if(!pins||board.session.workload)return 0;
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
int board_application_start(void){return board.session.workload==0&&mimus_board_start();}
