#include "headset.h"
#include "bluetooth.h"
#include "bt_security.h"
#include "bt_sdp_server.h"
#include "bt_sco.h"
#include "a733_storage_digest.h"
#include <string.h>
#ifdef SUNSTONE_BLUETOOTH_MSBC
#include "msbc.h"
#include "playback.h"
#endif
enum phase {CONFIGURE,CONFIGURING,SECURITY_CONFIG,CONNECT,AUTHENTICATE,SDP,CONTROL,AG,FALLBACK,AUDIO,CAPTURE,CLOSE,DONE};
static struct {
    struct bluetooth_application *app;
    struct bt_hci_incoming_config incoming;
    struct bt_acl acl;
    struct bt_security security;
    struct bt_l2cap l2cap;
    struct bt_rfcomm_registry registry;
    struct bt_sdp_server server;
    struct bt_sdp discovery;
    struct bt_rfcomm rfcomm;
    struct bt_hfp_ag ag;
    struct bt_sco audio;
    struct bt_sco_binding audio_binding;
    struct bluetooth_pcm_snapshot snapshot;
    struct bluetooth_pcm_record records[BLUETOOTH_PCM_RECORDS];
    uint8_t pcm[BLUETOOTH_PCM_BYTES],silence[254];
    uint64_t link,stream,last_owner,operation_deadline,capture_deadline,next_silence;
    unsigned stage,auth_started,silence_bytes,configured,wideband,silence_offset,silence_ready;
    atomic_uint terminal;
    void *segment_consumer;
    struct bluetooth_application *segment_app;
    atomic_ullong segment_head,segment_tail;
    atomic_uint segment_stop,segment_flags,segment_disconnected;
    struct {
        struct bluetooth_segment_snapshot snapshot;
        struct bluetooth_pcm_record records[BLUETOOTH_PCM_RECORDS];
        uint8_t pcm[BLUETOOTH_PCM_BYTES];
    } segments[BLUETOOTH_SEGMENT_SLOTS];
    uint64_t partial_serial;
    unsigned partial_offset,segment_bytes,segment_records;

} voice;
static struct {struct bluetooth_application *app;} context;
static int live(struct bluetooth_application *a)
{return a&&a==voice.app&&a->iteration_live&&xTaskGetCurrentTaskHandle()==a->owner&&
    !atomic_load_explicit(&a->failed,memory_order_acquire)&&!(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4);}
