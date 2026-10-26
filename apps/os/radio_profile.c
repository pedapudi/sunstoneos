/* Resident radio profile bounds of the integrated OS image. It replaces
 * apps/board/radio_profile.c, which requires a session that ends 60 s after
 * it starts. A resident session has no end: main.c sets its deadline to
 * UINT64_MAX/24 us, beyond every counter value. Bootstrap and startup keep
 * fixed offsets from the session start; the radio lifetime and the client
 * deadline follow the session's. Firmware owners request four-bit 25 MHz
 * operation; the resident offsets retain the existing 120 s bound. This is configuration,
 * not a board measurement. A radio that misses them fails, stops reporting
 * progress, and the watchdog resets the board. */
#include "../board/application.h"
#include "a733_storage_digest.h"
#include <string.h>
#ifndef SUNSTONE_BOARD_RESIDENT
#error Supervised images use apps/board/radio_profile.c
#endif
#define OS_RADIO_BOOTSTRAP_US 120000000u
#define OS_RADIO_STARTUP_US 130000000u
#define OS_RADIO_CLIENT_MARGIN_US 10000u
int board_radio_profile_runtime(struct board_radio_profile *out,const struct board_radio_profile *raw,
    const struct a733_board_session *session,uint8_t digest[32])
{
    if(!out||!raw||!session||!digest||out==raw||raw->magic!=BOARD_RADIO_PROFILE_MAGIC||
       (raw->version!=BOARD_RADIO_PROFILE_SUPERVISED_VERSION&&raw->version!=BOARD_RADIO_PROFILE_LISTENER_SUPERVISED_VERSION)||
       raw->bytes!=sizeof(*raw)||!board_radio_profile_network_valid(raw)||raw->platform.handoff||raw->ce.handoff||
       raw->platform.deadline_us||raw->platform.lifetime_deadline_us||raw->ce.deadline_us||
       raw->bootstrap_deadline_us||raw->startup_deadline_us||raw->client_deadline_us||
       !session->generation||session->deadline_us!=UINT64_MAX/24||
       session->session_start_ticks/24>=session->deadline_us-OS_RADIO_CLIENT_MARGIN_US-OS_RADIO_STARTUP_US)return 0;
    uint8_t hash[32];storage_digest_bytes(raw,sizeof(*raw),hash);
    if(memcmp(hash,session->radio_profile_digest,32))return 0;
    *out=*raw;out->version=raw->version==BOARD_RADIO_PROFILE_LISTENER_SUPERVISED_VERSION?BOARD_RADIO_PROFILE_LISTENER_VERSION:BOARD_RADIO_PROFILE_VERSION;
    uint64_t start=session->session_start_ticks/24;
    out->platform.handoff=out->ce.handoff=session->generation;
    out->platform.deadline_us=out->ce.deadline_us=out->bootstrap_deadline_us=start+OS_RADIO_BOOTSTRAP_US;
    out->platform.lifetime_deadline_us=out->client_deadline_us=session->deadline_us-OS_RADIO_CLIENT_MARGIN_US;
    out->startup_deadline_us=start+OS_RADIO_STARTUP_US;
    storage_digest_bytes(out,sizeof(*out),digest);
    return 1;
}
