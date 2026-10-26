/* Changed constructor and Bluetooth bootstrap polling only. Earlier application
 * bodies supply modeled providers but are not executed. */
#ifdef CHECK_BOARD
#define main archived_board_main
#include "board-radio.c"
#undef main
int main(int argc,char **argv)
{
    assert(argc==2);fast_refused=!strcmp(argv[1],"refused");
    shared.used=1;source.engine=&engine;engine.binding.registry=&shared.clocks;
    atomic_init(&recovery_state,1);
    struct board_radio_config c={.shared=&shared,.chip_id=&id,.admit=admission,.recovery_state=&recovery_state,
        .platform={.lifetime_deadline_us=10000},.session={.owner_task=task,.bootstrap_deadline=500},
        .entropy={.source=&source,.owner=task,.clock=&shared.counter,.qualification_record=9},
        .target={.security=AIC8800_WIFI_WPA2_PSK_CCMP,.beacon_evidence=8},
        .security={.beacon_record=8,.entropy_record=9},.network={.bootstrap=task,.network={.entropy_record=9}},
        .startup_deadline_us=8000,.association_timeout_us=1000,.security_timeout_us=2000};
    assert(board_radio_init(&app,&c)==!fast_refused&&fast_requests==1);
    assert(app.constructed==!fast_refused&&app.platform.four_bit==!fast_refused);
    assert(!session_calls);puts("Board constructor requests fast mode before session construction");
}
#else
#define BLUETOOTH_APPLICATION_FIXTURE_ENTRY archived_bluetooth_main
#include "bluetooth-application.c"
int main(int argc,char **argv)
{
    assert(argc==2);const char *name=argv[1];
    wire.now=1;wire.room=1024;providers.used=1;providers.boot.identity=99;atomic_init(&recovery,1);
    profile=(struct bluetooth_profile){.magic=BLUETOOTH_PROFILE_MAGIC,.version=1,.bytes=624,
        .topology=2,.sid_authority=7,.radio_authority=3,.uart_authority=4,.wake_authority=5,
        .wiring_evidence={1},.sid_evidence={2},.fresh_transport_evidence={3},.pm_latch={6,7,0x3f,0},
        .radio={.owner=20,.topology={.identity=2}},.wake={.authority=5,.desired={{1,0,0},{0,0,0}}},
        .uart={.owner=21,.route=A733_UART1_BLUETOOTH,.format={1,8,2,0,1,24000000,1500000,0}},
        .firmware_address={0x43010000,0x43020000,0x43030000,0x43040000},.firmware_bytes={1708,24744,23288,341260},
        .session_owner=22,.runtime_lease_us=10000000,.renew_margin_us=3000000,
        .command_timeout_us=500000,.frame_timeout_us=100000,.binding_timeout_us=1000000};
    uint8_t digest[32];storage_digest_bytes(&profile,profile.bytes,digest);
    fast_refused=!strcmp(name,"constructor-refused");
    assert(bluetooth_application_init(&app,&profile,&providers,&chip,owner,&recovery,0,60000000,digest)==!fast_refused);
    assert(fast_requests==1&&app.radio.four_bit==!fast_refused);
    if(!strncmp(name,"constructor",11)){assert(!radio_polls);puts(name);return 0;}
    scheduler=taskSCHEDULER_RUNNING;current_task=owner;
    assert(bluetooth_application_poll(&app)==0&&!radio_polls&&app.stage==1);
    uint64_t sampled=wire.now;unsigned expected=1,success=1;
    if(!strncmp(name,"progress-",9)){
        radio_progress=1u<<(unsigned)(name[9]-'0');radio_tick=200;expected=3;
    }else if(!strcmp(name,"busy")){radio_progress=15;radio_status=A733_SESSION_BUSY;}
    else if(!strcmp(name,"retained")){radio_status=A733_SESSION_RETAINED;success=0;}
    else if(!strcmp(name,"invalid")){radio_status=A733_SESSION_INVALID;success=0;}
    else if(!strcmp(name,"clock-failure")){clock_fail=1;success=0;}
    else if(!strcmp(name,"deadline")){radio_progress=15;radio_tick=100;app.bootstrap_deadline=50;}
    else if(!strcmp(name,"ready-boundary")){radio_progress=15;radio_tick=10;ready_on_poll=2;expected=2;}
    else if(!strcmp(name,"runtime")){app.stage=2;radio_progress=15;radio_tick=10;}
    else assert(!strcmp(name,"waiting"));
    assert(poll_module(&app,&sampled)==(int)success&&radio_polls==expected);
    if(success&&app.stage==1)assert(sampled==wire.now);
    if(!strcmp(name,"deadline")){
        /* The following caller deadline decision uses refreshed time. */
        radio_polls=0;wire.now=1;assert(bluetooth_application_poll(&app)==-1);
        assert(radio_polls==1&&radio_retains==1&&uart_retains==1&&!wire.tn);
    }
    puts(name);return 0;
}
#endif
