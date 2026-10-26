/* Only the changed startup deadline branches; earlier protocol cases supply
 * fixtures and remain unexecuted. */
#ifdef CHECK_HEADSET
#include <assert.h>
#include <stdio.h>
#include "../apps/bluetooth/headset.c"
int main(void)
{
    struct bluetooth_profile p={.version=3};
    struct bluetooth_application a={.profile=&p,.resident=1,.started=1000000,.deadline=90000000};
    assert(stage_deadline(&a,48000000)==11000000);
    assert(stage_deadline(&a,59500000)==11000000);
    p.version=2;assert(stage_deadline(&a,48000000)==9000000);
    assert(stage_deadline(&a,59500000)==20500000);
    p.headset.mode=4;assert(stage_deadline(&a,48000000)==11000000);
    a.deadline=5000000;assert(stage_deadline(&a,48000000)==5000000);
    a.started=UINT64_MAX-100;a.deadline=UINT64_MAX-50;
    assert(stage_deadline(&a,48000000)==a.deadline);
    a.resident=0;a.started=1000000;assert(stage_deadline(&a,48000000)==41000000);
    p.headset.mode=0;assert(stage_deadline(&a,48000000)==49000000);
    puts("resident stage budgets, lifetime cap, saturation and supervised offsets");
}
#else
#define BLUETOOTH_MODULE_FIXTURE_ENTRY archived_module_main
#include "bluetooth-module-owner.c"
int main(int argc,char **argv)
{
    assert(argc==2);const char *name=argv[1];
    wire.now=1;wire.room=1024;providers.used=1;providers.boot.identity=99;
    profile=(struct bluetooth_profile){.magic=BLUETOOTH_PROFILE_MAGIC,.version=1,.bytes=624,
        .topology=2,.sid_authority=7,.radio_authority=3,.uart_authority=4,.wake_authority=5,
        .wiring_evidence={1},.sid_evidence={2},.fresh_transport_evidence={3},.pm_latch={6,7,0x3f,0},
        .radio={.owner=20,.topology={.identity=2}},.wake={.authority=5,.desired={{1,0,0},{0,0,0}}},
        .uart={.owner=21,.route=A733_UART1_BLUETOOTH,.format={1,8,2,0,1,24000000,1500000,0}},
        .identity={.chip_word=0x7,.subrevision_word=0x20},
        .firmware_address={0x43010000,0x43020000,0x43030000,0x43040000},.firmware_bytes={1708,24744,23288,341260},
        .session_owner=22,.runtime_lease_us=10000000,.renew_margin_us=3000000,
        .command_timeout_us=500000,.frame_timeout_us=100000,.binding_timeout_us=1000000};
    storage_digest_bytes(&profile,profile.bytes,digest);fresh_wire=wire;


    reset();
    unsigned resident=!strncmp(name,"resident",8),own=!strcmp(name,"own-late");
    if(own)assert(bluetooth_application_init(&app,&profile,&providers,&chip,owner,&latch,0,60000000,digest));
    else assert(attach(0,resident?BLUETOOTH_RESIDENT_END_US:60000000));
    scheduler=taskSCHEDULER_RUNNING;current_task=owner;
    assert(bluetooth_application_poll(&app)==0&&app.stage==1);
    if(!strcmp(name,"resident-pending")){
        /* No accepted generation: the provisional HCI offset is not armed. */
        wire.now=app.hci_deadline+1;
        assert(bluetooth_application_poll(&app)==0&&!app.firmware_generation);
        wire.now=app.bootstrap_deadline-1;
        assert(bluetooth_application_poll(&app)==0&&!app.firmware_generation);
        wire.now=app.bootstrap_deadline;
        assert(bluetooth_application_poll(&app)==-1&&uart_retains==1);
        assert((atomic_load(&latch)&4)&&!module.fault&&atomic_load(&board_latch)==1);
        puts(name);return 0;
    }
    bootstrap_ready=1;uart_busy=1;wire.now=app.bootstrap_deadline-1;
    int expected=0;
    if(!strcmp(name,"pending-before"))bootstrap_ready=0;
    else if(!strcmp(name,"pending-at")){bootstrap_ready=0;++wire.now;expected=-1;}
    else if(!strcmp(name,"ready-before")){}
    else if(!strcmp(name,"ready-at")){++wire.now;expected=-1;}
    else if(!strcmp(name,"ready-after")||own){wire.now+=2;expected=-1;}
    else if(!strcmp(name,"ready-cross")){bootstrap_tick=1;expected=-1;}
    else if(!strcmp(name,"clock-failure")){bootstrap_clock_fail=1;expected=-1;}
    else if(!strcmp(name,"zero-generation")){observed_generation=0;expected=-1;}
    else if(!strcmp(name,"resident-early")){
        /* The admitted resident version-3 schedule, independent of profile validation. */
        profile.version=3;wire.now=1000000;
    }else if(!strcmp(name,"resident-fixed"))wire.now=50000000;
    else if(!strcmp(name,"wake-wait")){uart_busy=0;wake_busy=1;}
    else if(!strcmp(name,"hci-wait")){uart_busy=0;}
    else assert(!strcmp(name,"uart-wait")||!strcmp(name,"module-wait")||!strcmp(name,"generation-change"));
    assert(bluetooth_application_poll(&app)==expected);
    if(expected<0)goto retained;
    if(!strcmp(name,"pending-before")){assert(!app.firmware_generation&&!wire.tn);goto done;}
    assert(app.firmware_generation==4&&!atomic_load(&app.result));
    if(!strcmp(name,"ready-before"))goto done;
    if(!strcmp(name,"resident-early"))assert(app.started==1000000&&app.hci_deadline==6000000);
    if(!strcmp(name,"resident-fixed"))assert(app.started==50000000&&app.hci_deadline==65000000);
    if(!strcmp(name,"hci-wait")){assert(app.stage==2);uart_busy=1;}
    uint64_t anchor=app.started,deadline=app.hci_deadline;
    if(!strcmp(name,"generation-change")){
        observed_generation=5;assert(bluetooth_application_poll(&app)==-1);goto retained;
    }
    if(!strcmp(name,"module-wait"))bootstrap_ready=0;
    if(!resident){wire.now=app.bootstrap_deadline;assert(bluetooth_application_poll(&app)==0);}
    wire.now=deadline-1;assert(bluetooth_application_poll(&app)==0);
    assert(app.started==anchor&&app.hci_deadline==deadline&&!atomic_load(&app.result));
    wire.now=deadline;assert(bluetooth_application_poll(&app)==-1);
retained:
    assert(atomic_load(&latch)&4);assert(uart_retains==1);
    assert(!module.fault&&atomic_load(&board_latch)==1&&!atomic_load(&app.result));
    if(own)assert(app.attached==0);else assert(app.attached==1);
done:
    puts(name);return 0;
}
#endif