static int scheduling(void *ctx,const struct bt_h4_binding *b,uint64_t generation,uint64_t latency)
{
    struct bluetooth_application *a=voice.app;
    return ctx==&context&&context.app==a&&live(a)&&b==&a->hci.transport.binding&&
        generation==a->profile->headset.scheduling_authority&&latency==a->profile->headset.receive_latency_us&&
        b->firmware_generation==a->firmware_generation&&b->uart_generation==a->uart_generation&&
        a->profile->uart.format.auto_flow;
}
static int security(uint64_t link,uint64_t *generation,unsigned *sc)
{return live(voice.app)?bt_security_authorized(&voice.security,link,generation,sc):-1;}
static int policy(void *ctx,uint64_t link,uint16_t psm,unsigned incoming)
{
    (void)incoming;if(ctx!=&context||link!=voice.link)return -1;
    uint64_t generation;unsigned sc;
    if(security(link,&generation,&sc)!=1)return -1;
    return psm==1||psm==3;
}
static int psm(void *ctx,uint64_t link,uint16_t value,unsigned incoming)
{return value==1&&incoming?bt_sdp_server_admit(&voice.server,link,value,incoming):policy(ctx,link,value,incoming);}
static int service(void *ctx,uint64_t link,uint16_t uuid,uint8_t channel)
{
    if(ctx!=&context||!channel||channel>30||!voice.app)return -1;
    const struct bluetooth_headset_profile *p=&voice.app->profile->headset;
    if(p->incoming?(uuid!=0x111f||channel!=p->server_channel):uuid!=0x111e)return -1;
    return policy(ctx,link,3,p->incoming);
}
static int audio_current(void *ctx,const struct bt_h4_binding *b,uint64_t generation,uint64_t latency)
{return scheduling(ctx,b,generation,latency);}
static int audio_security(void *ctx,const struct bt_hfp_ag *ag,struct bt_sco_security *out)
{
    if(ctx!=&context||ag!=&voice.ag||!out)return -1;
    uint64_t generation;unsigned sc;
    if(security(voice.link,&generation,&sc)!=1)return -1;
    *out=(struct bt_sco_security){generation,sc};return 1;
}
static struct bt_acl_link *link(void)
{for(unsigned i=0;i<BT_ACL_LINKS;++i)if(voice.acl.links[i].generation==voice.link)return &voice.acl.links[i];return NULL;}
static struct bt_sco_link *stream(void)
{for(unsigned i=0;i<BT_SCO_LINKS;++i)if(voice.audio.links[i].generation==voice.stream)return &voice.audio.links[i];return NULL;}
int bluetooth_headset_retain(struct bluetooth_application *a)
{
    if(!a||xTaskGetCurrentTaskHandle()!=a->owner)return -1;
    atomic_fetch_or_explicit(a->recovery_state,4,memory_order_acq_rel);
    if(voice.app==a){if(voice.security.self)(void)bt_security_retain(&voice.security);
        atomic_store_explicit(&voice.terminal,2,memory_order_release);}
    return -1;
}
static int fail(void){return bluetooth_headset_retain(voice.app);}
static uint64_t cap(uint64_t now,uint64_t duration,uint64_t limit)
{uint64_t end=now>UINT64_MAX-duration?UINT64_MAX:now+duration;return end<limit?end:limit;}
static uint64_t stage_deadline(const struct bluetooth_application *a,uint64_t duration)
{
    /* Recognition reserves the final20s of the existing60s supervised session.
     * This allocation is not a physical inference-time qualification. */
    if((a->profile->version==3||a->profile->headset.mode==4)&&duration>40000000)duration=40000000;
    return a->started+duration;
}
static int upper_init(struct bluetooth_application *a)
{
    const struct bluetooth_headset_profile *p=&a->profile->headset;
    struct bt_security_policy sec={.allow_just_works=p->allow_just_works,.minimum_level=p->minimum_security,.deadline=a->deadline};
    memcpy(sec.peer,p->peer,6);memcpy(sec.pin,p->pin,17);
    voice.wideband=p->mode>=3&&(a->hci.identity.features[2]&8)&&(a->hci.identity.features[3]&0x80);
    struct bt_sdp_ag_profile ag={.handle=0x10000,.version=0x0109,.rfcomm_channel=(uint8_t)p->server_channel,
        .brsf_features=voice.wideband?512:0,.supported_features=voice.wideband?32:0};
    voice.audio_binding=(struct bt_sco_binding){p->scheduling_authority,p->receive_latency_us,p->response_timeout_us,audio_current,audio_security,&context};
    if(bt_acl_init(&voice.acl,&a->hci)!=1||bt_security_init(&voice.security,&voice.acl,a->owner,a->recovery_state,&sec,a->profile->command_timeout_us)!=1||
        bt_rfcomm_registry_init(&voice.registry,&voice.l2cap,psm,service,&context)!=1||
        bt_sdp_server_init(&voice.server,&voice.l2cap,&ag,1000000,2000000,policy,&context)!=1||
        bt_l2cap_init_bootstrap(&voice.l2cap,&voice.acl,p->mtu,1000000,60000000,bt_rfcomm_admit,&voice.registry,&a->hci.transport.binding)!=1||
        bt_sco_init(&voice.audio,&voice.acl,&voice.audio_binding)!=1)return fail();
    return 1;
}
static int control_step(struct bluetooth_application *a)
{
    if(voice.discovery.l2cap&&voice.discovery.phase!=BT_SDP_CLOSED&&
        bt_sdp_step(&voice.discovery,64,a->deadline)<0)return -1;
    if(bt_sdp_server_step(&voice.server,64,a->deadline)!=1)return -1;
    if(voice.ag.rfcomm){if(bt_hfp_ag_step(&voice.ag,64,128,a->deadline)<0)return -1;}
    else if(bt_rfcomm_step(&voice.rfcomm,64,a->deadline)<0)return -1;
    return bt_security_service(&voice.security,a->deadline);
}
static int repeated_audio(struct bluetooth_application *,struct bt_sco_link *,uint64_t);
int bluetooth_headset_poll(struct bluetooth_application *a,uint64_t now)
{
    if(!a||a->profile->version<2||!a->iteration_live||xTaskGetCurrentTaskHandle()!=a->owner)return -1;
    if(!voice.app){voice.app=a;context.app=a;voice.last_owner=now;}
    if(!live(a)||now<voice.last_owner||now-voice.last_owner>a->profile->headset.receive_latency_us)return fail();
    voice.last_owner=now;
#ifdef SUNSTONE_BLUETOOTH_MSBC
    /* No established stream exists to close yet. Stop setup and retain any
     * in-flight command/UART ownership; cancellation is not a disconnect. */
    if(bluetooth_playback_mode(a)&&voice.stage<=AUDIO&&bluetooth_playback_cancelled(a))return fail();
#endif
    if(a->profile->headset.mode<2&&atomic_load_explicit(&voice.terminal,memory_order_acquire))return 1;
    if(a->profile->headset.mode>=2&&voice.stage<AUDIO&&atomic_load_explicit(&voice.segment_stop,memory_order_acquire))return fail();
    const struct bluetooth_headset_profile *p=&a->profile->headset;
    if(voice.operation_deadline&&now>=voice.operation_deadline)return fail();
    if(voice.stage==CONFIGURE){
        voice.incoming=(struct bt_hci_incoming_config){.accept_timeout_slots=p->accept_slots,
            .page_interval_slots=p->page_interval,.page_window_slots=p->page_window,
            .service_generation=p->scheduling_authority,.receive_latency_us=p->receive_latency_us,.synchronous_audio=1,
            .current=scheduling,.context=&context};
        voice.operation_deadline=stage_deadline(a,48000000);
        if(bt_hci_configure_incoming(&a->hci,&voice.incoming,voice.operation_deadline)!=1)return fail();
        voice.stage=CONFIGURING;return 0;
    }
    if(voice.stage==CONFIGURING){
        if(bt_hci_step(&a->hci,64,voice.operation_deadline)!=1)return fail();
        if(!bt_hci_configuration_complete(&a->hci))return 0;
        if(upper_init(a)!=1)return -1;voice.stage=SECURITY_CONFIG;return 0;
    }
    if(voice.stage==SECURITY_CONFIG){
        if(bt_security_step(&voice.security,64,voice.operation_deadline)!=1)return fail();
        if(!voice.security.configured)return 0;
        voice.operation_deadline=stage_deadline(a,53000000);
        int result;
        if(p->incoming){
            result=bt_acl_incoming(&voice.acl,p->response_timeout_us);
            if(result==1)result=bt_acl_listen(&voice.acl,p->peer,voice.operation_deadline,&voice.link);
        } else {struct bt_acl_peer peer={.page_scan_mode=p->peer_page_scan_mode,
            .role_switch=p->peer_role_switch,.clock_offset=p->peer_clock_offset};memcpy(peer.address,p->peer,6);
            result=bt_acl_connect(&voice.acl,&peer,voice.operation_deadline,&voice.link);}
        if(result!=1||bt_security_attach(&voice.security,voice.link)!=1)return fail();
        voice.stage=CONNECT;return 0;
    }
    if(voice.stage==CONNECT||voice.stage==AUTHENTICATE){
        if(bt_security_step(&voice.security,64,voice.operation_deadline)!=1)return fail();
        struct bt_acl_link *l=link();if(!l||l->state==BT_ACL_CLOSED)return fail();
        if(voice.stage==CONNECT){
            if(l->state!=BT_ACL_CONNECTED)return 0;
            int r=bt_security_start(&voice.security,voice.link);if(r<0)return fail();if(!r)return 0;
            voice.auth_started=1;voice.stage=AUTHENTICATE;
        }
        uint64_t generation;unsigned sc;int r=security(voice.link,&generation,&sc);if(r<0)return fail();if(!r)return 0;
        voice.operation_deadline=stage_deadline(a,56000000);
        if(p->incoming){
            if(bt_rfcomm_listen(&voice.rfcomm,&voice.registry,voice.link,0x111f,(uint8_t)p->server_channel,p->mtu,voice.operation_deadline)!=1)return fail();
            voice.stage=CONTROL;
        } else {
            if(bt_sdp_begin(&voice.discovery,&voice.l2cap,voice.link,0x111e,voice.operation_deadline)!=1)return fail();voice.stage=SDP;
        }
        return 0;
    }
    if(voice.stage==SDP){
        if(bt_sdp_step(&voice.discovery,64,voice.operation_deadline)<0||bt_sdp_server_step(&voice.server,64,voice.operation_deadline)!=1||
            bt_security_service(&voice.security,voice.operation_deadline)!=1)return fail();
        if(voice.discovery.status==BT_SDP_PENDING)return 0;
        if(voice.discovery.status!=BT_SDP_COMPLETE)return fail();
        unsigned record=BT_SDP_RECORDS,endpoint=0;
        for(unsigned i=0;i<voice.discovery.records&&record==BT_SDP_RECORDS;++i){const struct bt_sdp_record *r=bt_sdp_record(&voice.discovery,i);
            if(!r||!r->matches_class)continue;
            for(unsigned j=0;j<r->endpoints;++j)if(r->endpoint[j].l2cap_psm==3&&r->endpoint[j].rfcomm_channel){record=i;endpoint=j;break;}}
        if(record==BT_SDP_RECORDS||bt_rfcomm_begin(&voice.rfcomm,&voice.registry,&voice.discovery,record,endpoint,p->mtu,voice.operation_deadline)!=1)return fail();
        if(bt_sdp_close(&voice.discovery,voice.operation_deadline)!=1)return fail();
        voice.stage=CONTROL;return 0;
    }
    if(control_step(a)!=1)return fail();
    if(voice.stage==CONTROL){
        if(voice.rfcomm.phase!=BT_RFCOMM_OPEN)return 0;
        static const uint8_t no_telephony[7]={0};
        if(bt_hfp_ag_init_codecs(&voice.ag,&voice.rfcomm,no_telephony,voice.operation_deadline,p->line_timeout_us,voice.wideband?6:0)!=1)return fail();
        voice.stage=AG;return 0;
    }
    if(voice.stage==AG||voice.stage==FALLBACK){
        if(voice.ag.phase!=BT_HFP_AG_READY)return 0;
        if(voice.wideband&&(voice.ag.hf_features&128)){
            unsigned desired=voice.stage==FALLBACK?1:(voice.ag.peer_codecs&4)?2:1;
            if(!voice.ag.selected_codec||voice.ag.selected_codec!=desired){
                if(!voice.ag.offered_codec){int selected=bt_hfp_ag_select_codec(&voice.ag,desired);if(selected<0)return fail();}
                return 0;
            }
        }
        if(p->mode>=4&&voice.ag.selected_codec!=2)return fail();
        voice.operation_deadline=stage_deadline(a,58000000);
        if(voice.stage==FALLBACK){if(bt_sco_retry_cvsd(&voice.audio,voice.stream,voice.operation_deadline,&voice.stream)!=1)return fail();}
        else if(bt_sco_open(&voice.audio,&voice.ag,0,voice.operation_deadline,&voice.stream)!=1)return fail();
        voice.stage=AUDIO;return 0;
    }
    if(bt_sco_step(&voice.audio,64,a->deadline)!=1||bt_security_service(&voice.security,a->deadline)!=1)return fail();
    const struct bt_sco_packet *tx=bt_sco_result(&voice.audio);
#ifdef SUNSTONE_BLUETOOTH_MSBC
    if(!bluetooth_playback_mode(a))
#endif
    if(tx&&bt_sco_release_result(&voice.audio,tx->serial)!=1)return fail();
    struct bt_sco_link *l=stream();if(!l)return fail();
    if(voice.stage==AUDIO){
        if(p->mode==3&&l->phase==BT_SCO_CLOSED&&l->codec==2&&l->fallback_allowed){voice.stage=FALLBACK;return 0;}
        if(l->phase==BT_SCO_CLOSED||l->phase==BT_SCO_RETAINED)return fail();
        if(l->phase!=BT_SCO_OPEN)return 0;
        if(p->mode>=4&&l->codec!=2)return fail();
        voice.capture_deadline=cap(now,p->capture_timeout_us,stage_deadline(a,59000000));
        voice.operation_deadline=p->mode>=2?0:voice.capture_deadline;voice.next_silence=now;
        voice.silence_bytes=voice.audio.mtu&~1u;if(voice.silence_bytes>254)voice.silence_bytes=254;
        if(!voice.silence_bytes)return fail();voice.stage=CAPTURE;
    }
    if(p->mode>=2)return repeated_audio(a,l,now);
    const struct bt_sco_packet *rx=bt_sco_receive(&voice.audio);
    if(rx){
        if(rx->link!=voice.stream)return fail();
        if(voice.stage!=CAPTURE){if(bt_sco_release_receive(&voice.audio,rx->serial)!=1)return fail();}
        else {
        if(voice.snapshot.records>=BLUETOOTH_PCM_RECORDS)return fail();
        unsigned remaining=p->samples*2-voice.snapshot.bytes;size_t used=rx->length<remaining?rx->length:remaining;
        voice.records[voice.snapshot.records++]=(struct bluetooth_pcm_record){rx->acquired,voice.snapshot.bytes,(uint16_t)rx->length,(uint16_t)used,rx->status,0};
        if(rx->status)++voice.snapshot.loss_packets;
        memcpy(voice.pcm+voice.snapshot.bytes,rx->bytes,used);voice.snapshot.bytes+=(uint32_t)used;
        if(bt_sco_release_receive(&voice.audio,rx->serial)!=1)return fail();
        }
    }
    if(voice.stage==CAPTURE){
        if(l->phase!=BT_SCO_OPEN)return fail();
        if(voice.snapshot.bytes==p->samples*2){
            /* Let the already accepted UART packet finish before disconnect;
             * no new application silence frame is admitted beyond this point. */
            if(voice.audio.active_tx<BT_SCO_PACKETS||a->hci.transport.tx_offset<a->hci.transport.tx_length)return 0;
            if(bt_sco_close(&voice.audio,voice.stream,stage_deadline(a,59500000))!=1)return fail();
            voice.operation_deadline=stage_deadline(a,59500000);voice.stage=CLOSE;return 0;
        }
        if(now>=voice.next_silence){uint64_t serial;
            int r=bt_sco_send(&voice.audio,voice.stream,voice.silence,voice.silence_bytes,voice.capture_deadline,&serial);
            if(r<0)return fail();if(r==1)voice.next_silence=cap(now,(voice.silence_bytes/2)*125,voice.capture_deadline);}
        return 0;
    }
    if(voice.stage==CLOSE&&l->phase==BT_SCO_CLOSED){
        if(voice.snapshot.loss_packets)return fail();
        voice.snapshot.firmware=a->firmware_generation;voice.snapshot.uart=a->uart_generation;
        voice.snapshot.acl=voice.link;voice.snapshot.audio=voice.stream;voice.snapshot.state=1;
        storage_digest_bytes(voice.pcm,voice.snapshot.bytes,voice.snapshot.sha256);
        voice.stage=DONE;atomic_store_explicit(&voice.terminal,1,memory_order_release);return 1;
    }
    return 0;
}

