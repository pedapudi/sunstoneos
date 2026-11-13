/* Read the actual application's private protocol state through its pure timer
 * query. Driver timer activation/retirement predicates are the reference; this
 * fixture performs no UART, radio, provider or clock callback. */
#include "host_rtos.h"
#ifndef HEADSET_SOURCE
#define HEADSET_SOURCE "../apps/bluetooth/headset.c"
#endif
#include HEADSET_SOURCE
#include <assert.h>
#include <stdio.h>

#define LIMIT UINT64_C(1000000)
static struct bluetooth_application application;
static struct bluetooth_profile profile;
static unsigned cases;
static void baseline(void)
{
    memset(&application,0,sizeof(application));memset(&profile,0,sizeof(profile));
    memset(&voice,0,sizeof(voice));memset(&context,0,sizeof(context));
    application.self=&application;application.profile=&profile;application.owner=xTaskGetCurrentTaskHandle();
    application.deadline=LIMIT;application.firmware_generation=1;atomic_store(&application.result,1);
    profile.version=2;profile.headset.mode=2;profile.headset.receive_latency_us=1000;
    application.hci.self=&application.hci;application.hci.state=BT_HCI_IDENTIFIED;
    application.hci.transport.state=BT_H4_ACTIVE;application.uart.config.deferred_irq=1;
    voice.app=&application;voice.stage=AG;
    assert(bluetooth_application_next_deadline(&application,UINT64_MAX)==LIMIT);
}
#define CASE(name,activate,field,retire) do{baseline();activate;field=123; \
    assert(bluetooth_application_next_deadline(&application,UINT64_MAX)==123);retire; \
    assert(bluetooth_application_next_deadline(&application,UINT64_MAX)==LIMIT); \
    ++cases;puts("PASS " name);}while(0)
