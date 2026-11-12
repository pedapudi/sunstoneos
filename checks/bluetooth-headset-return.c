/* Execute the production upper owner. Protocol workers are explicit boundary
 * models here; the companion SCO check executes actual UART and wire workers. */
#include <assert.h>
#include <stdio.h>
#include "../apps/bluetooth/headset.c"
static struct bluetooth_application app;
static struct bluetooth_profile profile;
static atomic_uint recovery;
static unsigned calls,scan_ready,acl_done,security_done,audio_done,closed_ag;
static unsigned acl_stop_calls,close_server_calls,server_steps,security_stop_calls;
static uint64_t bound_seen;
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (TaskHandle_t)1;}
BaseType_t xTaskGenericNotify(TaskHandle_t task,UBaseType_t index,uint32_t value,eNotifyAction action,uint32_t *prior)
{(void)task;(void)index;(void)value;(void)action;(void)prior;return pdPASS;}
int bt_acl_close_admission(struct bt_acl *q,uint64_t d){assert(q==&voice.acl&&d);return 1;}
int bt_security_stop(struct bt_security *s,uint64_t d){assert(s==&voice.security&&d);++security_stop_calls;return 1;}
int bt_security_retain(struct bt_security *s){assert(s==&voice.security);return -1;}
int bt_security_service(struct bt_security *s,uint64_t d){assert(s==&voice.security&&d);return 1;}
int bt_security_quiescent(const struct bt_security *s){assert(s==&voice.security);return security_done;}
int bt_hci_step(struct bt_hci *h,unsigned b,uint64_t d){assert(h==&app.hci&&b==64&&d);++calls;return 1;}
int bt_hci_scan_disabled(const struct bt_hci *h){assert(h==&app.hci);return scan_ready;}
int bt_acl_step(struct bt_acl *q,unsigned b,uint64_t d){assert(q==&voice.acl&&b==64&&d);++calls;return 1;}
int bt_acl_stop(struct bt_acl *q,uint64_t d){assert(q==&voice.acl&&d);assert(!voice.audio.initialized||audio_done);++acl_stop_calls;return 1;}
int bt_acl_quiescent(const struct bt_acl *q){assert(q==&voice.acl);return acl_done;}
const struct bt_h4_record *bt_acl_receive(const struct bt_acl *q){assert(q==&voice.acl);return NULL;}
int bt_sco_stop(struct bt_sco *q,uint64_t d){assert(q==&voice.audio&&d);q->stop_deadline=d;return 1;}
int bt_sco_close_admission(struct bt_sco *q,uint64_t d){assert(q==&voice.audio&&d);q->stop_deadline=d;return 1;}
int bt_sco_step(struct bt_sco *q,unsigned b,uint64_t d){assert(q==&voice.audio&&b==64&&d);++calls;return 1;}
int bt_sco_service(struct bt_sco *q){assert(q==&voice.audio);return 1;}
int bt_sco_quiescent(const struct bt_sco *q){assert(q==&voice.audio);return audio_done;}
const struct bt_sco_packet *bt_sco_result(const struct bt_sco *q){assert(q==&voice.audio);return NULL;}
const struct bt_sco_packet *bt_sco_receive(const struct bt_sco *q){assert(q==&voice.audio);return NULL;}
int bt_sco_release_result(struct bt_sco *q,uint64_t serial){(void)q;(void)serial;assert(0);return -1;}
int bt_sco_release_receive(struct bt_sco *q,uint64_t serial){(void)q;(void)serial;assert(0);return -1;}
int bt_hfp_ag_voice(struct bt_hfp_ag *q,unsigned on){assert(q==&voice.ag&&!on);q->recognition=0;q->response_length=1;return 1;}
int bt_hfp_ag_step(struct bt_hfp_ag *q,unsigned b,unsigned bytes,uint64_t d)
{assert(q==&voice.ag&&b==64&&bytes==128&&d);++calls;q->response_length=0;
 if(q->phase==BT_HFP_AG_CLOSING){q->phase=BT_HFP_AG_CLOSED;voice.rfcomm.phase=BT_RFCOMM_CLOSED;}return 1;}
