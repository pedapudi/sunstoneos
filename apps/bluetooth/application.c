#include "application.h"
#include "aarch64_device_memory.h"
#include "boot.h"
#include <string.h>
struct bluetooth_application bluetooth;
struct bluetooth_profile bluetooth_profile;
static StaticTask_t owner_storage;
static StackType_t owner_stack[4096];
static TaskHandle_t owner;
int bluetooth_boot_profile(const struct a733_board_session *s,const struct bluetooth_profile *p)
{
    if(!s||!p||s->workload!=2||!s->sid_authority||s->sid_authority!=p->sid_authority||
        !bluetooth_profile_valid(p,s->sid_evidence)||p->topology!=s->topology||
        memcmp(p->radio.fabric,s->fabric,sizeof(s->fabric))||
        memcmp(p->radio.fabric_hz,s->fabric_hz,sizeof(s->fabric_hz)))return 0;
    uint64_t owners[]={s->pmic_owner,s->usb0_owner,p->radio.owner,p->uart.owner};
    for(unsigned i=0;i<4;++i){if(!owners[i])return 0;for(unsigned j=0;j<i;++j)if(owners[i]==owners[j])return 0;}
    return 1;
}
static void bluetooth_owner(void *context)
{
    (void)context;for(;;){
        if(!board_live(NULL))(void)bluetooth_application_retain(&bluetooth);
        else if(atomic_load_explicit(&providers.shared.pmic_ready,memory_order_acquire))
            (void)bluetooth_application_poll(&bluetooth);
        vTaskDelay(1);
    }
}
int board_application_task_privileged(void (*entry)(void *),void *context)
{return !context&&entry==bluetooth_owner;}
int board_application_pins(struct a733_pin_handoff *pins)
{
    const struct bluetooth_profile *p=(const void *)BLUETOOTH_PROFILE_ADDRESS;
    const struct a733_board_session *s=&board.session;
    if(!pins||!bluetooth_boot_profile(s,p)||pins->authorized[A733_PIN_G]||pins->authorized[A733_PIN_M])return 0;
    for(uintptr_t page=BLUETOOTH_PROFILE_ADDRESS;page<0x43100000;page+=4096){
        struct aarch64_translation t;
        if(!aarch64_device_translate(page,0,&t)||t.physical!=page||t.attributes!=0xff||t.shareability!=3)return 0;
    }
    memset(&bluetooth_profile,0,sizeof(bluetooth_profile));
    memcpy(&bluetooth_profile,p,p->bytes);
    pins->authorized[A733_PIN_G]=0x3ff;pins->authorized[A733_PIN_M]=0x1b;
    pins->latch[1]=p->pm_latch;return 1;
}
int board_application_start(void)
{
    if(owner||!bluetooth_boot_profile(&board.session,&bluetooth_profile))return 0;
    owner=xTaskCreateStatic(bluetooth_owner,"bluetooth",4096,NULL,4,owner_stack,&owner_storage);
    if(!owner)return 0;
    return bluetooth_application_init(&bluetooth,&bluetooth_profile,&providers.shared,bluetooth_chip_id(),
        owner,&board.watchdog_state,board.session.session_start_ticks/24,board.session.deadline_us,board.session.sid_evidence);
}