static void test(void *unused)
{
    (void)unused;
    CASE("firmware bootstrap",application.firmware_generation=0,application.bootstrap_deadline,application.firmware_generation=1);
    CASE("HCI identity",atomic_store(&application.result,0),application.hci_deadline,atomic_store(&application.result,1));
    CASE("HCI command",application.hci.command=BT_HCI_WAITING,application.hci.command_deadline,application.hci.command=BT_HCI_IDLE);
    CASE("HCI initialization",application.hci.state=BT_HCI_INITIALIZING,application.hci.initialization_deadline,application.hci.state=BT_HCI_IDENTIFIED);
    CASE("HCI incoming setup",application.hci.state=BT_HCI_CONFIGURING,application.hci.configuration_deadline,application.hci.state=BT_HCI_IDENTIFIED);
    CASE("H4 partial frame",(void)0,application.hci.transport.partial_deadline,application.hci.transport.partial_deadline=0);
    CASE("H4 TX",application.hci.transport.tx_length=4,application.hci.transport.tx_deadline,application.hci.transport.tx_offset=4);
    CASE("application stop",application.stop_phase=1,application.stop_deadline,application.stop_phase=0);
    CASE("headset operation",(void)0,voice.operation_deadline,voice.operation_deadline=0);
    CASE("headset stop",context.stop_phase=STOP_SCAN,context.stop_deadline,context.stop_phase=STOP_DONE);
    CASE("headset close",context.stop_phase=STOP_AUDIO,context.close_deadline,context.stop_phase=STOP_DONE);
    CASE("capture window",voice.stage=IDLE,voice.window,voice.stage=CLOSE);
    CASE("capture deadline",voice.stage=CAPTURE,voice.capture_deadline,voice.stage=CLOSE);
    CASE("silence cadence",voice.stage=CAPTURE,voice.next_silence,voice.stage=CLOSE);
    CASE("security lifetime",voice.security.self=&voice.security,voice.security.deadline,voice.security.self=NULL);
    CASE("security command",voice.security.self=&voice.security;voice.security.pending=1,voice.security.command_deadline,voice.security.pending=0);
    CASE("ACL attempt",voice.acl.state=BT_ACL_ACTIVE;voice.acl.links[0].state=BT_ACL_CONNECTING,voice.acl.links[0].deadline,voice.acl.links[0].state=BT_ACL_CONNECTED);
    CASE("ACL listen",voice.acl.state=BT_ACL_ACTIVE;voice.acl.links[0].state=BT_ACL_WAIT_REQUEST,voice.acl.links[0].deadline,voice.acl.links[0].state=BT_ACL_CLOSED);
    CASE("ACL TX",voice.acl.state=BT_ACL_ACTIVE;voice.acl.packets[0].state=BT_ACL_SENT,voice.acl.packets[0].deadline,voice.acl.packets[0].state=BT_ACL_CREDIT_RETIRED);
    CASE("L2CAP caller",voice.l2cap.state=BT_L2CAP_ACTIVE;voice.l2cap.channels[0].state=BT_L2CAP_CONNECTING,voice.l2cap.channels[0].deadline,voice.l2cap.channels[0].state=BT_L2CAP_OPEN);
    CASE("L2CAP RTX",voice.l2cap.state=BT_L2CAP_ACTIVE;voice.l2cap.channels[0].state=BT_L2CAP_CONFIGURING;voice.l2cap.channels[0].timer=BT_L2CAP_RTX,voice.l2cap.channels[0].timer_deadline,voice.l2cap.channels[0].state=BT_L2CAP_CLOSED);
    CASE("L2CAP ERTX",voice.l2cap.state=BT_L2CAP_ACTIVE;voice.l2cap.channels[0].state=BT_L2CAP_DISCONNECTING;voice.l2cap.channels[0].timer=BT_L2CAP_ERTX,voice.l2cap.channels[0].timer_deadline,voice.l2cap.channels[0].state=BT_L2CAP_QUARANTINED);
    CASE("L2CAP partial RX",voice.l2cap.state=BT_L2CAP_ACTIVE;voice.l2cap.rx[0].state=BT_L2CAP_PARTIAL,voice.l2cap.rx[0].deadline,voice.l2cap.rx[0].state=BT_L2CAP_RECEIVED);
    CASE("L2CAP signal RX",voice.l2cap.state=BT_L2CAP_ACTIVE;voice.l2cap.rx[0].state=BT_L2CAP_SIGNAL,voice.l2cap.rx[0].deadline,voice.l2cap.rx[0].state=BT_L2CAP_FREE);
    CASE("L2CAP TX",voice.l2cap.state=BT_L2CAP_ACTIVE;voice.l2cap.tx[0].state=BT_L2CAP_TRANSMITTING,voice.l2cap.tx[0].deadline,voice.l2cap.tx[0].state=BT_L2CAP_COMPLETED);
    CASE("SDP query",voice.discovery.phase=BT_SDP_RUNNING;voice.discovery.status=BT_SDP_PENDING,voice.discovery.deadline,voice.discovery.status=BT_SDP_COMPLETE);
    CASE("SDP close",voice.discovery.phase=BT_SDP_CLOSING,voice.discovery.close_deadline,voice.discovery.phase=BT_SDP_CLOSED);
    CASE("SDP server response",voice.server.initialized=1;voice.server.sessions[0].channel=1;voice.server.sessions[0].tx_serial=1,voice.server.sessions[0].deadline,voice.server.sessions[0].tx_serial=0);
    CASE("SDP server close",voice.server.initialized=1;voice.server.sessions[0].channel=1;voice.server.sessions[0].closing=1,voice.server.sessions[0].close_deadline,voice.server.sessions[0].close_started=1);
    CASE("RFCOMM opening",voice.rfcomm.phase=BT_RFCOMM_OPENING,voice.rfcomm.deadline,voice.rfcomm.phase=BT_RFCOMM_OPEN);
    CASE("RFCOMM listen",voice.rfcomm.phase=BT_RFCOMM_LISTENING;voice.rfcomm.incoming=1,voice.rfcomm.deadline,voice.rfcomm.channel=1);
    CASE("RFCOMM control timer",voice.rfcomm.phase=BT_RFCOMM_OPEN,voice.rfcomm.timer,voice.rfcomm.phase=BT_RFCOMM_CLOSED);
    CASE("RFCOMM unsent TX",voice.rfcomm.phase=BT_RFCOMM_OPEN;voice.rfcomm.tx[0].state=BT_RFCOMM_QUEUED,voice.rfcomm.tx[0].deadline,voice.rfcomm.tx[0].state=BT_RFCOMM_SUBMITTED);
    CASE("RFCOMM close",voice.rfcomm.phase=BT_RFCOMM_CLOSING,voice.rfcomm.close_deadline,voice.rfcomm.phase=BT_RFCOMM_CLOSED);
    CASE("HFP setup",voice.ag.phase=BT_HFP_AG_INITIALIZING,voice.ag.setup_deadline,voice.ag.phase=BT_HFP_AG_READY);
    CASE("HFP line",voice.ag.phase=BT_HFP_AG_READY,voice.ag.line_deadline,voice.ag.phase=BT_HFP_AG_CLOSING);
    CASE("HFP codec",voice.ag.phase=BT_HFP_AG_READY,voice.ag.codec_deadline,voice.ag.codec_deadline=0);
    CASE("HFP response",voice.ag.phase=BT_HFP_AG_READY;voice.ag.response_length=1,voice.ag.response_deadline,voice.ag.response_accepted=1);
    CASE("SCO setup",voice.audio.initialized=1;voice.audio.links[0].phase=BT_SCO_SETUP_WAIT,voice.audio.links[0].deadline,voice.audio.links[0].phase=BT_SCO_OPEN);
    CASE("SCO TX",voice.audio.initialized=1;voice.audio.tx[0].state=BT_SCO_SENT,voice.audio.tx[0].deadline,voice.audio.tx[0].state=BT_SCO_RETIRED);
    CASE("SCO reject",voice.audio.initialized=1;voice.audio.refuse=3,voice.audio.refuse_deadline,voice.audio.refuse=0);
    baseline();voice.last_owner=1;
    assert(receive_timely(&application,1000000));application.uart.config.deferred_irq=0;
    assert(!receive_timely(&application,1000000)&&receive_timely(&application,1001));
    application.uart.config.deferred_irq=1;assert(!receive_timely(&application,0));
    baseline();profile.headset.mode=4;assert(bluetooth_application_next_deadline(&application,UINT64_MAX)==0);
    printf("PASS %u active/retired protocol timers and event-mode idle gap\n",cases);
}
int main(void){host_rtos_run(test,NULL,2,1000000);return 0;}
