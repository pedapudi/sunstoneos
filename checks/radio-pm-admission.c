/* Only the PM latch contract: normal ownership versus returning preservation. */
#include "a733_board_application.h"
#include "boot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
const unsigned char a733_smp_build_id[32]={1};
int main(void)
{
    struct a733_board_session s={.magic=A733_BOARD_SESSION_MAGIC,
        .version=A733_BOARD_SESSION_VERSION,.bytes=sizeof(s),.epoch=1,.generation=2,.lease=3,
        .topology=4,.cold_master=5,.memory_authority=6,.recovery_authority=7,.sid_authority=8,
        .pmic_owner=10,.usb0_owner=11,.fabric_hz={24000000,24000000,24000000},
        .usb_supply_uv={3300000,3300000},.usb0_cold_reference=1,
        .radio_profile_address=0x43000000,.radio_profile_bytes=1024,
        .latch_m_identity=9,.latch_m_owner=20,.latch_m_known_mask=0x3f,.latch_m_value=0x24};
#ifdef SUNSTONE_BOARD_SUPERVISED
    s.workload=3;
#else
    s.workload=1;
    s.minimum_reload_us=5000000;s.maximum_recovery_us=6000000;s.guard_us=10000;
    s.reset_by_us=9000000;s.deadline_us=2000000;s.reload_before_ticks=24;s.reload_after_ticks=48;
    s.latch_identity=20;s.latch_owner=21;s.latch_known_mask=0x3fff;
    s.typec_owner=12;s.usb2_owner=13;s.usb2_revision=1;
    s.source_min_mv=4750;s.source_max_mv=5500;s.source_ma=500;
    s.zero_threshold_max_mv=800;s.fusb_device_id=0x80;
#endif
    memset(s.image,1,32);memset(s.qualification,2,32);memset(s.recovery,2,32);
    memset(s.wiring,3,32);memset(s.memory,4,32);memset(s.sid_evidence,5,32);
    memset(s.radio_profile_digest,6,32);
    assert(a733_board_session_valid(&s,100));
    s.latch_m_known_mask=0;s.latch_m_value=0;
#ifdef SUNSTONE_BOARD_SUPERVISED
    assert(a733_board_session_valid(&s,100));
#else
    assert(!a733_board_session_valid(&s,100));
#endif
    s.latch_m_value=1;assert(!a733_board_session_valid(&s,100));s.latch_m_value=0;
    s.latch_m_known_mask=0x1f;assert(!a733_board_session_valid(&s,100));s.latch_m_known_mask=0x3f;
    s.latch_m_value=0x40;assert(!a733_board_session_valid(&s,100));s.latch_m_value=0;
    s.latch_m_identity=0;assert(!a733_board_session_valid(&s,100));s.latch_m_identity=9;
    s.latch_m_owner=0;assert(!a733_board_session_valid(&s,100));
    puts("Radio PM admission: normal versus returning records and malformed records passed");
    return 0;
}
