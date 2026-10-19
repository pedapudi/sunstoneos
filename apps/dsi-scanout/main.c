#include "dsi.h"
#include "platform.h"
#include "a733_storage_digest.h"
#include <string.h>
__attribute__((used,retain)) const uint64_t dsi_application_geometry[8]={DSI_PROFILE_MAGIC,1,4096,288,256,0x78000000,0x79000000,60};
int board_application_task_privileged(void (*entry)(void *),void *context){return !context&&entry==dsi_task;}
int board_application_pins(struct a733_pin_handoff *pins)
{
 const struct dsi_session *s=dsi_admitted_session();
 if(!pins||!s||!dsi_profile_valid(s)||memcmp(&s->board,&board.admitted_session,sizeof(s->board)))return 0;
 /* The full sealed page and physical entry record bind these exact routes.
  * The global pin provider still validates all expected fields before writes. */
 for(unsigned i=0;i<s->values[DSI_PIN_COUNT];++i)pins->authorized[s->pins[i].bank]|=1u<<s->pins[i].pin;
 dsi.session=*s;storage_digest_bytes(&dsi.session,sizeof(dsi.session),dsi.profile_hash);dsi.admitted=1;return 1;
}
int board_application_start(void)
{
 if(!dsi.admitted||memcmp(dsi.session.board.image,a733_smp_build_id,32))return 0;
 atomic_init(&dsi.result,0);atomic_init(&dsi.failed,0);
 dsi.handle=xTaskCreateStatic(dsi_task,"dsi",8192,NULL,4,dsi.stack,&dsi.task);return dsi.handle!=NULL;
}
