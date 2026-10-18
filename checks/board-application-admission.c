#include "a733_board_application.h"
#include "a733_storage_digest.h"
#include "boot.h"
#include <assert.h>
#include <string.h>
const unsigned char a733_smp_build_id[32]={1};
int main(void)
{
    struct a733_board_session s={.magic=A733_BOARD_SESSION_MAGIC,.version=1,.bytes=sizeof(s),
        .epoch=1,.generation=2,.lease=3,.minimum_reload_us=5000000,.maximum_recovery_us=6000000,
        .guard_us=10000,.reset_by_us=9000000,.deadline_us=2000000,.reload_before_ticks=24,.reload_after_ticks=48,
        .topology=4,.cold_master=5,.memory_authority=6,.recovery_authority=7,.sid_authority=8,
        .latch_identity=20,.latch_owner=21,.latch_known_mask=0x3fff,
        .pmic_owner=10,.usb0_owner=11,.typec_owner=12,.usb2_owner=13,
        .fabric_hz={100000000,100000000,100000000},.usb_supply_uv={3300000,3300000},
        .usb0_cold_reference=1,.usb2_revision=1,.source_min_mv=4750,.source_max_mv=5500,
        .source_ma=500,.zero_threshold_max_mv=800,.fusb_device_id=0x80};
    memset(s.image,1,32);memset(s.qualification,2,32);memset(s.recovery,2,32);
    memset(s.wiring,3,32);memset(s.memory,4,32);memset(s.sid_evidence,5,32);
    assert(a733_board_session_valid(&s,100));
    struct a733_smp_contract c={.magic=A733_BOARD_CONTRACT_MAGIC,.version=1,.bytes=sizeof(c),
        .epoch=1,.generation=2,.inverse_generation=~UINT64_C(2),.lease=3,.armed_at=24,
        .startup_deadline=24000024,.diagnostic_deadline=48000000,.expires_at=216000000,
        .watchdog_mode=0x41,.watchdog_configuration=1,.watchdog_output=2,.fel_flag=0x5aa5a55a,
        .redirect_flag=0xfa50392f,.redirect_entry=0x20,.reserved={A733_BOARD_SESSION_ADDRESS,sizeof(s)}};
    memcpy(c.build_id,a733_smp_build_id,32);storage_digest_bytes(&s,sizeof(s),&c.reserved[2]);
    assert(a733_board_contract_valid(&c,&s,a733_smp_build_id,100));
    struct a733_board_session bad=s;bad.memory_authority=0;assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.sid_authority=0;assert(!a733_board_session_valid(&bad,100));
    bad=s;memset(bad.recovery,0,32);assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.usb2_owner=bad.usb0_owner;assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.zero_threshold_max_mv=801;assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.deadline_us=5000000;assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.source_ma=499;assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.fabric[0]^=1;assert(!a733_board_contract_valid(&c,&bad,a733_smp_build_id,100));
    assert(!a733_board_contract_valid(&c,&s,a733_smp_build_id,c.startup_deadline));
    c.build_id[0]^=1;assert(!a733_board_contract_valid(&c,&s,a733_smp_build_id,100));
    return 0;
}
