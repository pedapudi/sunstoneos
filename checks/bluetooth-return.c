/* The actual application stop state machine composes with real UART/H4/HCI.
 * Module readbacks and the platform CLOSED response are modeled here; their
 * driver checks exercise those register sequences independently. */
#define BLUETOOTH_APPLICATION_FIXTURE_ENTRY archived_application_main
#include "bluetooth-application.c"
#include <stdlib.h>
static void answer(void)
{
    if(app.hci.command!=BT_HCI_WAITING||wire.ri!=wire.rn)return;
    assert(app.hci.opcode==0x0c1a||app.hci.opcode==0x0c03);
    cc(&wire,app.hci.opcode,1,1,0);
}
static void hci_start(void)
{
    assert(bluetooth_application_poll(&app)==0&&app.stage==1);
    bootstrap_ready=1;
    assert(bluetooth_application_poll(&app)==0&&app.stage==2);
}
static void admission_cases(void)
{
    struct a733_clock_token token={.owner=profile.radio.owner};
    assert(radio_admit(&app,&app.radio_config,&token)==1);
    app.radio.bluetooth_stop.authority=37;
    assert(radio_admit(&app,&app.radio_config,&token)==0);
    app.radio.bluetooth_stop=(struct a733_radio_bluetooth_stop){0};
    profile.version=4;
    profile.bluetooth_stop=(struct a733_radio_bluetooth_stop){.authority=37,.topology=2,
        .wiring_evidence={1},.expected={1,0,0}};
    app.radio.bluetooth_stop=profile.bluetooth_stop;
    assert(radio_admit(&app,&app.radio_config,&token)==1);
    app.radio.bluetooth_stop.wiring_evidence[31]=1;
    assert(radio_admit(&app,&app.radio_config,&token)==0);
    app.attached=1;
    assert(wake_admit(&app,&app.radio.config,&token)==0);
    app.radio.bluetooth_stop=profile.bluetooth_stop;
    assert(wake_admit(&app,&app.radio.config,&token)==1);
    profile.version=1;
    assert(wake_admit(&app,&app.radio.config,&token)==0);
    app.radio.bluetooth_stop=(struct a733_radio_bluetooth_stop){0};
    assert(wake_admit(&app,&app.radio.config,&token)==1);
    app.attached=0;
}
int main(int argc,char **argv)
{
    assert(argc==2);unsigned mode=(unsigned)strtoul(argv[1],NULL,0);
    application_fixture_init();
    if(mode==10)admission_cases();
    uint64_t deadline=3000000;
    if(mode!=1)hci_start(); /* Mode 1 stops during UART configuration. */
    if(mode==2)return_peer=0; /* WLAN DOWN is insufficient. */
    if(mode==3)uart_busy=1;
    if(mode==4||mode==8||mode==9){app.attached=1;app.session.stop_requested=1;}
    if(mode==5)return_held=-1;
    if(mode==6){wire.room=1;assert(bluetooth_application_poll(&app)==0);}
    if(mode==7)return_residual=129;
    int r=0;unsigned passes=0,commands=0;size_t tx=wire.tn;
    for(;passes<100&&r==0;++passes){
        r=bluetooth_application_stop(&app,deadline);
        if(mode==3&&passes==4)wire.now=deadline;
        if(mode==6&&passes==4)wire.now=deadline;
        if(app.attached&&!atomic_load(&app.stop_peer)&&bluetooth_application_module_release(&app)==1){
            assert(!uart_closes);
            bluetooth_application_module_down(&app,mode==8?-1:mode==9?2:1);
        }
        if(mode!=6)answer();
        if(wire.tn!=tx){tx=wire.tn;++commands;}
        if(mode!=3&&mode!=6)wire.now+=1000;
    }
    unsigned accesses=wire.accesses,polls=radio_polls,closes=uart_closes;
    if(mode==2||mode==3||mode==5||mode==6||mode==8||mode==9){
        assert(r==-1&&!uart_closes&&app.stop_phase==STOP_FAULT);
        for(unsigned i=0;i<3;++i)assert(bluetooth_application_stop(&app,deadline+1000000)==-1);
    }else{
        assert(r==1&&uart_closes==1&&app.stop_phase==STOP_DONE&&bluetooth_application_module_release(&app)==1);
        for(unsigned i=0;i<3;++i)assert(bluetooth_application_stop(&app,deadline+1000000)==1);
    }
    assert(wire.accesses==accesses&&radio_polls==polls&&uart_closes==closes);
    assert(app.stop_deadline==deadline&&passes<100);
    printf("{\"case\":%u,\"passes\":%u,\"commands\":%u,\"elapsed_us\":%llu,\"result\":%d}\n",
        mode,passes,commands,(unsigned long long)wire.now,r);
    return 0;
}