static int separate(const void *a,size_t an,const void *b,size_t bn)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&an<=UINTPTR_MAX-x&&bn<=UINTPTR_MAX-y&&(x+an<=y||y+bn<=x);}
int bluetooth_headset_copy(struct bluetooth_application *a,unsigned kind,uint32_t offset,size_t bytes,
                          uint8_t out[1024],size_t *copied,struct bluetooth_pcm_snapshot *snapshot)
{
    if(!a||a->self!=a||a->profile->version<2||kind>1||!bytes||bytes>1024||
        !separate(out,1024,copied,sizeof(*copied))||!separate(out,1024,snapshot,sizeof(*snapshot))||
        !separate(copied,sizeof(*copied),snapshot,sizeof(*snapshot)))return -1;
    const void *buffers[]={out,copied,snapshot};size_t sizes[]={1024,sizeof(*copied),sizeof(*snapshot)};
    for(unsigned i=0;i<3;++i)if(!separate(buffers[i],sizes[i],&voice,sizeof(voice))||
        !separate(buffers[i],sizes[i],a,sizeof(*a))||!separate(buffers[i],sizes[i],a->profile,sizeof(*a->profile))||
        !separate(buffers[i],sizes[i],a->shared,sizeof(*a->shared)))return -1;
    *copied=0;memset(snapshot,0,sizeof(*snapshot));
    if(atomic_load_explicit(&a->failed,memory_order_acquire)||
        (atomic_load_explicit(a->recovery_state,memory_order_acquire)&4))return -1;
    unsigned terminal=atomic_load_explicit(&voice.terminal,memory_order_acquire);
    if(terminal==2)return -1;if(!terminal)return 0;
    /* The producer publishes once and never mutates capture bytes afterward. */
    if(voice.app!=a)return -1;
    size_t total=kind?voice.snapshot.records*sizeof(struct bluetooth_pcm_record):voice.snapshot.bytes;
    if(offset>total||bytes>total-offset)return -1;
    memcpy(out,(kind?(const uint8_t *)voice.records:voice.pcm)+offset,bytes);
    *snapshot=voice.snapshot;*copied=bytes;
    return atomic_load_explicit(&a->failed,memory_order_acquire)||
        (atomic_load_explicit(a->recovery_state,memory_order_acquire)&4)?-1:1;
}

