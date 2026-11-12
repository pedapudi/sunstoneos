/* Real protocol workers compare fixed 1 ms owner passes with identical peer
 * rules. Immediate replies isolate scheduling; serialized replies use 8N1 at
 * the admitted baud with zero controller processing time. Neither measures
 * physical UART, controller or task execution latency. */
#include "headset-sco-fixture.h"
#include <inttypes.h>
static struct audio_fixture scan;
static uint64_t scan_partial_serial;
static unsigned scan_partial_offset,scan_turn;
static int previous_consumer_rotation(uint64_t);
static int previous_acl_progress(uint64_t);
#include "headset-scan-step.h"

/* Exact prior one-consumer rotation: the regression baseline. */
static int previous_consumer_rotation(uint64_t bound)
{
    if(scan.g.rf.d.lower.q.state!=BT_L2CAP_ACTIVE)return bt_acl_step(&scan.g.rf.d.lower.a,64,bound);
    int r=0;unsigned audio=0,advanced=0;
    switch(scan_turn++%4){
    case 0:
        if(scan.q.initialized){r=bt_sco_step(&scan.q,64,bound);audio=advanced=1;}
        break;
    case 1:
        if(scan.g.rf.d.client.l2cap&&scan.g.rf.d.client.phase!=BT_SDP_CLOSED){
            if(scan.g.rf.d.client.phase!=BT_SDP_CLOSING&&bt_sdp_close(&scan.g.rf.d.client,bound)!=1)return -1;
            r=bt_sdp_step(&scan.g.rf.d.client,64,bound);advanced=1;}
        break;
    case 2:r=bt_sdp_server_step(&scan.g.sdp,64,bound);advanced=1;break;
    default:
        if(scan.g.ag.rfcomm){r=stop_control_step(bound);advanced=1;}
        else if(scan.g.rf.client.l2cap&&scan.g.rf.client.phase!=BT_RFCOMM_CLOSED){
            if(scan.g.rf.client.phase!=BT_RFCOMM_CLOSING&&bt_rfcomm_close(&scan.g.rf.client,bound)!=1)return -1;
            r=bt_rfcomm_step(&scan.g.rf.client,64,bound);advanced=1;}
        break;
    }
    if(!advanced)r=bt_l2cap_step(&scan.g.rf.d.lower.q,64,bound);
    if(r<0)return -1;
    if(scan.q.initialized&&((!audio&&bt_sco_service(&scan.q)!=1)||stop_audio_records()!=1))return -1;
    return r;
}

static int previous_acl_progress(uint64_t bound)
{
    if(bt_l2cap_step(&scan.g.rf.d.lower.q,64,bound)!=1||!stop_audio_service())return -1;
    return 1;
}

