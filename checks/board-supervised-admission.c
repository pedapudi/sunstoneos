#include "a733_board_application.h"
#include "a733_storage_digest.h"
#include "boot.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
const unsigned char a733_smp_build_id[32]={1};
int main(int argc,char **argv)
{
    struct a733_board_session s={.magic=A733_BOARD_SESSION_MAGIC,.version=A733_BOARD_SESSION_VERSION,
        .bytes=sizeof(s),.epoch=1,.generation=2,.lease=3,.topology=4,.cold_master=5,
        .memory_authority=6,.recovery_authority=7,.workload=2,.pmic_owner=10,.usb0_owner=11,
        .fabric_hz={24000000,24000000,24000000},.usb_supply_uv={3300000,3300000,1200000,900000},.usb0_cold_reference=1};
    memset(s.image,1,32);memset(s.qualification,2,32);memset(s.recovery,2,32);
    memset(s.wiring,3,32);memset(s.memory,4,32);
    assert(a733_board_session_valid(&s,100));
    struct a733_board_session takeover=s;takeover.usb0_cold_reference=2;
#ifdef SUNSTONE_SD_SINGLE_CORE
    assert(a733_board_session_valid(&takeover,100));
    takeover.workload=0;assert(!a733_board_session_valid(&takeover,100));
#else
    assert(!a733_board_session_valid(&takeover,100));
#endif
    takeover=s;takeover.usb0_cold_reference=3;assert(!a733_board_session_valid(&takeover,100));
    struct a733_smp_contract c={.magic=A733_BOARD_CONTRACT_MAGIC,.version=A733_BOARD_CONTRACT_VERSION,
        .bytes=sizeof(c),.epoch=1,.generation=2,.inverse_generation=~UINT64_C(2),.lease=3,.armed_at=24,
        .startup_deadline=24000024,.diagnostic_deadline=1440000024,.expires_at=1440000024,
        .reserved={A733_BOARD_SESSION_ADDRESS,sizeof(s)}};
    memcpy(c.build_id,a733_smp_build_id,32);storage_digest_bytes(&s,sizeof(s),&c.reserved[2]);
    assert(a733_board_contract_valid(&c,&s,a733_smp_build_id,100));
    struct a733_board_session bad=s;bad.minimum_reload_us=1;assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.session_start_ticks=24;assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.deadline_us=60000000;assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.magic=UINT64_C(0x41533733424f4131);assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.source_ma=500;assert(!a733_board_session_valid(&bad,100));
    c.watchdog_mode=0x41;assert(!a733_board_contract_valid(&c,&s,a733_smp_build_id,100));c.watchdog_mode=0;
    c.redirect_flag=0xfa50392f;assert(!a733_board_contract_valid(&c,&s,a733_smp_build_id,100));c.redirect_flag=0;
    assert(!a733_board_contract_valid(&c,&s,a733_smp_build_id,c.startup_deadline));
    bad=s;bad.workload=4;assert(!a733_board_session_valid(&bad,100));
    bad=s;bad.workload=3;assert(!a733_board_session_valid(&bad,100));
    bad.sid_authority=8;memset(bad.sid_evidence,5,32);bad.radio_profile_address=0x43000000;
    bad.radio_profile_bytes=1024;memset(bad.radio_profile_digest,6,32);
    bad.latch_m_identity=9;bad.latch_m_owner=20;bad.latch_m_known_mask=0x3f;
    assert(a733_board_session_valid(&bad,100));
    assert(argc==2);FILE *f=fopen(argv[1],"wb");assert(f&&fwrite(&s,1,sizeof(s),f)==sizeof(s)&&!fclose(f));
    return 0;
}