/* The input slots have one serialized Bluetooth producer and one bound
 * recognizer consumer. An unreturned slot is never written again. */
int bluetooth_headset_repeated(const struct bluetooth_application *a)
{return a&&a->self==a&&a->profile&&a->profile->version==3&&(a->profile->headset.mode==2||a->profile->headset.mode==3||a->profile->headset.mode==5); }
int bluetooth_headset_wideband(const struct bluetooth_application *a)
{return a&&a->self==a&&a->profile&&((a->profile->version==2&&a->profile->headset.mode==4)||(a->profile->version==3&&(a->profile->headset.mode==3||a->profile->headset.mode==5)));}
static int segment_live(struct bluetooth_application *a)
{return bluetooth_headset_repeated(a)&&voice.segment_app==a&&
    !atomic_load_explicit(&a->failed,memory_order_acquire)&&
    !(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4);}
int bluetooth_headset_segment_bind(struct bluetooth_application *a,void *consumer)
{
    if(!bluetooth_headset_repeated(a)||!consumer||voice.segment_consumer||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    voice.segment_app=a;voice.segment_consumer=consumer;return 1;
}
int bluetooth_headset_segment_stop(struct bluetooth_application *a)
{if(!segment_live(a))return -1;atomic_store_explicit(&voice.segment_stop,1,memory_order_release);return 1;}
int bluetooth_headset_segment_status(struct bluetooth_application *a,struct bluetooth_segment_status *out)
{
    if(!segment_live(a)||!out||!separate(out,sizeof(*out),&voice,sizeof(voice))||
       !separate(out,sizeof(*out),a,sizeof(*a))||!separate(out,sizeof(*out),a->profile,sizeof(*a->profile))||
       !separate(out,sizeof(*out),a->shared,sizeof(*a->shared)))return -1;
    /* Tail precedes head so concurrent publication/consumption cannot make the
     * observed consumer count exceed the observed producer count. */
    /* Acquire closure before its preceding publications, so a terminal empty
     * observation cannot omit the last accepted segment. */
    out->disconnected=atomic_load_explicit(&voice.segment_disconnected,memory_order_acquire);
    out->consumed=atomic_load_explicit(&voice.segment_tail,memory_order_acquire);
    out->published=atomic_load_explicit(&voice.segment_head,memory_order_acquire);
    out->flags=atomic_load_explicit(&voice.segment_flags,memory_order_acquire);
    return segment_live(a)?1:-1;
}
int bluetooth_headset_segment_copy(struct bluetooth_application *a,uint64_t ticket,uint32_t offset,size_t bytes,
    uint8_t out[1024],size_t *copied,struct bluetooth_segment_snapshot *snapshot)
{
    if(!segment_live(a)||!voice.segment_consumer||xTaskGetCurrentTaskHandle()!=voice.segment_consumer||
       !out||!copied||!snapshot||!bytes||bytes>1024||
       !separate(out,1024,copied,sizeof(*copied))||!separate(out,1024,snapshot,sizeof(*snapshot))||
       !separate(copied,sizeof(*copied),snapshot,sizeof(*snapshot)))return -1;
    const void *buffers[]={out,copied,snapshot};size_t sizes[]={1024,sizeof(*copied),sizeof(*snapshot)};
    for(unsigned i=0;i<3;++i)if(!separate(buffers[i],sizes[i],&voice,sizeof(voice))||
        !separate(buffers[i],sizes[i],a,sizeof(*a))||!separate(buffers[i],sizes[i],a->profile,sizeof(*a->profile))||
        !separate(buffers[i],sizes[i],a->shared,sizeof(*a->shared)))return -1;
    *copied=0;memset(snapshot,0,sizeof(*snapshot));
    uint64_t tail=atomic_load_explicit(&voice.segment_tail,memory_order_relaxed);
    uint64_t head=atomic_load_explicit(&voice.segment_head,memory_order_acquire);
    if(head<tail||head-tail>BLUETOOTH_SEGMENT_SLOTS)return -1;if(head==tail)return 0;
    const struct bluetooth_segment_snapshot *s=&voice.segments[tail%BLUETOOTH_SEGMENT_SLOTS].snapshot;
    if((ticket&&ticket!=tail+1)||s->ticket!=tail+1||offset>s->capture.bytes||bytes>s->capture.bytes-offset)return -1;
    memcpy(out,voice.segments[tail%BLUETOOTH_SEGMENT_SLOTS].pcm+offset,bytes);*copied=bytes;*snapshot=*s;
    return segment_live(a)?1:-1;
}
int bluetooth_headset_segment_ack(struct bluetooth_application *a,uint64_t ticket)
{
    if(!segment_live(a)||!voice.segment_consumer||xTaskGetCurrentTaskHandle()!=voice.segment_consumer)return -1;
    uint64_t tail=atomic_load_explicit(&voice.segment_tail,memory_order_relaxed);
    uint64_t head=atomic_load_explicit(&voice.segment_head,memory_order_acquire);
    if(!ticket||tail==UINT64_MAX||ticket!=tail+1||head<=tail||head-tail>BLUETOOTH_SEGMENT_SLOTS)return -1;
    atomic_store_explicit(&voice.segment_tail,tail+1,memory_order_release);return 1;
}
static void segment_stopping(unsigned flag)
{
    atomic_fetch_or_explicit(&voice.segment_flags,flag,memory_order_release);
    atomic_store_explicit(&voice.segment_stop,1,memory_order_release);
    voice.operation_deadline=voice.app->started+40000000;
}
static int repeated_audio(struct bluetooth_application *a,struct bt_sco_link *l,uint64_t now)
{
    const struct bluetooth_headset_profile *p=&a->profile->headset;
    if(voice.stage==CAPTURE){
        if(l->phase!=BT_SCO_OPEN)return fail();
        if(now>=a->started+38000000)segment_stopping(BLUETOOTH_SEGMENT_WINDOW);
        else if(p->mode!=4&&now>=voice.capture_deadline)segment_stopping(BLUETOOTH_SEGMENT_DEADLINE);
        if(atomic_load_explicit(&voice.segment_stop,memory_order_acquire)&&!voice.operation_deadline)
            segment_stopping(BLUETOOTH_SEGMENT_CANCEL);
    }
#ifdef SUNSTONE_BLUETOOTH_MSBC
    if(bluetooth_playback_mode(a)){
        if(voice.stage==CAPTURE&&bluetooth_playback_cancelled(a))segment_stopping(BLUETOOTH_SEGMENT_CANCEL);
        if(atomic_load_explicit(&voice.segment_stop,memory_order_acquire)&&!bluetooth_playback_done(a))bluetooth_playback_stop(a);
        if(bluetooth_playback_step(a,&voice.audio,voice.link,voice.stream,now,a->deadline)!=1)return fail();
    }
#endif
    const struct bt_sco_packet *rx=bt_sco_receive(&voice.audio);
    if(rx){
        if(rx->link!=voice.stream)return fail();
        if(voice.partial_serial&&voice.partial_serial!=rx->serial)return fail();
        #ifdef SUNSTONE_BLUETOOTH_MSBC
        if(p->mode==4){if(bluetooth_playback_discard(a,rx->length,rx->status)!=1)return fail();voice.partial_offset=rx->length;}
        else
#endif
        if(voice.stage==CAPTURE&&!atomic_load_explicit(&voice.segment_stop,memory_order_acquire)){
            voice.partial_serial=rx->serial;
            if(rx->status)segment_stopping(BLUETOOTH_SEGMENT_LOSS);
            while(voice.partial_offset<rx->length&&!atomic_load_explicit(&voice.segment_stop,memory_order_acquire)){
                uint64_t head=atomic_load_explicit(&voice.segment_head,memory_order_relaxed);
                uint64_t tail=atomic_load_explicit(&voice.segment_tail,memory_order_acquire);
                if(head<tail||head-tail>BLUETOOTH_SEGMENT_SLOTS||head>=UINT64_MAX/p->samples)return fail();
                if(head-tail==BLUETOOTH_SEGMENT_SLOTS||voice.segment_records==BLUETOOTH_PCM_RECORDS){
                    segment_stopping(BLUETOOTH_SEGMENT_BACKLOG);break;}
                unsigned slot=(unsigned)(head%BLUETOOTH_SEGMENT_SLOTS);
                struct bluetooth_segment_snapshot *s=&voice.segments[slot].snapshot;
                if(!voice.segment_bytes){memset(s,0,sizeof(*s));s->ticket=head+1;s->first_sample=head*p->samples;s->first_acquired=rx->acquired;}
#ifdef SUNSTONE_BLUETOOTH_MSBC
                if(l->codec==2){
                    uint8_t decoded[240];uint64_t first=0;
                    int result=bluetooth_msbc_byte(rx->bytes[voice.partial_offset++],rx->acquired,decoded,&first);
                    if(result<0){segment_stopping(BLUETOOTH_SEGMENT_LOSS);break;}
                    if(!result)continue;
                    if(!voice.segment_bytes)s->first_acquired=first;
                    if(p->samples*2-voice.segment_bytes<sizeof(decoded))return fail();
                    voice.segments[slot].records[voice.segment_records++]=(struct bluetooth_pcm_record){first,
                        voice.segment_bytes,60,240,0,0};
                    memcpy(voice.segments[slot].pcm+voice.segment_bytes,decoded,sizeof(decoded));
                    voice.segment_bytes+=sizeof(decoded);s->last_acquired=rx->acquired;
                }else
#endif
                {
                unsigned remaining=p->samples*2-voice.segment_bytes;
                size_t used=rx->length-voice.partial_offset;if(used>remaining)used=remaining;
                voice.segments[slot].records[voice.segment_records++]=(struct bluetooth_pcm_record){rx->acquired,
                    voice.segment_bytes,(uint16_t)rx->length,(uint16_t)used,rx->status,0};
                memcpy(voice.segments[slot].pcm+voice.segment_bytes,rx->bytes+voice.partial_offset,used);
                voice.segment_bytes+=(unsigned)used;voice.partial_offset+=(unsigned)used;s->last_acquired=rx->acquired;
                }
                if(voice.segment_bytes==p->samples*2){
                    s->capture=(struct bluetooth_pcm_snapshot){.firmware=a->firmware_generation,.uart=a->uart_generation,
                        .acl=voice.link,.audio=voice.stream,.state=l->codec,.bytes=voice.segment_bytes,.records=voice.segment_records};
                    storage_digest_bytes(voice.segments[slot].pcm,voice.segment_bytes,s->capture.sha256);
                    atomic_store_explicit(&voice.segment_head,head+1,memory_order_release);
                    voice.segment_bytes=voice.segment_records=0;
                    voice.capture_deadline=cap(now,p->capture_timeout_us,a->started+38000000);
                }
            }
        }
        /* Closing explicitly abandons unpublished packet tails. Accepted input
         * slots stay immutable; only the exact currently borrowed RX is freed. */
        if(voice.partial_offset==rx->length||voice.stage!=CAPTURE||atomic_load_explicit(&voice.segment_stop,memory_order_acquire)){
            if(bt_sco_release_receive(&voice.audio,rx->serial)!=1)return fail();
            voice.partial_serial=0;voice.partial_offset=0;
        }
    }
    if(voice.stage==CAPTURE){
#ifdef SUNSTONE_BLUETOOTH_MSBC
        if(bluetooth_playback_mode(a)){
            if(atomic_load_explicit(&voice.segment_stop,memory_order_acquire)&&!bluetooth_playback_done(a))bluetooth_playback_stop(a);
            if(bluetooth_playback_done(a))segment_stopping(BLUETOOTH_SEGMENT_WINDOW);
        }
#endif
        if(atomic_load_explicit(&voice.segment_stop,memory_order_acquire)){
            if(voice.audio.active_tx<BT_SCO_PACKETS||a->hci.transport.tx_offset<a->hci.transport.tx_length)return 0;
            if(bt_sco_close(&voice.audio,voice.stream,a->started+40000000)!=1)return fail();voice.stage=CLOSE;return 0;
        }
#ifdef SUNSTONE_BLUETOOTH_MSBC
        if(l->codec==2){
            if(p->mode>=4)return 0;
            if(now>=voice.next_silence){
                if(!voice.silence_ready){if(bluetooth_msbc_silence(voice.silence)!=1)return fail();voice.silence_ready=1;}
                unsigned bytes=60-voice.silence_offset;if(bytes>voice.audio.mtu)bytes=voice.audio.mtu;
                uint64_t serial;int r=bt_sco_send(&voice.audio,voice.stream,voice.silence+voice.silence_offset,bytes,voice.capture_deadline,&serial);
                if(r<0)return fail();if(r==1){voice.silence_offset+=bytes;
                    if(voice.silence_offset==60){voice.silence_offset=0;voice.silence_ready=0;voice.next_silence=cap(now,7500,voice.capture_deadline);}}
            }
        }else
#endif
        if(now>=voice.next_silence){uint64_t serial;
            int r=bt_sco_send(&voice.audio,voice.stream,voice.silence,voice.silence_bytes,voice.capture_deadline,&serial);
            if(r<0)return fail();if(r==1)voice.next_silence=cap(now,(voice.silence_bytes/2)*125,voice.capture_deadline);}
        return 0;
    }
    if(voice.stage==CLOSE&&l->phase==BT_SCO_CLOSED){
        voice.stage=DONE;voice.operation_deadline=0;
#ifdef SUNSTONE_BLUETOOTH_MSBC
        if(bluetooth_playback_mode(a))bluetooth_playback_closed(a);
#endif
        atomic_store_explicit(&voice.segment_disconnected,1,memory_order_release);return 1;
    }
    /* DONE still reaches control_step, bt_sco_step and security_service in the
     * owner poll. Revocation remains observable through the output lifetime. */
    return voice.stage==DONE?1:0;
}