static const char *schedule_name;
static uint64_t origin;
static size_t start_tx;
static unsigned replied,wire_delay;
static uint64_t reply_due,classified_at,queued_at,status_at,terminal_at;
static void peer_reply(struct fixture *h)
{
    if(replied||!reply_due||h->now<reply_due)return;
    cs(h,0x042a,1,0);
    const uint8_t refused[]={0x0d,0,0,9,2,3,4,5,6,2,0,0,0,0,0,0,0};
    event(h,0x2c,refused,sizeof(refused));replied=1;
    printf("%s peer-reply-ready=%" PRIu64 " us observed=%" PRIu64 " us\n",
           schedule_name,reply_due-origin,h->now-origin);
}
static int timed_write(void *p,uintptr_t address,uint32_t value)
{
    struct fixture *h=p;size_t before=h->tn;
    if(!queued_at&&scan.q.refuse==2)queued_at=h->now;
    int r=write32(p,address,value);
    if(h->tn!=before){
        if(before==start_tx)printf("%s first-TX=%" PRIu64 " us\n",schedule_name,h->now-origin);
        if(h->tn==start_tx+11){
            const uint8_t command[]={1,0x2a,4,7,9,2,3,4,5,6,0x0d};
            assert(!memcmp(h->tx+start_tx,command,sizeof(command))&&!replied);
            printf("%s UART-accepted=%" PRIu64 " us bytes=11\n",schedule_name,h->now-origin);
            /* 8N1 serializes 10 bits/byte. Both complete replies become
             * available after 11 command and 27 response bytes at the actual
             * admitted baud. Controller processing adds zero modeled delay.
             * This is a deterministic wire lower bound, not a board measure. */
            reply_due=h->now+(wire_delay ? (38u*10u*1000000u+h->uart.plan.actual_baud-1)/h->uart.plan.actual_baud : 0);
            peer_reply(h);
        }
    }
    return r;
}
static void observe(const char *step_name,int result)
{
    struct fixture *h=&scan.g.rf.d.lower.f;
    if(!classified_at&&scan.q.refuse_deadline)
        classified_at=scan.q.refuse_deadline-scan.q.binding.response_timeout_us;
    if(!queued_at&&scan.q.refuse==2)queued_at=h->now;
    if(!status_at&&scan.q.refuse_acknowledgement)status_at=h->now;
    if(!terminal_at&&scan.q.refuse_acknowledgement&&!scan.q.refuse)terminal_at=h->now;
    printf("%s %s at=%" PRIu64 " us result=%d refuse=%u HCI=%u command=%u "
           "SCO-error=%u deadline=%" PRIu64 " us RX-unread=%zu TX=%zu\n",
           schedule_name,step_name,h->now-origin,result,scan.q.refuse,
           h->hci.state,h->hci.command,scan.q.error,
           scan.q.refuse_deadline ? scan.q.refuse_deadline-origin : 0,
           h->rn-h->ri,h->tn-start_tx);
}
static void prepare_acl_stop(void)
{
    struct fixture *h=&scan.g.rf.d.lower.f;
    assert(bt_sco_stop(&scan.q,90000)==1);command(&scan,0x0406);status(&scan,0x0406,0);
    const uint8_t audio_done[]={0,0,0,0x13};event(h,5,audio_done,sizeof(audio_done));
    assert(audio_poll(&scan)==1&&bt_sco_quiescent(&scan.q)==1);
    assert(bt_acl_stop(&scan.g.rf.d.lower.a,90000)==1);
    for(unsigned i=0;i<8&&h->hci.command!=BT_HCI_WAITING;++i){
        ++h->now;h->room=1024;assert(stop_acl_step(90000)==1);}
    assert(h->hci.opcode==0x0406&&h->hci.command==BT_HCI_WAITING);
    cs(h,0x0406,1,0);++h->now;assert(stop_acl_step(90000)==1);
    assert(scan.g.rf.d.lower.a.links[0].state==BT_ACL_DISCONNECTING);
}
static void run_schedule(unsigned schedule,unsigned delayed)
{
    opened(&scan);struct fixture *h=&scan.g.rf.d.lower.f;
    assert(bt_acl_close_admission(&scan.g.rf.d.lower.a,90000)==1);
    assert(bt_sco_close_admission(&scan.q,90000)==1);
    if(schedule>=3)prepare_acl_stop();
    static const char *names[]={"normal-order","previous-rotation","return-round","return-ACL","previous-ACL"};
    schedule_name=names[schedule];
    origin=h->now;start_tx=h->tn;replied=0;reply_due=0;wire_delay=delayed;scan_turn=1;
    classified_at=queued_at=status_at=terminal_at=0;
    printf("%s wire-delay=%u baud=%u response-bound=%" PRIu64 " us\n",
           schedule_name,wire_delay,h->uart.plan.actual_baud,scan.q.binding.response_timeout_us);
    h->uart.ops.write32=timed_write;
    const uint8_t request[]={9,2,3,4,5,6,4,4,0x24,2};event(h,4,request,sizeof(request));
    int result=1;
    for(unsigned pass=1;pass<=3&&result>=0;++pass){
        h->now=origin+1000*pass;h->room=1024;peer_reply(h);
        if(schedule==0){
            /* No SDP client is active. This is control_step's actual server,
             * AG/RFCOMM order followed by bluetooth_headset_poll's SCO step. */
            result=bt_sdp_server_step(&scan.g.sdp,64,90000);observe("server",result);
            if(result>=0){result=bt_hfp_ag_step(&scan.g.ag,64,128,90000);observe("control",result);}
            if(result>=0){result=bt_sco_step(&scan.q,64,90000);observe("audio",result);}
        }else{
            if(schedule==1)result=previous_consumer_rotation(90000);
            else if(schedule==2)result=stop_consumers_step(90000);
            else if(schedule==3)result=stop_acl_step(90000);
            else result=previous_acl_progress(90000);
            observe("consumer",result);
        }
        if(pass==1)assert(scan.q.refuse_deadline==origin+3000);
        if(replied&&!scan.q.refuse)break;
    }
    assert(replied&&h->tn==start_tx+11);
    unsigned success=schedule==2||schedule==3||(schedule==0&&!delayed);
    if(success){
        assert(result==1&&!scan.q.refuse&&h->now==origin+2000&&!scan.q.error);
        assert(status_at&&terminal_at&&terminal_at<scan.q.refuse_deadline);
        if(schedule==3)assert(bt_sco_quiescent(&scan.q)==1);
    }
    else{
        assert(result==-1&&h->now==scan.q.refuse_deadline&&scan.q.refuse==2);
        assert(h->hci.state==BT_HCI_FAULT&&bt_sco_quiescent(&scan.q)==-1);
        printf("%s retained-link-phase=%u stop-close=%u\n",schedule_name,scan.q.links[0].phase,scan.q.stop_close);
        assert(scan.g.rf.d.lower.a.state==BT_ACL_FAULT);
        assert(!terminal_at);
    }
    printf("%s summary classified=%" PRIu64 " queued=%" PRIu64 " status=%" PRIu64
           " terminal=%" PRIu64 " final=%" PRIu64 " original-D=%" PRIu64 " result=%d\n",
           schedule_name,classified_at-origin,queued_at-origin,status_at?status_at-origin:0,
           terminal_at?terminal_at-origin:0,h->now-origin,scan.q.refuse_deadline-origin,result);
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    for(unsigned delayed=0;delayed<2;++delayed)
        for(unsigned schedule=0;schedule<5;++schedule)run_schedule(schedule,delayed);
    puts("PASS 1 ms owner timing with immediate and serialized peer replies, including late ACL-stop refusals");
    return 0;
}