int bt_hfp_ag_close(struct bt_hfp_ag *q,uint64_t d)
{assert(q==&voice.ag&&d&&!q->recognition&&!q->response_length&&audio_done);++closed_ag;q->phase=BT_HFP_AG_CLOSING;return 1;}
int bt_rfcomm_step(struct bt_rfcomm *q,unsigned b,uint64_t d){assert(q==&voice.rfcomm&&b==64&&d);++calls;q->phase=BT_RFCOMM_CLOSED;return 0;}
int bt_rfcomm_close(struct bt_rfcomm *q,uint64_t d){assert(q==&voice.rfcomm&&d);q->phase=BT_RFCOMM_CLOSING;return 1;}
const struct bt_rfcomm_buffer *bt_rfcomm_result(const struct bt_rfcomm *q){assert(q==&voice.rfcomm);return NULL;}
const struct bt_rfcomm_buffer *bt_rfcomm_receive(const struct bt_rfcomm *q){assert(q==&voice.rfcomm);return NULL;}
int bt_rfcomm_release_result(struct bt_rfcomm *q,uint64_t s){(void)q;(void)s;assert(0);return -1;}
int bt_rfcomm_release_receive(struct bt_rfcomm *q,uint64_t s){(void)q;(void)s;assert(0);return -1;}
int bt_sdp_close(struct bt_sdp *q,uint64_t d){assert(q==&voice.discovery&&d);q->phase=BT_SDP_CLOSING;return 1;}
int bt_sdp_step(struct bt_sdp *q,unsigned b,uint64_t d){assert(q==&voice.discovery&&b==64&&d);++calls;q->phase=BT_SDP_CLOSED;return 0;}
int bt_sdp_server_step(struct bt_sdp_server *q,unsigned b,uint64_t d)
{assert(q==&voice.server&&b==64&&d);++calls;++server_steps;
 if(voice.l2cap.channels[0].state==BT_L2CAP_CONNECTING&&server_steps==2)voice.l2cap.channels[0].state=BT_L2CAP_OPEN;
 if(q->sessions[0].closing){voice.l2cap.channels[0].state=BT_L2CAP_CLOSED;voice.l2cap.channels[0].completion=BT_L2CAP_CONFIRMED;}return 1;}
