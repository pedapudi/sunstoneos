/* Radio station entry of the integrated OS image (docs/os-image.md). The
 * board composition owns the radio in workload 1: main.c starts the radio
 * owner and the network client, with the crypto engine entropy source that
 * the WPA2 supplicant uses, before the start list runs. It refuses radio and
 * crypto engine owners equal to a board owner, but it cannot see the owners
 * the OS session adds. This entry refuses those collisions before any
 * provider is constructed. */
#include "os.h"
#include "../board/application.h"
#include "a733_mapping_check.h"
#include "a733_storage_digest.h"
#include <string.h>
static struct board_radio_profile profile;
static unsigned admitted;
/* The board's own radio tasks are privileged by main.c. */
int os_radio_task_privileged(void (*entry)(void *),void *context){(void)entry;(void)context;return 0;}
int os_radio_pins(struct a733_pin_handoff *pins)
{
    const struct os_session *s=os_session_admitted();
    if(!pins||!s||s->board.workload!=1||s->board.radio_profile_address!=UINT64_C(0x43000000)||
       s->board.radio_profile_bytes!=sizeof(profile))return 0;
    static const struct a733_mapping_rule wb={A733_MAPPING_READ,1,{{A733_MAIR_NORMAL_WB,A733_SH_INNER}},0,1};
    if(!a733_mapping_check_pages(aarch64_device_translate,0x43000000,0x43000000,sizeof(profile),&wb))return 0;
    /* The profile copy main.c authenticates later has the same owners. */
    const volatile uint8_t *source=(const void *)UINT64_C(0x43000000);
    for(size_t i=0;i<sizeof(profile);++i)((uint8_t *)&profile)[i]=source[i];
    uint8_t hash[32];storage_digest_bytes(&profile,sizeof(profile),hash);
    if(memcmp(hash,s->board.radio_profile_digest,32))return 0;
    uint64_t owners[]={s->thermal_owner,
#ifdef SUNSTONE_A733_OS_AUDIO
        s->audio.dma.owner,
#endif
    };
    for(unsigned i=0;i<sizeof(owners)/sizeof(owners[0]);++i)
        if(owners[i]==profile.platform.owner||owners[i]==profile.ce.owner)return 0;
    admitted=1;return 1;
}
const struct board_radio_profile *os_radio_profile(void){return admitted?&profile:NULL;}
/* main.c created both radio tasks and started the radio before this list. */
int os_radio_start(void)
{
    const struct os_session *s=os_session_admitted();
    return s&&s->board.workload==1&&board.handles[5]&&board.handles[6];
}
