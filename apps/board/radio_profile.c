#include "application.h"
#include "a733_storage_digest.h"
#include <string.h>
#ifdef SUNSTONE_BOARD_SUPERVISED
int board_radio_profile_runtime(struct board_radio_profile *out,const struct board_radio_profile *raw,
    const struct a733_board_session *session,uint8_t digest[32])
{
    if(!out||!raw||!session||!digest||out==raw||raw->magic!=BOARD_RADIO_PROFILE_MAGIC||
       raw->version!=BOARD_RADIO_PROFILE_SUPERVISED_VERSION||raw->bytes!=sizeof(*raw)||!board_radio_profile_network_valid(raw)||raw->platform.handoff||raw->ce.handoff||
       raw->platform.deadline_us||raw->platform.lifetime_deadline_us||raw->ce.deadline_us||
       raw->bootstrap_deadline_us||raw->startup_deadline_us||raw->client_deadline_us||
       !session->generation||session->session_start_ticks/24>UINT64_MAX-60000000||
       session->deadline_us!=session->session_start_ticks/24+60000000)return 0;
    uint8_t hash[32];storage_digest_bytes(raw,sizeof(*raw),hash);
    if(memcmp(hash,session->radio_profile_digest,32))return 0;
    *out=*raw;out->version=BOARD_RADIO_PROFILE_VERSION;
    uint64_t start=session->session_start_ticks/24;
    out->platform.handoff=out->ce.handoff=session->generation;
    out->platform.deadline_us=out->ce.deadline_us=out->bootstrap_deadline_us=start+40000000;
    out->platform.lifetime_deadline_us=out->client_deadline_us=session->deadline_us-10000;
    out->startup_deadline_us=start+50000000;
    storage_digest_bytes(out,sizeof(*out),digest);
    return 1;
}
#endif