int bt_sdp_server_close(struct bt_sdp_server *q,uint64_t ch,uint64_t d)
{assert(q==&voice.server&&ch==7&&d&&voice.l2cap.channels[0].state==BT_L2CAP_OPEN);++close_server_calls;q->sessions[0].closing=1;return 1;}
int bt_sdp_server_release_channel(struct bt_sdp_server *q,uint64_t ch)
{assert(q==&voice.server&&ch==7&&voice.l2cap.channels[0].state==BT_L2CAP_CLOSED);q->sessions[0].channel=0;voice.l2cap.channels[0].state=BT_L2CAP_UNUSED;return 1;}
int bt_l2cap_step(struct bt_l2cap *q,unsigned b,uint64_t d){assert(q==&voice.l2cap&&b==64&&d);++calls;bound_seen=d;return 1;}
int bt_l2cap_close(struct bt_l2cap *q,uint64_t ch,uint64_t d){assert(q==&voice.l2cap&&ch&&d);return 0;}
static void reset(unsigned upper)
{
    memset(&app,0,sizeof(app));memset(&profile,0,sizeof(profile));memset(&voice,0,sizeof(voice));memset(&context,0,sizeof(context));atomic_store(&recovery,0);
    app.self=&app;app.profile=&profile;app.owner=(TaskHandle_t)1;app.recovery_state=&recovery;app.iteration_live=1;app.deadline=60000000;
    profile.version=3;profile.headset.mode=2;profile.headset.receive_latency_us=1000;
    calls=closed_ag=acl_stop_calls=close_server_calls=server_steps=security_stop_calls=0;
    scan_ready=acl_done=security_done=audio_done=1;bound_seen=0;
    if(upper){voice.acl.state=BT_ACL_ACTIVE;voice.security.self=&voice.security;voice.l2cap.state=BT_L2CAP_ACTIVE;voice.server.initialized=1;}
}
static int poll(uint64_t now,uint64_t d)
{
    unsigned before=calls;
    unsigned limit=voice.l2cap.state!=BT_L2CAP_ACTIVE?1:context.stop_phase==STOP_ACL?2:4;
    int r=bluetooth_headset_stop(&app,now,d);assert(calls-before<=limit);return r;
}
static void run_done(void)
{int r=0;uint64_t t=voice.last_owner+1;if(t<10)t=10;for(;t<100&&r==0;++t)r=poll(t,3000000);assert(r==1);}
int main(void)
{
    reset(0);app.iteration_live=0;assert(bluetooth_headset_stop_begin(&app,1,3000000)==1&&calls==0);
    app.iteration_live=1;scan_ready=0;assert(poll(2,3000000)==0);scan_ready=1;assert(poll(3,3000000)==1);
    unsigned terminal=calls;assert(poll(4000000,5000000)==1&&calls==terminal);
    assert(bluetooth_headset_retain(&app)==-1);assert(poll(4000001,5000000)==-1&&calls==terminal);
    reset(1);run_done();assert(acl_stop_calls==1&&security_stop_calls&&context.close_deadline==2000010);
    reset(1);voice.discovery.l2cap=&voice.l2cap;voice.discovery.phase=BT_SDP_RUNNING;voice.audio.initialized=1;
    assert(poll(10,3000000)==0&&calls==4);run_done();
    reset(1);voice.audio.initialized=1;voice.ag.rfcomm=&voice.rfcomm;voice.rfcomm.l2cap=&voice.l2cap;
    voice.ag.phase=BT_HFP_AG_READY;voice.ag.recognition=1;voice.rfcomm.phase=BT_RFCOMM_OPEN;audio_done=0;
    for(uint64_t t=10;t<20;++t)assert(poll(t,3000000)==0);
    assert(!closed_ag&&!acl_stop_calls&&voice.audio.stop_deadline);audio_done=1;run_done();assert(closed_ag==1&&acl_stop_calls==1);
    reset(1);voice.server.sessions[0].channel=7;voice.l2cap.channels[0].generation=7;voice.l2cap.channels[0].state=BT_L2CAP_CONNECTING;
    run_done();assert(close_server_calls==1&&server_steps>=3);
    reset(1);voice.operation_deadline=100;assert(poll(10,3000000)==0&&context.close_deadline==100);
    assert(poll(100,5000000)==-1);terminal=calls;assert(poll(101,5000000)==-1&&calls==terminal);
    reset(1);voice.l2cap.tx[0].state=BT_L2CAP_TRANSMITTING;voice.l2cap.tx[0].bytes[0]=0x5a;
    for(uint64_t t=10;t<30;++t)assert(poll(t,3000000)==0);
    assert(!acl_stop_calls&&voice.l2cap.tx[0].bytes[0]==0x5a);voice.l2cap.tx[0].state=BT_L2CAP_FREE;run_done();
    reset(1);voice.l2cap.channels[0].state=BT_L2CAP_QUARANTINED;int r=0;
    for(uint64_t t=10;t<30&&!r;++t)r=poll(t,3000000);assert(r==-1&&!acl_stop_calls);
    terminal=calls;assert(poll(30,3000000)==-1&&calls==terminal);
    reset(1);security_done=0;for(uint64_t t=10;t<30;++t)assert(poll(t,3000000)==0);
    assert(context.stop_phase==STOP_ACL);security_done=1;assert(poll(30,3000000)==1);
    for(unsigned mode=4;mode<=6;++mode){reset(0);profile.headset.mode=mode;assert(bluetooth_headset_stop_begin(&app,1,3000000)==-1&&calls==0);}
    puts("PASS nine upper return cases and three unsupported-mode refusals");
    return 0;
}
