/* Actual component queries and publications; device and lower-pass boundaries
 * are modeled. Real queue admission and SDP server scans execute. */
#include "host_rtos.h"
#ifndef HEADSET_SOURCE
#define HEADSET_SOURCE "../apps/bluetooth/headset.c"
#endif
#include HEADSET_SOURCE
#include <assert.h>
#include <stdio.h>

static struct bluetooth_application app;
static struct bluetooth_profile profile;
static struct a733_native_usb shared;
static atomic_uint recovery;
static struct bluetooth_work_state before,after;
static unsigned lower_passes,observed,expected;
static uint8_t payload[4]={1,2,3,4};
/* The server follows its lower-layer pass. No device or fake time advancement
 * is needed to decide whether that pass enqueues work for the next one. */
int bt_l2cap_step(struct bt_l2cap *q,unsigned budget,uint64_t until)
{assert(q==&voice.l2cap&&budget&&until);++lower_passes;return 1;}
int bt_hci_step(struct bt_hci *h,unsigned budget,uint64_t until)
{assert(h==&app.hci&&budget&&until);return 1;}
static int queue_policy(void *v,uint64_t link,uint16_t psm,unsigned incoming)
{(void)v;assert(link==1&&psm==1&&incoming);return 1;}
static void baseline(void)
{
    memset(&app,0,sizeof(app));memset(&profile,0,sizeof(profile));
    memset(&voice,0,sizeof(voice));memset(&context,0,sizeof(context));
    atomic_store(&recovery,0);
    app.self=&app;app.profile=&profile;app.shared=&shared;app.recovery_state=&recovery;
    app.owner=xTaskGetCurrentTaskHandle();profile.version=3;profile.headset.mode=2;
    app.hci.self=&app.hci;app.hci.state=BT_HCI_IDENTIFIED;app.hci.acl_owner=&voice.acl;
    app.hci.transport.state=BT_H4_ACTIVE;app.hci.transport.count=4;app.hci.transport.assembling=4;
    app.hci.transport.binding.uart=&app.uart.uart;
    voice.app=&app;voice.segment_app=&app;voice.segment_consumer=app.owner;
    voice.acl.hci=&app.hci;voice.acl.state=BT_ACL_ACTIVE;
    voice.acl.active_tx=BT_ACL_TX_SLOTS;voice.acl.command_link=BT_ACL_LINKS;
    voice.acl.l2cap_owner=&voice.l2cap;
    voice.acl.links[0].state=BT_ACL_CONNECTED;voice.acl.links[0].generation=1;
    voice.l2cap.acl=&voice.acl;voice.l2cap.state=BT_L2CAP_ACTIVE;voice.l2cap.next_serial=1;
    voice.l2cap.timeout_us=1000000;voice.l2cap.ertx_us=60000000;
    for(unsigned i=1;i<BT_L2CAP_CHANNELS;i+=2){
        voice.l2cap.channels[i]=(struct bt_l2cap_channel){.state=BT_L2CAP_OPEN,.generation=i+1,
            .link=1,.psm=1,.remote_cid=0x40+i,.remote_mtu=1024};
        voice.server.sessions[i]=(struct bt_sdp_server_session){.link=1,.channel=i+1,.deadline=1000000};
    }
    voice.server.l2cap=&voice.l2cap;voice.server.initialized=voice.server.lower_checked=1;
    voice.server.policy=queue_policy;
}
static void snap(struct bluetooth_work_state *out)
{assert(bluetooth_application_work_state(&app,out)==1);}
static void wrong_query(void *p)
{*(int *)p=bluetooth_application_work_state(&app,&after);}
static void wrong_request(void *p)
{*(int *)p=bluetooth_headset_segment_request(&app,BLUETOOTH_SEGMENT_START,0);}
static void notifier(void *unused)
{
    (void)unused;
    for(;;){
        (void)ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
        if(expected==1)assert(atomic_load_explicit(&voice.segment_request,memory_order_acquire)==BLUETOOTH_SEGMENT_START);
        else if(expected==2)assert(atomic_load_explicit(&voice.segment_tail,memory_order_acquire)==1);
        else if(expected==3)assert(atomic_load_explicit(&voice.segment_stop,memory_order_acquire)==1);
        else assert(0);
        ++observed;
    }
}
static void test(void *unused)
{
    (void)unused;baseline();snap(&before);
    app.started=1;app.hci.transport.now=100;voice.last_owner=100;voice.security.now=100;
    voice.pcm[0]^=1;voice.security.commands[0].bytes[0]^=1;
    voice.l2cap.requests[0].sequence=23;
    snap(&after);assert(!memcmp(&before,&after,sizeof(before)));
    assert(!ulTaskNotifyTake(pdTRUE,0));
    memset(&after,0xa5,sizeof(after));TaskHandle_t other=host_server_create("other",4);int result=9;
    host_call(other,wrong_query,&result);assert(!result&&((uint8_t *)&after)[0]==0xa5);
    assert(!bluetooth_application_work_state(&app,(void *)voice.pcm)&&voice.pcm[0]==1);
    context.app=&app;assert(!bluetooth_application_work_state(&app,(void *)&context)&&context.app==&app);
    atomic_store(&recovery,1);
    assert(!bluetooth_application_work_state(&app,(void *)&recovery)&&atomic_load(&recovery)==1);
    baseline();voice.app=NULL;app.hci=(struct bt_hci){0};snap(&after);
    assert(!after.hci.state&&!after.h4.count);
    assert(bluetooth_application_rx_unclassified(&app)==-1);
    app.hci.state=BT_HCI_STOPPED;app.stop_phase=3;snap(&after);assert(after.application.stop_phase==3);
    atomic_store(&app.failed,1);assert(!bluetooth_application_work_state(&app,&after));
    puts("PASS exact metadata, bootstrap/stop, caller/alias/failure guards and ignored clocks/payload/history");

    baseline();assert(!bluetooth_application_rx_unclassified(&app));
    app.hci.state=BT_HCI_STOPPED;assert(!bluetooth_application_rx_unclassified(&app));
    app.hci.transport.state=BT_H4_CLOSED;assert(bluetooth_application_rx_unclassified(&app)==-1);
    app.hci.transport.state=BT_H4_ACTIVE;
    app.hci.transport.staged=1;assert(bluetooth_application_rx_unclassified(&app)==1);
    app.hci.transport.staged=0;app.hci.transport.assembling=0;assert(bluetooth_application_rx_unclassified(&app)==1);
    app.hci.transport.assembling=4;app.hci.transport.records[0].owner=BT_H4_PARTIAL;
    assert(bluetooth_application_rx_unclassified(&app)==1);
    app.hci.transport.records[0].owner=BT_H4_COMPLETE;assert(bluetooth_application_rx_unclassified(&app)==1);
    app.hci.transport.records[0].owner=BT_H4_OWNED;app.hci.transport.records[0].serial=8;
    voice.acl.seen_record=7;assert(bluetooth_application_rx_unclassified(&app)==1);
    voice.acl.seen_record=8;assert(!bluetooth_application_rx_unclassified(&app));
    app.hci.transport.records[1].owner=BT_H4_OWNED;app.hci.transport.records[1].serial=9;
    assert(bluetooth_application_rx_unclassified(&app)==1);
    voice.acl.hci=NULL;voice.acl.seen_record=99;assert(bluetooth_application_rx_unclassified(&app)==1);
    atomic_store(&app.failed,1);assert(bluetooth_application_rx_unclassified(&app)==-1);
    puts("PASS RX age retained across staging, partial, COMPLETE and unclassified OWNED records");

    baseline();voice.acl.packets[0].state=BT_ACL_QUEUED;voice.acl.packets[0].serial=1;
    voice.acl.packets[0].deadline=1000000;
    snap(&before);
    for(unsigned i=0;i<8;++i){assert(bt_acl_step(&voice.acl,64,1000)==1);snap(&after);assert(!memcmp(&before,&after,sizeof(before)));}
    assert(!ulTaskNotifyTake(pdTRUE,0));
    baseline();uint64_t serial=0;
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i)voice.l2cap.tx[i].state=BT_L2CAP_TRANSMITTING;
    snap(&before);
    assert(!bt_l2cap_send(&voice.l2cap,2,payload,sizeof(payload),1000,&serial));
    snap(&after);assert(!memcmp(&before,&after,sizeof(before)));
    for(unsigned pending=0;pending<2;++pending){
        for(unsigned i=1;i<BT_L2CAP_CHANNELS;i+=2)voice.server.sessions[i].pending_length=pending?2:0;
        snap(&before);unsigned rotations=0,old=voice.server.rotation;
        for(unsigned i=0;i<8;++i){
            assert(bt_sdp_server_step(&voice.server,64,1000)==1);
            rotations+=old!=voice.server.rotation;old=voice.server.rotation;
            snap(&after);assert(!memcmp(&before,&after,sizeof(before)));
        }
        assert(rotations);
    }
    assert(!ulTaskNotifyTake(pdTRUE,0));
    voice.l2cap.tx[0].state=BT_L2CAP_FREE;snap(&before);unsigned passes=lower_passes;
    assert(bt_sdp_server_step(&voice.server,64,1000)==1&&lower_passes==passes+1);
    snap(&after);assert(memcmp(&before,&after,sizeof(before))&&voice.l2cap.tx[0].state==BT_L2CAP_TRANSMITTING);
    puts("PASS unchanged blocked credit/full pool and rotating server; lower enqueue after its pass changes snapshot");

    baseline();app.uart.config.deferred_irq=1;
    app.owner=host_task_create("notify-owner",notifier,NULL,4);
    expected=1;assert(bluetooth_headset_segment_request(&app,BLUETOOTH_SEGMENT_START,0)==1&&observed==1);
    assert(bluetooth_headset_segment_request(&app,BLUETOOTH_SEGMENT_START,0)==1&&observed==1);
    host_call(other,wrong_request,&result);assert(result==-1&&observed==1);
    atomic_store(&voice.segment_head,1);expected=2;
    assert(bluetooth_headset_segment_ack(&app,1)==1&&observed==2);
    assert(bluetooth_headset_segment_ack(&app,1)==-1&&observed==2);
    expected=3;assert(bluetooth_headset_segment_stop(&app)==1&&observed==3);
    assert(bluetooth_headset_segment_stop(&app)==1&&observed==3);
    atomic_store(&voice.segment_stop,0);atomic_store(&voice.segment_request,0);app.uart.config.deferred_irq=0;
    assert(bluetooth_headset_segment_request(&app,BLUETOOTH_SEGMENT_START,1)==1&&observed==3);
    puts("PASS changed publications precede deferred-owner hints; idempotent/refused/legacy calls stay silent");
    printf("PASS snapshot_bytes=%zu pair_bytes=%zu\n",sizeof(before),2*sizeof(before));
}
int main(void){host_rtos_run(test,NULL,2,1000000);return 0;}
