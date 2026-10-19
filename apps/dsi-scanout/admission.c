#include "dsi-session.h"
#include "a733_device_memory_layout.h"
#include "a733_storage_digest.h"
#include <string.h>
/* Use the exact single-core board prefix validator and existing power allowlist.
 * Its incompatible ordinary contract validator is compiled under private names. */
#define a733_smp_contract_valid dsi_unused_board_contract
#define a733_board_contract_valid dsi_unused_board_contract_fields
#include "prefix-admission.inc"
#undef a733_smp_contract_valid
#undef a733_board_contract_valid
int a733_smp_contract_valid(const struct a733_smp_contract *c,uint64_t now)
{
 const struct dsi_session *s=(const void *)A733_BOARD_SESSION_ADDRESS;
 const struct a733_device_memory_layout layout={1,A733_DEVICE_WB_START,A733_DEVICE_WB_END,A733_DEVICE_NC_START,A733_DEVICE_NC_END};
 if(!c||c->magic!=A733_SMP_CONTRACT_MAGIC||c->version!=2||c->bytes!=288||
 !a733_device_memory_handoff_valid(&layout,c->reserved)||!a733_board_session_valid(&s->board,now)||!dsi_profile_valid(s)||
 s->board.workload!=2||c->epoch!=s->board.epoch||c->generation!=s->board.generation||c->inverse_generation!=~c->generation||
 c->lease!=s->board.lease||memcmp(c->build_id,a733_smp_build_id,32)||memcmp(s->board.image,c->build_id,32)||
 c->armed_at>now||c->armed_at>UINT64_MAX-60*A733_SMP_COUNTER_HZ||
 c->startup_deadline!=c->armed_at+A733_BOARD_STARTUP_SECONDS*A733_SMP_COUNTER_HZ||now>=c->startup_deadline||
 c->diagnostic_deadline!=c->armed_at+60*A733_SMP_COUNTER_HZ||c->expires_at!=c->diagnostic_deadline||
 c->watchdog_mode||c->watchdog_configuration||c->watchdog_output||c->fel_flag||c->redirect_flag||c->redirect_entry||c->alternate_flag)return 0;
 const uint8_t *session=dsi_contract_session(c),*entry=dsi_contract_entry(c);uint8_t hash[32];
 if(!session||!entry||memcmp(entry,s->board.qualification,32))return 0;
 storage_digest_bytes(s,sizeof(*s),hash);return !memcmp(hash,session,32);
}
