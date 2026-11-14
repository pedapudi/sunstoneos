#include "headset.h"
#ifdef SUNSTONE_BLUETOOTH_A2DP
#include "a2dp.h"
#endif
#include "bluetooth.h"
#include "bt_security.h"
#include "bt_sdp_server.h"
#include "bt_sco.h"
#include "bt_memory.h"
#include "a733_storage_digest.h"
#include <string.h>
#ifdef SUNSTONE_BLUETOOTH_MSBC
#include "msbc.h"
#include "playback.h"
#endif
/* IDLE waits for a recognition activation with the AG ready and the codec
 * selected; voice recognition opens the audio link from there (FALLBACK,
 * AUDIO), captures one segment, then closes it before returning to IDLE. */
enum phase {CONFIGURE,CONFIGURING,SECURITY_CONFIG,CONNECT,AUTHENTICATE,SDP,CONTROL,AG,IDLE,FALLBACK,AUDIO,CAPTURE,CLOSE,DONE};
/* The owner's per-poll scalars come first, so one base register reaches
 * them within a cache line or two; the words shared with the consumer follow
 * on their own cache line. Voice recognition: the AG advertises it (recognition),
 * the AG first reached idle (idled), an activation runs (active), started
 * with +BVRA (bvra), +BVRA: 0 is owed (voice_off), the bytes an activation
 * skips to the next sample or frame boundary, the bytes released since the
 * link opened (position), the stream position after the last decoded mSBC
 * frame (boundary), the capture window and the end of AT+BVRA=1 and START
 * admission (admit_end). stop is the owner's copy of segment_stop, loaded once
 * per poll and set by the owner's own stops. */
struct bluetooth_headset_state {
    unsigned stage,stop,idled,recognition,active,bvra,voice_off,skip,idle_close;
    unsigned partial_offset,segment_bytes,segment_records,hashed;
    unsigned silence_bytes,silence_offset,silence_ready,wideband,auth_started,configured;
    uint64_t window,admit_end,capture_deadline,next_silence,operation_deadline,link,stream,last_owner;
    uint64_t position;
#ifdef SUNSTONE_BLUETOOTH_MSBC
    uint64_t boundary;
#endif
    uint64_t partial_serial,published_samples,silence_restarts;
    struct bluetooth_application *app,*segment_app;
    void *segment_consumer;
    _Alignas(64) atomic_ullong segment_head,segment_tail;
    atomic_uint segment_stop,segment_flags,segment_disconnected,segment_request,terminal;
    _Alignas(64) struct bt_hci_incoming_config incoming;
    struct bt_acl acl;
    struct bt_security security;
    struct bt_l2cap l2cap;
    struct bt_rfcomm_registry registry;
    struct bt_sdp_server server;
    struct bt_sdp discovery;
    struct bt_rfcomm rfcomm;
    struct bt_hfp_ag ag;
    #ifdef SUNSTONE_BLUETOOTH_A2DP
    struct bt_avdtp sink;
    #endif
    struct bt_sco audio;
    struct bt_sco_binding audio_binding;
    struct storage_digest digest;
    struct bluetooth_pcm_snapshot snapshot;
#ifdef SUNSTONE_BLUETOOTH_A2DP
    struct bluetooth_pcm_record records[1500];
#else
    struct bluetooth_pcm_record records[BLUETOOTH_PCM_RECORDS];
#endif
    uint8_t pcm[BLUETOOTH_PCM_BYTES],silence[254];
    struct {
        struct bluetooth_segment_snapshot snapshot;
        struct bluetooth_pcm_record records[BLUETOOTH_PCM_RECORDS];
        uint8_t pcm[BLUETOOTH_PCM_BYTES];
    } segments[BLUETOOTH_SEGMENT_SLOTS];
};
/* voice names the state. The OS gives its memory at the grant (bluetooth_headset_memory); the Bluetooth image keeps it static. */
#ifdef SUNSTONE_A733_OS
static struct bluetooth_headset_state *voice_state;
const size_t bluetooth_headset_state_bytes=sizeof(struct bluetooth_headset_state);
int bluetooth_headset_memory(void *memory)
{
    if(voice_state||!memory||(uintptr_t)memory%_Alignof(struct bluetooth_headset_state))return 0;
    voice_state=memory;return 1;
}
#else
static struct bluetooth_headset_state voice_storage,*const voice_state=&voice_storage;
#endif
#define voice (*voice_state)
enum stop_phase {STOP_OFF,STOP_SCAN,STOP_VOICE,STOP_AUDIO,STOP_CONTROL,STOP_SDP,STOP_SERVER,STOP_CHANNELS,STOP_ACL,STOP_DONE,STOP_FAULT};
static struct {struct bluetooth_application *app;uint64_t stop_deadline,close_deadline;unsigned stop_phase;} context;
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
#ifdef SUNSTONE_BLUETOOTH_A2DP
    return psm==1||psm==BT_AVDTP_PSM;
#else
    return psm==1||psm==3;
#endif
}
static int psm(void *ctx,uint64_t link,uint16_t value,unsigned incoming)
{if(context.stop_phase&&incoming)return 0;
 if(value==1&&incoming)return bt_sdp_server_admit(&voice.server,link,value,incoming);
#ifdef SUNSTONE_BLUETOOTH_A2DP
 return bt_avdtp_admit(&voice.sink,link,value,incoming);
#else
 return policy(ctx,link,value,incoming);
#endif
}
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
uint64_t bluetooth_headset_next_deadline(const struct bluetooth_application *a,uint64_t until)
{
    if(!a||a->self!=a||!until||xTaskGetCurrentTaskHandle()!=a->owner)return 0;
    /* The OS admits capture modes 2 and 3. Playback and A2DP have different
     * producer cadences and require their own event-owner integration. */
    if(a->profile->headset.mode>=4)return 0;
    if(!voice_state||voice.app!=a)return until;
    if(voice.stage==DONE)return until;
#define EARLIER(value) do{uint64_t at=(value);if(at&&at<until)until=at;}while(0)
    EARLIER(voice.operation_deadline);
    if(context.stop_phase&&context.stop_phase!=STOP_DONE&&context.stop_phase!=STOP_FAULT){
        EARLIER(context.stop_deadline);EARLIER(context.close_deadline);
    }
    if(voice.stage>=IDLE&&voice.stage<CLOSE&&!voice.stop)EARLIER(voice.window);
    if(voice.stage==CAPTURE){
        EARLIER(voice.capture_deadline);
        if(!voice.stop&&!voice.idle_close){
            /* A full TX pool resumes on controller credit retirement. A due
             * cadence alone cannot make progress while every slot is held. */
            for(unsigned i=0;i<BT_SCO_PACKETS;++i)if(voice.audio.tx[i].state==BT_SCO_FREE){
                EARLIER(voice.next_silence);break;
            }
        }
    }
    const struct bt_security *security=&voice.security;
    if(security->self){EARLIER(security->deadline);if(security->pending)EARLIER(security->command_deadline);}
    const struct bt_acl *acl=&voice.acl;
    if(acl->state==BT_ACL_ACTIVE){
        EARLIER(acl->return_deadline);
        for(unsigned i=0;i<BT_ACL_LINKS;++i){
            const struct bt_acl_link *link=&acl->links[i];
            if(link->state!=BT_ACL_UNUSED&&link->state!=BT_ACL_CONNECTED&&link->state!=BT_ACL_CLOSED)
                EARLIER(link->deadline);
        }
        for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i)
            if(acl->packets[i].state>=BT_ACL_QUEUED&&acl->packets[i].state<=BT_ACL_SENT)
                EARLIER(acl->packets[i].deadline);
    }
    const struct bt_l2cap *l2cap=&voice.l2cap;
    if(l2cap->state==BT_L2CAP_ACTIVE){
        for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i){
            const struct bt_l2cap_channel *channel=&l2cap->channels[i];
            if(channel->state==BT_L2CAP_CONNECTING||channel->state==BT_L2CAP_CONFIGURING||channel->state==BT_L2CAP_DISCONNECTING){
                EARLIER(channel->deadline);
                if(channel->timer!=BT_L2CAP_TIMER_NONE)EARLIER(channel->timer_deadline);
            }
        }
        for(unsigned i=0;i<BT_L2CAP_PDUS;++i){
            if(l2cap->tx[i].state==BT_L2CAP_TRANSMITTING)EARLIER(l2cap->tx[i].deadline);
            if(l2cap->rx[i].state==BT_L2CAP_PARTIAL||l2cap->rx[i].state==BT_L2CAP_SIGNAL)EARLIER(l2cap->rx[i].deadline);
        }
    }
    const struct bt_sdp *sdp=&voice.discovery;
    if(sdp->phase!=BT_SDP_OFF&&sdp->phase!=BT_SDP_CLOSED){
        if(sdp->status==BT_SDP_PENDING)EARLIER(sdp->deadline);
        if(sdp->phase==BT_SDP_CLOSING)EARLIER(sdp->close_deadline);
    }
    if(voice.server.initialized)for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i){
        const struct bt_sdp_server_session *session=&voice.server.sessions[i];
        if(!session->channel||session->error)continue;
        if(session->tx_serial||session->pending_length)EARLIER(session->deadline);
        if(session->closing&&!session->close_started)EARLIER(session->close_deadline);
    }
    const struct bt_rfcomm *rfcomm=&voice.rfcomm;
    if(rfcomm->phase!=BT_RFCOMM_OFF&&rfcomm->phase!=BT_RFCOMM_CLOSED&&rfcomm->phase!=BT_RFCOMM_FAULT){
        if(rfcomm->phase==BT_RFCOMM_OPENING||rfcomm->phase==BT_RFCOMM_NEGOTIATING||
           (rfcomm->incoming&&!rfcomm->channel))EARLIER(rfcomm->deadline);
        if(rfcomm->phase==BT_RFCOMM_CLOSING)EARLIER(rfcomm->close_deadline);
        EARLIER(rfcomm->timer);
        for(unsigned i=0;i<BT_RFCOMM_SLOTS;++i)if(rfcomm->tx[i].state==BT_RFCOMM_QUEUED)EARLIER(rfcomm->tx[i].deadline);
    }
    const struct bt_hfp_ag *ag=&voice.ag;
    if(ag->phase!=BT_HFP_AG_OFF&&ag->phase!=BT_HFP_AG_CLOSED&&ag->phase!=BT_HFP_AG_CLOSING&&ag->phase!=BT_HFP_AG_FAULT){
        if(ag->phase==BT_HFP_AG_INITIALIZING)EARLIER(ag->setup_deadline);
        EARLIER(ag->codec_deadline);EARLIER(ag->line_deadline);
        if(ag->response_length&&!ag->response_accepted)EARLIER(ag->response_deadline);
#ifdef SUNSTONE_BT_HFP_CALL_CONTROL
        if(ag->calls&&ag->control.call.setup==1&&!ag->response_length&&!ag->line_deadline)EARLIER(ag->next_ring);
#endif
    }
    const struct bt_sco *audio=&voice.audio;
    if(audio->initialized){
        EARLIER(audio->stop_deadline);if(audio->refuse)EARLIER(audio->refuse_deadline);
        for(unsigned i=0;i<BT_SCO_LINKS;++i)
            if(audio->links[i].phase!=BT_SCO_UNUSED&&audio->links[i].phase!=BT_SCO_OPEN&&audio->links[i].phase!=BT_SCO_CLOSED)
                EARLIER(audio->links[i].deadline);
        for(unsigned i=0;i<BT_SCO_PACKETS;++i)
            if(audio->tx[i].state==BT_SCO_QUEUED||audio->tx[i].state==BT_SCO_PARTIAL||audio->tx[i].state==BT_SCO_SENT)
                EARLIER(audio->tx[i].deadline);
    }
#undef EARLIER
    return until;
}
/* Failure, stop, wake-up, publication, activation start and end, the
 * in-segment write and consumer admission are kept out of line: each runs at
 * most once per segment, packet or call, and inlining them into the per-poll
 * path costs code size and callee-saved registers on every poll. */
static __attribute__((noinline)) int fail(void){return bluetooth_headset_retain(voice.app);}
static uint64_t cap(uint64_t now,uint64_t duration,uint64_t limit)
{uint64_t end=a733_deadline_after_saturating(now,duration);return end<limit?end:limit;}
static uint64_t stage_deadline(const struct bluetooth_application *a,uint64_t duration)
{
    /* Recognition reserves the final20s of the existing60s supervised session.
     * This allocation is not a physical inference-time qualification. */
    if((a->profile->version>=3||a->profile->headset.mode==4)&&duration>40000000)duration=40000000;
    if(a->resident){
        uint64_t firmware=(a->profile->version>=3||a->profile->headset.mode==4)?30000000:40000000;
        return cap(a->started,duration-firmware,a->deadline);
    }
    return a->started+duration;
}
static int upper_init(struct bluetooth_application *a)
{
    const struct bluetooth_headset_profile *p=&a->profile->headset;
    struct bt_security_policy sec={.allow_just_works=p->allow_just_works,.minimum_level=p->minimum_security,.deadline=a->deadline};
    memcpy(sec.peer,p->peer,6);memcpy(sec.pin,p->pin,17);
    voice.wideband=p->mode>=3&&(a->hci.identity.features[2]&8)&&(a->hci.identity.features[3]&0x80);
    /* Mode5 keeps continuous capture for its caller playback. */
    voice.recognition=bluetooth_headset_repeated(a)&&p->mode!=5;
    unsigned features=(voice.wideband?512u:0u)|(voice.recognition?BT_HFP_AG_VOICE:0u);
#ifdef SUNSTONE_BT_HFP_CALL_CONTROL
    /* With no telephony every call indicator stays zero and dialing is refused. */
    features|=BT_HFP_AG_CALL_FEATURES;
#endif
    struct bt_sdp_ag_profile ag={.handle=0x10000,.version=0x0109,.rfcomm_channel=(uint8_t)p->server_channel,
        .brsf_features=features,.supported_features=(uint16_t)((voice.wideband?32u:0u)|(voice.recognition?4u:0u))};
#ifdef SUNSTONE_BT_HFP_CALL_CONTROL
    /* HFP section 6.3: Network is the ability to reject a call. */
    ag.network=1;
#endif
    voice.audio_binding=(struct bt_sco_binding){p->scheduling_authority,p->receive_latency_us,p->response_timeout_us,audio_current,audio_security,&context};
#ifdef SUNSTONE_BLUETOOTH_A2DP
    ag=(struct bt_sdp_ag_profile){.sink_handle=0x10001,.sink_features=4};
    static const uint8_t capabilities[4]={0x3f,0xff,2,53};
    if(bt_avdtp_init(&voice.sink,&voice.l2cap,capabilities,p->response_timeout_us,policy,&context)!=1)return fail();
#endif
    if(bt_acl_init(&voice.acl,&a->hci)!=1||bt_security_init(&voice.security,&voice.acl,a->owner,a->recovery_state,&sec,a->profile->command_timeout_us)!=1||
        bt_rfcomm_registry_init(&voice.registry,&voice.l2cap,psm,service,&context)!=1||
        bt_sdp_server_init(&voice.server,&voice.l2cap,&ag,1000000,2000000,policy,&context)!=1||
        bt_l2cap_init_bootstrap(&voice.l2cap,&voice.acl,p->mtu,1000000,60000000,bt_rfcomm_admit,&voice.registry,&a->hci.transport.binding)!=1)return fail();
#ifndef SUNSTONE_BLUETOOTH_A2DP
    if(bt_sco_init(&voice.audio,&voice.acl,&voice.audio_binding)!=1)return fail();
#endif
    return 1;
}
/* Reserve the profile peer's next ACL generation and attach it to security. */
static int reserve(struct bluetooth_application *a)
{
    const struct bluetooth_headset_profile *p=&a->profile->headset;int result;
    if(p->incoming)result=bt_acl_listen(&voice.acl,p->peer,voice.operation_deadline,&voice.link);
    else {struct bt_acl_peer peer={.page_scan_mode=p->peer_page_scan_mode,
        .role_switch=p->peer_role_switch,.clock_offset=p->peer_clock_offset};memcpy(peer.address,p->peer,6);
        result=bt_acl_connect(&voice.acl,&peer,voice.operation_deadline,&voice.link);}
    return result==1&&bt_security_attach(&voice.security,voice.link)==1?1:fail();
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
static int recognition(struct bluetooth_application *,uint64_t,unsigned);
static void voice_admission(uint64_t,unsigned);
static void idle(const struct bluetooth_application *);
static int finished(void);
#ifdef SUNSTONE_BLUETOOTH_A2DP
static int a2dp_poll(struct bluetooth_application *a,uint64_t now)
{
    if(voice.stage==DONE)return 1;
    if(bt_sdp_server_step(&voice.server,64,a->deadline)!=1||
       bt_avdtp_step(&voice.sink,64,a->deadline)<0||bt_security_service(&voice.security,a->deadline)!=1)return fail();
    if(voice.stage==CLOSE){
        if(voice.sink.signal||voice.sink.media)return 0;
        voice.snapshot.firmware=a->firmware_generation;voice.snapshot.uart=a->uart_generation;
        voice.snapshot.acl=voice.link;voice.snapshot.state=3;
        storage_digest_bytes(voice.pcm,voice.snapshot.bytes,voice.snapshot.sha256);
        voice.stage=DONE;atomic_store_explicit(&voice.terminal,1,memory_order_release);return 1;
    }
    if(now>=voice.capture_deadline)return fail();
    if(voice.snapshot.records>=1500)return fail();
    if(bluetooth_a2dp_decode(&voice.sink,voice.pcm,a->profile->headset.samples*2,
        &voice.snapshot,&voice.records[voice.snapshot.records],now)<0)return fail();
    if(voice.snapshot.bytes==a->profile->headset.samples*2){
        voice.operation_deadline=stage_deadline(a,59500000);
        if(bt_avdtp_close(&voice.sink,voice.operation_deadline)!=1)return fail();
        voice.stage=CLOSE;
    }
    return 0;
}
#endif
static int receive_timely(const struct bluetooth_application *a,uint64_t now)
{
    return now>=voice.last_owner&&(a->uart.config.deferred_irq||
        now-voice.last_owner<=a->profile->headset.receive_latency_us);
}
int bluetooth_headset_poll(struct bluetooth_application *a,uint64_t now)
{
    if(!a||a->profile->version<2||!a->iteration_live||xTaskGetCurrentTaskHandle()!=a->owner)return -1;
    if(context.stop_phase)return bluetooth_headset_stop(a,now,context.stop_deadline);
    if(!voice.app){voice.app=a;context.app=a;voice.last_owner=now;}
#ifdef SUNSTONE_BLUETOOTH_A2DP
    if(atomic_load_explicit(&voice.terminal,memory_order_acquire))return live(a)?1:-1;
#endif
    /* A deferred owner can sleep indefinitely without traffic. Its binding
     * records the earliest pending IRQ and enforces IRQ-to-service latency
     * before this call. Polling mode instead bounds the gap between passes. */
    if(!live(a)||!receive_timely(a,now))return fail();
    voice.last_owner=now;
#ifdef SUNSTONE_BLUETOOTH_MSBC
    /* No established stream exists to close yet. Stop setup and retain any
     * in-flight command/UART ownership; cancellation is not a disconnect. */
    if(bluetooth_playback_mode(a)&&voice.stage<=AUDIO&&bluetooth_playback_cancelled(a))return fail();
#endif
    if((a->profile->headset.mode<2||a->profile->headset.mode==6)&&atomic_load_explicit(&voice.terminal,memory_order_acquire))return 1;
    voice.stop=atomic_load_explicit(&voice.segment_stop,memory_order_acquire);
    /* Before the AG first reaches idle a stop retains the transports: setup
     * commands may be in flight. After it, every stage through CAPTURE,
     * including the AG and FALLBACK stages that open the link, handles voice
     * recognition and stops like idle. */
    if(voice.stop&&voice.stage<IDLE&&!voice.idled)return fail();
    const unsigned idled=voice.idled;
    unsigned word=0;
    const struct bluetooth_headset_profile *p=&a->profile->headset;
    if(voice.operation_deadline&&now>=voice.operation_deadline)return fail();
#ifdef SUNSTONE_BLUETOOTH_A2DP
    if(voice.stage>=CAPTURE)return a2dp_poll(a,now);
#endif
    if(voice.stage==CONFIGURE){
        voice.incoming=(struct bt_hci_incoming_config){.accept_timeout_slots=p->accept_slots,
            .page_interval_slots=p->page_interval,.page_window_slots=p->page_window,
            .service_generation=p->scheduling_authority,.receive_latency_us=p->receive_latency_us,.synchronous_audio=1,
            .current=scheduling,.context=&context};
#ifdef SUNSTONE_BLUETOOTH_A2DP
        voice.incoming.synchronous_audio=0;
#endif
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
        if(p->incoming&&bt_acl_incoming(&voice.acl,p->response_timeout_us)!=1)return fail();
        if(reserve(a)!=1)return -1;
        voice.stage=CONNECT;return 0;
    }
    if(voice.stage==CONNECT||voice.stage==AUTHENTICATE){
        if(bt_security_step(&voice.security,64,voice.operation_deadline)!=1)return fail();
        struct bt_acl_link *l=link();if(!l)return fail();
        if(l->state==BT_ACL_CLOSED){
            /* An attempt that closed before any handle existed, such as a page
             * timeout, is retried under a fresh generation until the stage
             * deadline. Each retry retains one security entry, which bounds
             * them; a link that ever connected still ends the session. */
            if(voice.stage!=CONNECT||l->handle_valid)return fail();
            if(voice.security.pending||voice.security.count)return 0;
            return reserve(a)==1?0:-1;
        }
        if(voice.stage==CONNECT){
            if(l->state!=BT_ACL_CONNECTED)return 0;
            int r=bt_security_start(&voice.security,voice.link);if(r<0)return fail();if(!r)return 0;
            voice.auth_started=1;voice.stage=AUTHENTICATE;
        }
        uint64_t generation;unsigned sc;int r=security(voice.link,&generation,&sc);if(r<0)return fail();if(!r)return 0;
#ifdef SUNSTONE_BLUETOOTH_A2DP
        voice.capture_deadline=cap(now,p->capture_timeout_us,stage_deadline(a,59000000));
        voice.operation_deadline=voice.capture_deadline;voice.stage=CAPTURE;return 0;
#endif
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
    if(idled){word=atomic_load_explicit(&voice.segment_request,memory_order_acquire);voice_admission(now,word);}
    if(control_step(a)!=1)return fail();
    if(voice.stage==CONTROL){
        if(voice.rfcomm.phase!=BT_RFCOMM_OPEN)return 0;
        static const uint8_t no_telephony[7]={0};
#ifdef SUNSTONE_BT_HFP_CALL_CONTROL
        if(bt_hfp_ag_init_calls(&voice.ag,&voice.rfcomm,no_telephony,voice.operation_deadline,p->line_timeout_us,
            voice.wideband?6:0,voice.recognition,4000000)!=1)return fail();
#else
        if(bt_hfp_ag_init(&voice.ag,&voice.rfcomm,no_telephony,voice.operation_deadline,p->line_timeout_us,
            voice.wideband?6:0,voice.recognition)!=1)return fail();
#endif
        voice.stage=AG;return 0;
    }
    if(idled&&voice.stage<=CAPTURE){int r=recognition(a,now,word);if(r!=1)return r;}
    if(voice.stage==AG||voice.stage==FALLBACK){
        if(voice.ag.phase!=BT_HFP_AG_READY)return 0;
        if(voice.wideband&&(voice.ag.hf_features&128)){
            /* HFP 4.11.3 permits the selected codec on the same service link.
             * AT+BAC invalidates it when fresh negotiation is required. */
            unsigned desired=voice.stage==FALLBACK?1:voice.ag.selected_codec?voice.ag.selected_codec:(voice.ag.peer_codecs&4)?2:1;
            if(!voice.ag.selected_codec||voice.ag.selected_codec!=desired){
                if(!voice.ag.offered_codec){int selected=bt_hfp_ag_select_codec(&voice.ag,desired);if(selected<0)return fail();}
                return 0;
            }
        }
        if(p->mode>=4&&voice.ag.selected_codec!=2)return fail();
        /* Voice recognition waits idle for an activation, which then bounds
         * the link open and its CVSD retry. */
        if(voice.recognition&&voice.stage==AG&&!voice.active){idle(a);return 0;}
        if(!voice.recognition)voice.operation_deadline=stage_deadline(a,58000000);
        if(voice.stage==FALLBACK){if(bt_sco_retry_cvsd(&voice.audio,voice.stream,voice.operation_deadline,&voice.stream)!=1)return fail();}
        else if(voice.recognition&&voice.stream){
            int reopened=bt_sco_reopen(&voice.audio,voice.stream,voice.operation_deadline,&voice.stream);
            if(reopened<0)return fail();if(!reopened)return 0;
        }else if(bt_sco_open(&voice.audio,&voice.ag,0,voice.operation_deadline,&voice.stream)!=1)return fail();
        voice.stage=AUDIO;return 0;
    }
    if(bt_sco_step(&voice.audio,64,a->deadline)!=1||bt_security_service(&voice.security,a->deadline)!=1)return fail();
    const struct bt_sco_packet *tx=bt_sco_result(&voice.audio);
#ifdef SUNSTONE_BLUETOOTH_MSBC
    if(!bluetooth_playback_mode(a))
#endif
    if(tx&&bt_sco_release_result(&voice.audio,tx->serial)!=1)return fail();
    /* Idle, and done without ever opening a link, have no stream. */
    if(voice.stage==IDLE||(voice.stage==DONE&&!voice.stream))return voice.stage==DONE;
    struct bt_sco_link *l=stream();if(!l)return fail();
    if(voice.stage==AUDIO){
        if(p->mode==3&&l->phase==BT_SCO_CLOSED&&l->codec==2&&l->fallback_allowed&&!voice.stop){voice.stage=FALLBACK;return 0;}
        /* A stop during an open waits for the result; a confirmed setup
         * failure then ends cleanly. */
        if(voice.recognition&&voice.stop&&l->phase==BT_SCO_CLOSED)return finished();
        if(l->phase==BT_SCO_CLOSED||l->phase==BT_SCO_RETAINED)return fail();
        if(l->phase!=BT_SCO_OPEN)return 0;
        if(p->mode>=4&&l->codec!=2)return fail();
        /* Voice recognition armed the capture deadline at activation start. */
        voice.window=bluetooth_capture_window(a);
        if(!voice.recognition)voice.capture_deadline=cap(now,p->capture_timeout_us,stage_deadline(a,59000000));
        voice.operation_deadline=p->mode>=2?0:voice.capture_deadline;voice.next_silence=now;
        voice.position=voice.skip=0;
        voice.silence_offset=voice.silence_ready=0;
#ifdef SUNSTONE_BLUETOOTH_MSBC
        voice.boundary=0;
#endif
        if(p->mode==5){voice.active=1;storage_digest_init(&voice.digest);}
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
            if(r<0)return fail();
            if(r==1)voice.next_silence=bluetooth_cadence(voice.next_silence,now,(voice.silence_bytes/2)*125,voice.capture_deadline,&voice.silence_restarts);}
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

int bluetooth_headset_copy(struct bluetooth_application *a,unsigned kind,uint32_t offset,size_t bytes,
                          uint8_t out[1024],size_t *copied,struct bluetooth_pcm_snapshot *snapshot)
{
    if(!a||a->self!=a||a->profile->version<2||kind>1||!bytes||bytes>1024||
        !bt_range_disjoint(out,1024,copied,sizeof(*copied))||!bt_range_disjoint(out,1024,snapshot,sizeof(*snapshot))||
        !bt_range_disjoint(copied,sizeof(*copied),snapshot,sizeof(*snapshot)))return -1;
    const void *buffers[]={out,copied,snapshot};size_t sizes[]={1024,sizeof(*copied),sizeof(*snapshot)};
    for(unsigned i=0;i<3;++i)if(!bt_range_disjoint(buffers[i],sizes[i],&voice,sizeof(voice))||
        !bt_range_disjoint(buffers[i],sizes[i],a,sizeof(*a))||!bt_range_disjoint(buffers[i],sizes[i],a->profile,sizeof(*a->profile))||
        !bt_range_disjoint(buffers[i],sizes[i],a->shared,sizeof(*a->shared)))return -1;
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
 * recognizer consumer. A slot is written only while head-tail<2, and tail
 * advances only at the acknowledgement, so a borrowed slot never changes. */
int bluetooth_headset_repeated(const struct bluetooth_application *a)
{return a&&a->self==a&&a->profile&&a->profile->version>=3&&(a->profile->headset.mode==2||a->profile->headset.mode==3||a->profile->headset.mode==5); }
/* The snapshot is an owner-only comparison vector, not a device operation. */
int bluetooth_headset_work_state(const struct bluetooth_application *a,struct bluetooth_work_state *out)
{
    if(!a||a->self!=a||!a->profile||!out||xTaskGetCurrentTaskHandle()!=a->owner||
       (voice_state&&voice.app&&voice.app!=a)||
       (a->profile->version>=2&&a->profile->headset.mode>=4)||
       !bt_range_disjoint(out,sizeof(*out),&context,sizeof(context))||
       (voice_state&&!bt_range_disjoint(out,sizeof(*out),voice_state,sizeof(*voice_state))))return 0;
    memset(out,0,sizeof(*out));
    if(!voice_state)return 1;
#define TAKE(name) target->name=source->name;
    {struct bluetooth_work_headset *target=&out->headset;const struct bluetooth_headset_state *source=&voice;BT_WORK_HEADSET(TAKE)}
    {struct bluetooth_work_security *target=&out->security;const struct bt_security *source=&voice.security;BT_WORK_SECURITY(TAKE)}
    {struct bluetooth_work_acl *target=&out->acl;const struct bt_acl *source=&voice.acl;BT_WORK_ACL(TAKE)}
    {struct bluetooth_work_l2cap *target=&out->l2cap;const struct bt_l2cap *source=&voice.l2cap;BT_WORK_L2CAP(TAKE)}
    {struct bluetooth_work_rfcomm *target=&out->rfcomm;const struct bt_rfcomm *source=&voice.rfcomm;BT_WORK_RFCOMM(TAKE)}
    {struct bluetooth_work_sdp *target=&out->discovery;const struct bt_sdp *source=&voice.discovery;BT_WORK_SDP(TAKE)}
    {struct bluetooth_work_server *target=&out->server;const struct bt_sdp_server *source=&voice.server;BT_WORK_SERVER(TAKE)}
    {struct bluetooth_work_ag *target=&out->ag;const struct bt_hfp_ag *source=&voice.ag;
    /* HFP's field named voice is distinct from this file's state alias. */
#undef voice
    BT_WORK_AG(TAKE)
#define voice (*voice_state)
    }
    {struct bluetooth_work_sco *target=&out->audio;const struct bt_sco *source=&voice.audio;BT_WORK_SCO(TAKE)}
    for(unsigned i=0;i<BT_ACL_LINKS;++i){struct bluetooth_work_connection *target=&out->connections[i];
        const struct bt_conn *source=&voice.security.connections[i];BT_WORK_CONNECTION(TAKE)}
    for(unsigned i=0;i<BT_ACL_LINKS;++i){struct bluetooth_work_acl_link *target=&out->links[i];
        const struct bt_acl_link *source=&voice.acl.links[i];BT_WORK_ACL_LINK(TAKE)}
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i){struct bluetooth_work_acl_packet *target=&out->packets[i];
        const struct bt_acl_packet *source=&voice.acl.packets[i];BT_WORK_ACL_PACKET(TAKE)}
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i){struct bluetooth_work_channel *target=&out->channels[i];
        const struct bt_l2cap_channel *source=&voice.l2cap.channels[i];BT_WORK_CHANNEL(TAKE)}
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i){struct bluetooth_work_pdu *target=&out->tx[i];
        const struct bt_l2cap_pdu *source=&voice.l2cap.tx[i];BT_WORK_PDU(TAKE)}
    for(unsigned i=0;i<BT_L2CAP_PDUS;++i){struct bluetooth_work_pdu *target=&out->rx[i];
        const struct bt_l2cap_pdu *source=&voice.l2cap.rx[i];BT_WORK_PDU(TAKE)}
    for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i){struct bluetooth_work_fragment *target=&out->fragments[i];
        const struct bt_l2cap_fragment *source=&voice.l2cap.fragments[i];BT_WORK_FRAGMENT(TAKE)}
    for(unsigned i=0;i<BT_RFCOMM_SLOTS;++i){struct bluetooth_work_rfcomm_buffer *target=&out->rfcomm_tx[i];
        const struct bt_rfcomm_buffer *source=&voice.rfcomm.tx[i];BT_WORK_RFCOMM_BUFFER(TAKE)}
    for(unsigned i=0;i<BT_RFCOMM_DATA;++i){struct bluetooth_work_rfcomm_buffer *target=&out->rfcomm_rx[i];
        const struct bt_rfcomm_buffer *source=&voice.rfcomm.rx[i];BT_WORK_RFCOMM_BUFFER(TAKE)}
    for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i){struct bluetooth_work_server_session *target=&out->sessions[i];
        const struct bt_sdp_server_session *source=&voice.server.sessions[i];BT_WORK_SERVER_SESSION(TAKE)}
    for(unsigned i=0;i<BT_SCO_LINKS;++i){struct bluetooth_work_sco_link *target=&out->audio_links[i];
        const struct bt_sco_link *source=&voice.audio.links[i];BT_WORK_SCO_LINK(TAKE)}
    for(unsigned i=0;i<BT_SCO_PACKETS;++i){struct bluetooth_work_sco_packet *target=&out->audio_tx[i];
        const struct bt_sco_packet *source=&voice.audio.tx[i];BT_WORK_SCO_PACKET(TAKE)}
    for(unsigned i=0;i<BT_SCO_PACKETS;++i){struct bluetooth_work_sco_packet *target=&out->audio_rx[i];
        const struct bt_sco_packet *source=&voice.audio.rx[i];BT_WORK_SCO_PACKET(TAKE)}
#undef TAKE
    for(unsigned i=0;i<BT_ACL_LINKS;++i){
        out->connections[i].flags=voice.security.connections[i].flags[0];
        out->assembling[i]=voice.l2cap.assembling[i];out->next_id[i]=voice.l2cap.next_id[i];
        out->peer_sequence[i]=voice.l2cap.peer_sequence[i];
    }
    for(unsigned i=0;i<BT_RFCOMM_DATA;++i)out->credit_after[i]=voice.rfcomm.credit_after[i];
    for(unsigned i=0;i<BT_H4_SLOTS;++i)out->held_rx[i]=voice.audio.held_rx[i];
#define PUBLIC(name) out->headset.name=atomic_load_explicit(&voice.name,memory_order_acquire);
    PUBLIC(segment_head) PUBLIC(segment_tail) PUBLIC(segment_stop) PUBLIC(segment_flags)
    PUBLIC(segment_disconnected) PUBLIC(segment_request) PUBLIC(terminal)
#undef PUBLIC
    out->stop_phase=context.stop_phase;out->snapshot_bytes=voice.snapshot.bytes;
    out->snapshot_records=voice.snapshot.records;
    return 1;
}
int bluetooth_headset_rx_unclassified(const struct bluetooth_application *a)
{
    if(!a||a->self!=a||xTaskGetCurrentTaskHandle()!=a->owner||
       a->hci.transport.state!=BT_H4_ACTIVE||a->hci.transport.count>BT_H4_SLOTS||
       (voice_state&&voice.app&&voice.app!=a))return -1;
    uint64_t seen=0;
    if(voice_state&&voice.acl.hci==&a->hci&&a->hci.acl_owner==&voice.acl)seen=voice.acl.seen_record;
    for(unsigned i=0;i<a->hci.transport.count;++i){
        const struct bt_h4_record *r=&a->hci.transport.records[i];
        if(r->owner==BT_H4_OWNED&&r->serial>seen)return 1;
    }
    return 0;
}

int bluetooth_headset_wideband(const struct bluetooth_application *a)
{return a&&a->self==a&&a->profile&&((a->profile->version==2&&a->profile->headset.mode==4)||(a->profile->version>=3&&(a->profile->headset.mode==3||a->profile->headset.mode==5)));}
static int segment_live(struct bluetooth_application *a)
{return bluetooth_headset_repeated(a)&&voice.segment_app==a&&
    !atomic_load_explicit(&a->failed,memory_order_acquire)&&
    !(atomic_load_explicit(a->recovery_state,memory_order_acquire)&4);}
static __attribute__((noinline)) int consumer(struct bluetooth_application *a)
{return segment_live(a)&&voice.segment_consumer&&xTaskGetCurrentTaskHandle()==voice.segment_consumer;}
/* A lost or spurious notification changes only latency: the consumer re-reads
 * the counters and the request word after every wake-up. */
static __attribute__((noinline)) void wake(void){if(voice.segment_consumer)xTaskNotifyGive((TaskHandle_t)voice.segment_consumer);}
static void owner_wake(struct bluetooth_application *a)
{if(a->uart.config.deferred_irq&&a->owner)xTaskNotifyGive(a->owner);}
int bluetooth_headset_segment_bind(struct bluetooth_application *a,void *consumer)
{
    if(!bluetooth_headset_repeated(a)||!consumer||voice.segment_consumer||xTaskGetSchedulerState()!=taskSCHEDULER_NOT_STARTED)return 0;
    voice.segment_app=a;voice.segment_consumer=consumer;return 1;
}
int bluetooth_headset_segment_stop(struct bluetooth_application *a)
{
    if(!segment_live(a))return -1;
    if(!atomic_exchange_explicit(&voice.segment_stop,1,memory_order_acq_rel))owner_wake(a);
    return 1;
}
int bluetooth_headset_segment_status(struct bluetooth_application *a,struct bluetooth_segment_status *out)
{
    if(!segment_live(a)||!out||!bt_range_disjoint(out,sizeof(*out),&voice,sizeof(voice))||
       !bt_range_disjoint(out,sizeof(*out),a,sizeof(*a))||!bt_range_disjoint(out,sizeof(*out),a->profile,sizeof(*a->profile))||
       !bt_range_disjoint(out,sizeof(*out),a->shared,sizeof(*a->shared)))return -1;
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
/* The word is loaded before head. The owner stores head before the 0 that
 * ends an activation, so a 0 seen here comes with that activation's segment,
 * and an unchanged head means the activation published none. Only the
 * consumer moves the word out of 0, and only an activation start moves it out
 * of ACCEPT, so a failed exchange means an activation started or ended. */
int bluetooth_headset_segment_request(struct bluetooth_application *a,unsigned request,uint64_t published)
{
    if(!consumer(a)||atomic_load_explicit(&voice.segment_stop,memory_order_acquire)||
       (request!=BLUETOOTH_SEGMENT_ACCEPT&&request!=BLUETOOTH_SEGMENT_START))return -1;
    unsigned word=atomic_load_explicit(&voice.segment_request,memory_order_acquire);
    if(word==request)return 1;
    if(word>request||atomic_load_explicit(&voice.segment_head,memory_order_acquire)!=published)return 0;
    int changed=atomic_compare_exchange_strong_explicit(&voice.segment_request,&word,request,memory_order_acq_rel,memory_order_acquire);
    if(changed)owner_wake(a);
    return changed;
}
int bluetooth_headset_segment_take(struct bluetooth_application *a,const uint8_t **pcm,struct bluetooth_segment_snapshot *snapshot)
{
    if(!consumer(a)||!pcm||!snapshot||!bt_range_disjoint(pcm,sizeof(*pcm),snapshot,sizeof(*snapshot)))return -1;
    const void *buffers[]={pcm,snapshot};size_t sizes[]={sizeof(*pcm),sizeof(*snapshot)};
    for(unsigned i=0;i<2;++i)if(!bt_range_disjoint(buffers[i],sizes[i],&voice,sizeof(voice))||
        !bt_range_disjoint(buffers[i],sizes[i],a,sizeof(*a))||!bt_range_disjoint(buffers[i],sizes[i],a->profile,sizeof(*a->profile))||
        !bt_range_disjoint(buffers[i],sizes[i],a->shared,sizeof(*a->shared)))return -1;
    *pcm=NULL;memset(snapshot,0,sizeof(*snapshot));
    uint64_t tail=atomic_load_explicit(&voice.segment_tail,memory_order_relaxed);
    uint64_t head=atomic_load_explicit(&voice.segment_head,memory_order_acquire);
    if(head<tail||head-tail>BLUETOOTH_SEGMENT_SLOTS)return -1;if(head==tail)return 0;
    unsigned slot=(unsigned)(tail%BLUETOOTH_SEGMENT_SLOTS);const struct bluetooth_segment_snapshot *s=&voice.segments[slot].snapshot;
    if(s->ticket!=tail+1||s->capture.bytes<2||(s->capture.bytes&1)||s->capture.bytes>BLUETOOTH_PCM_BYTES)return -1;
    /* consumer() checked liveness at entry and ack checks it again; a failure
     * in between leaves the slot with its owner, so the pointer stays valid. */
    *snapshot=*s;*pcm=voice.segments[slot].pcm;return 1;
}
int bluetooth_headset_segment_ack(struct bluetooth_application *a,uint64_t ticket)
{
    if(!consumer(a))return -1;
    uint64_t tail=atomic_load_explicit(&voice.segment_tail,memory_order_relaxed);
    uint64_t head=atomic_load_explicit(&voice.segment_head,memory_order_acquire);
    if(!ticket||tail==UINT64_MAX||ticket!=tail+1||head<=tail||head-tail>BLUETOOTH_SEGMENT_SLOTS)return -1;
    atomic_store_explicit(&voice.segment_tail,tail+1,memory_order_release);owner_wake(a);return 1;
}
/* The first stop sets the close bound; later stop reasons only add flags. */
static __attribute__((noinline)) void segment_stopping(unsigned flag,uint64_t now)
{
    atomic_fetch_or_explicit(&voice.segment_flags,flag,memory_order_release);
    atomic_store_explicit(&voice.segment_stop,1,memory_order_release);
    voice.stop=1;
    if(!voice.operation_deadline)voice.operation_deadline=bluetooth_close_deadline(voice.app,now);
}
/* Publishes the even prefix of the segment being written, if it holds a
 * sample, and starts the next one. The digest holds whole blocks up to
 * hashed; the even bytes after it, fewer than 64, are hashed here. Head is
 * stored with release after the slot, its snapshot and digest. */
static __attribute__((noinline)) int publish(struct bluetooth_application *a,unsigned codec)
{
    unsigned bytes=voice.segment_bytes&~1u;
    if(bytes){
        uint64_t head=atomic_load_explicit(&voice.segment_head,memory_order_relaxed);
        unsigned slot=(unsigned)(head%BLUETOOTH_SEGMENT_SLOTS);
        struct bluetooth_segment_snapshot *s=&voice.segments[slot].snapshot;
        s->ticket=head+1;s->first_sample=voice.published_samples;
        s->capture=(struct bluetooth_pcm_snapshot){.firmware=a->firmware_generation,.uart=a->uart_generation,
            .acl=voice.link,.audio=voice.stream,.state=codec,.bytes=bytes,.records=voice.segment_records};
        (void)storage_digest_add(&voice.digest,voice.segments[slot].pcm+voice.hashed,bytes-voice.hashed);
        storage_digest_end(&voice.digest,s->capture.sha256);
        voice.published_samples+=bytes/2;
        atomic_store_explicit(&voice.segment_head,head+1,memory_order_release);
    }
    voice.segment_bytes=voice.segment_records=voice.hashed=0;storage_digest_init(&voice.digest);
    return 1;
}
/* Ends the activation: publication, then the request word with release, then
 * the notification, so a consumer that reads0 sees any published segment. */
static __attribute__((noinline)) void end(struct bluetooth_application *a,unsigned codec)
{
    (void)publish(a,codec);
    voice.active=0;voice.capture_deadline=0;voice.voice_off=voice.bvra&&voice.ag.recognition;
    voice.idle_close=voice.stage==AUDIO||voice.stage==CAPTURE;
    if(voice.idle_close)voice.operation_deadline=bluetooth_close_deadline(a,voice.last_owner);
    atomic_store_explicit(&voice.segment_request,0,memory_order_release);wake();
}
/* Every path to DONE follows a stop. A stop never leaves the headset's
 * recognition on: +BVRA: 0 goes out first, and DONE follows once no AG
 * response is in flight. An AG that is no longer ready has no headset to
 * answer. */
static __attribute__((noinline)) int finished(void)
{
    if(voice.ag.phase==BT_HFP_AG_READY){
        if(voice.ag.recognition)return bt_hfp_ag_voice(&voice.ag,0)<0?fail():0;
        if(voice.ag.response_length)return 0;
    }
    voice.stage=DONE;voice.operation_deadline=0;voice.active=0;
    atomic_store_explicit(&voice.segment_request,0,memory_order_release);
    atomic_store_explicit(&voice.segment_disconnected,1,memory_order_release);wake();return 1;
}
/* Entering idle without an activation. An AT+BVRA=1 or START must commit
 * before the capture window ends: now plus the line timeout (nonzero) within
 * the window, which is now < admit_end. */
static void idle(const struct bluetooth_application *a)
{
    uint64_t line=a->profile->headset.line_timeout_us;
    voice.window=bluetooth_capture_window(a);voice.admit_end=voice.window>=line?voice.window-line+1:0;
    voice.idled=1;voice.operation_deadline=0;voice.stage=IDLE;
}
/* The single AT+BVRA=1 admission rule. It runs before the AG step on every
 * poll once the AG first reached idle, so a stop, which persists through
 * CLOSE and DONE, closes admission at the next AG step. The word is ACTIVE
 * exactly while an activation runs, because only the owner changes it from
 * ACTIVE, so the word test also excludes an active one. */
static void voice_admission(uint64_t now,unsigned word)
{
    voice.ag.voice_admit=!voice.voice_off&&!voice.stop&&!voice.idle_close&&voice.stage<CLOSE&&
        (word==BLUETOOTH_SEGMENT_ACCEPT||word==BLUETOOTH_SEGMENT_START)&&now<voice.admit_end;
}
/* Starts an activation on a committed AT+BVRA=1 or +BVRA: 1 (bvra1), or on
 * START for a headset without voice recognition (bvra0). */
static __attribute__((noinline)) int begin(struct bluetooth_application *a,uint64_t now,unsigned bvra,unsigned word)
{
    do if(word!=BLUETOOTH_SEGMENT_ACCEPT&&word!=BLUETOOTH_SEGMENT_START)return fail();
    while(!atomic_compare_exchange_weak_explicit(&voice.segment_request,&word,BLUETOOTH_SEGMENT_ACTIVE,memory_order_acq_rel,memory_order_acquire));
    /* A packet may split a CVSD sample: after an odd byte count the
     * activation skips the second half. */
    voice.active=1;voice.bvra=bvra;voice.skip=(unsigned)(voice.position&1);storage_digest_init(&voice.digest);
    voice.capture_deadline=cap(now,a->profile->headset.capture_timeout_us,voice.window);
#ifdef SUNSTONE_BLUETOOTH_MSBC
    /* mSBC frames are 60 bytes, so once a frame has decoded on this link the
     * next frame boundary is known and the decoder starts there. Before that
     * the header search finds it, once per link. On a link that fell back to
     * CVSD no frame decodes, so boundary stays 0 and the sample parity skip
     * above applies. */
    if(voice.wideband){bluetooth_msbc_resync();
        if(voice.boundary){unsigned r=(unsigned)((voice.position-voice.boundary)%60);voice.skip=r?60-r:0;}}
#endif
    if(voice.stage!=IDLE)return 1;
    /* Each activation opens the link. The AG
     * stage opens it, after selecting the codec again if an AT+BAC while
     * idle cleared the selection. The bound allows one AG response and one
     * codec reselection, each within the line timeout, and the link open and
     * its CVSD retry, each within the response timeout; a supervised session
     * also caps it at its close deadline. */
    const struct bluetooth_headset_profile *p=&a->profile->headset;
    voice.operation_deadline=cap(now,2*(uint64_t)p->line_timeout_us+2*p->response_timeout_us,
        a->resident?UINT64_MAX:bluetooth_close_deadline(a,now));
    voice.stage=AG;return 1;
}
/* Starts an activation on START: +BVRA: 1 for a headset with voice
 * recognition, otherwise at once (HFP section 4.11). */
static __attribute__((noinline)) int start(struct bluetooth_application *a,uint64_t now,unsigned word)
{
    if(!(voice.ag.hf_features&BT_HFP_AG_HF_VOICE))return begin(a,now,0,word);
    return bt_hfp_ag_voice(&voice.ag,1)<0?fail():1;
}
/* +BVRA: 0 is owed after a segment ended an activation started with +BVRA. */
static __attribute__((noinline)) int voice_off(struct bluetooth_application *a,uint64_t now,unsigned word)
{
    if(voice.ag.recognition)return bt_hfp_ag_voice(&voice.ag,0)<0?fail():1;
    voice.voice_off=0;
    return word==BLUETOOTH_SEGMENT_START&&now<voice.admit_end?start(a,now,word):1;
}
/* The headset ended recognition early (AT+BVRA=0): publish the even prefix. */
static __attribute__((noinline)) int early_end(struct bluetooth_application *a)
{const struct bt_sco_link *l=stream();end(a,l?l->codec:0);return 1;}
/* A stop, the window, or an AG that left the ready state. The owner never
 * closes the AG, so the last, as when the headset closes the service-level
 * connection, is a release the owner did not request and fails the session
 * unless a stop or the window already ends it. Before the link is open, idle
 * and the AG stages end once no codec offer is in flight, through
 * finished(), and AUDIO waits for the open's result. With the link open the
 * repeated-capture step closes it. */
static __attribute__((noinline)) int halted(uint64_t now)
{
    if(!voice.stop&&now<voice.window)return fail();
    if(voice.stage==CAPTURE)return 1;
    if(!voice.stop)segment_stopping(BLUETOOTH_SEGMENT_WINDOW,now);
    /* A consumer's stop carries no flag yet. */
    else if(!atomic_load_explicit(&voice.segment_flags,memory_order_acquire))segment_stopping(BLUETOOTH_SEGMENT_CANCEL,now);
    return voice.stage==AUDIO?1:voice.ag.offered_codec?0:finished();
}
/* Voice recognition after the AG step, from IDLE through CAPTURE: stops,
 * the window and AG release, activation start and end, and the owner's
 * +BVRA: 0. 1 continue, 0 or -1 return that result. Every action is a tail
 * call, so a poll with nothing to do sets up no stack frame. */
static int recognition(struct bluetooth_application *a,uint64_t now,unsigned word)
{
    const struct bt_hfp_ag *g=&voice.ag;
    if(voice.stop||now>=voice.window||g->phase!=BT_HFP_AG_READY)return halted(now);
    if(voice.active)return voice.bvra&&!g->recognition?early_end(a):1;
    if(voice.idle_close)return voice.voice_off?voice_off(a,now,0):1;
    if(voice.voice_off)return voice_off(a,now,word);
    if(g->recognition)return begin(a,now,1,word);
    return word==BLUETOOTH_SEGMENT_START&&now<voice.admit_end?start(a,now,word):1;
}
/* Writes the packet into the segment being recorded, from partial_offset,
 * and ends or publishes the segment when it is full. */
static __attribute__((noinline)) int capture_packet(struct bluetooth_application *a,const struct bt_sco_link *l,
    const struct bt_sco_packet *rx,uint64_t now)
{
    const struct bluetooth_headset_profile *p=&a->profile->headset;
    voice.partial_serial=rx->serial;
    if(rx->status)segment_stopping(BLUETOOTH_SEGMENT_LOSS,now);
    /* An activation starts between packets; its first packets may hold the
     * bytes it skips. */
    if(voice.skip){unsigned n=rx->length-voice.partial_offset;if(n>voice.skip)n=voice.skip;
        voice.skip-=n;voice.partial_offset+=n;}
    while(voice.partial_offset<rx->length&&voice.active&&!voice.stop){
        uint64_t head=atomic_load_explicit(&voice.segment_head,memory_order_relaxed);
        uint64_t tail=atomic_load_explicit(&voice.segment_tail,memory_order_acquire);
        if(head<tail||head-tail>BLUETOOTH_SEGMENT_SLOTS||head>=UINT64_MAX/p->samples)return fail();
        if(head-tail==BLUETOOTH_SEGMENT_SLOTS||voice.segment_records==BLUETOOTH_PCM_RECORDS){
            segment_stopping(BLUETOOTH_SEGMENT_BACKLOG,now);break;}
        unsigned slot=(unsigned)(head%BLUETOOTH_SEGMENT_SLOTS);
        struct bluetooth_segment_snapshot *s=&voice.segments[slot].snapshot;
#ifdef SUNSTONE_BLUETOOTH_MSBC
        if(l->codec==2){
            /* Frames decode into the slot. A frame always fits: profile
             * admission makes mSBC sample counts multiples of 120, and
             * segment_bytes grows in 240-byte frames until the segment is
             * full and published. */
            uint64_t first=0;
            int result=bluetooth_msbc_byte(rx->bytes[voice.partial_offset++],rx->acquired,
                voice.segments[slot].pcm+voice.segment_bytes,&first);
            if(result<0){segment_stopping(BLUETOOTH_SEGMENT_LOSS,now);break;}
            if(!result)continue;
            if(!voice.segment_bytes){memset(s,0,sizeof(*s));s->first_acquired=first;}
            voice.segments[slot].records[voice.segment_records++]=(struct bluetooth_pcm_record){first,
                voice.segment_bytes,60,240,0,0};
            voice.segment_bytes+=240;s->last_acquired=rx->acquired;voice.boundary=voice.position+voice.partial_offset;
        }else
#endif
        {
            if(!voice.segment_bytes){memset(s,0,sizeof(*s));s->first_acquired=rx->acquired;}
            unsigned remaining=p->samples*2-voice.segment_bytes;
            size_t used=rx->length-voice.partial_offset;if(used>remaining)used=remaining;
            voice.segments[slot].records[voice.segment_records++]=(struct bluetooth_pcm_record){rx->acquired,
                voice.segment_bytes,(uint16_t)rx->length,(uint16_t)used,rx->status,0};
            memcpy(voice.segments[slot].pcm+voice.segment_bytes,rx->bytes+voice.partial_offset,used);
            voice.segment_bytes+=(unsigned)used;voice.partial_offset+=(unsigned)used;s->last_acquired=rx->acquired;
        }
        /* Hash each complete 64-byte block while it is in L1. Whole blocks
         * keep the digest's staging block empty, so they compress in place
         * from the slot; publish() hashes the rest. */
        unsigned whole=voice.segment_bytes&~63u;
        if(whole>voice.hashed){
            if(!storage_digest_add(&voice.digest,voice.segments[slot].pcm+voice.hashed,whole-voice.hashed))return fail();
            voice.hashed=whole;}
        if(voice.segment_bytes==p->samples*2){
            if(voice.recognition){end(a,l->codec);break;}
            (void)publish(a,l->codec);wake();
            voice.capture_deadline=cap(now,p->capture_timeout_us,voice.window);
        }
    }
    return 1;
}
static int repeated_audio(struct bluetooth_application *a,struct bt_sco_link *l,uint64_t now)
{
    const struct bluetooth_headset_profile *p=&a->profile->headset;
    const uint64_t window=voice.window;
    /* Only the close at the end of this step leaves CAPTURE. A consumer's
     * stop is flagged at the close, before its wait for the UART. */
    const unsigned capture=voice.stage==CAPTURE;
    if(capture){
        if(l->phase!=BT_SCO_OPEN)return fail();
        if(now>=window)segment_stopping(BLUETOOTH_SEGMENT_WINDOW,now);
        else if(voice.capture_deadline&&now>=voice.capture_deadline&&p->mode!=4)segment_stopping(BLUETOOTH_SEGMENT_DEADLINE,now);
    }
#ifdef SUNSTONE_BLUETOOTH_MSBC
    if(bluetooth_playback_mode(a)){
        if(capture&&bluetooth_playback_cancelled(a))segment_stopping(BLUETOOTH_SEGMENT_CANCEL,now);
        if(voice.stop&&!bluetooth_playback_done(a))bluetooth_playback_stop(a);
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
        if(capture&&voice.active&&!voice.stop&&capture_packet(a,l,rx,now)!=1)return -1;
        /* Closing explicitly abandons unpublished packet tails. Accepted input
         * slots stay immutable; only the exact currently borrowed RX is freed.
         * Between activations each packet is released without a copy. */
        if(!voice.active||!capture||voice.partial_offset==rx->length||voice.stop){
            voice.position+=rx->length;
            if(bt_sco_release_receive(&voice.audio,rx->serial)!=1)return fail();
            voice.partial_serial=0;voice.partial_offset=0;
        }
    }
    if(capture){
#ifdef SUNSTONE_BLUETOOTH_MSBC
        if(bluetooth_playback_mode(a)){
            if(voice.stop&&!bluetooth_playback_done(a))bluetooth_playback_stop(a);
            if(bluetooth_playback_done(a))segment_stopping(BLUETOOTH_SEGMENT_WINDOW,now);
        }
#endif
        if(voice.stop||voice.idle_close){
            /* Finish recognition-off control traffic before audio disconnect.
             * START may be pending, but cannot reopen until close has drained. */
            if(voice.recognition){
                if(voice.ag.recognition)return bt_hfp_ag_voice(&voice.ag,0)<0?fail():0;
                voice.voice_off=0;if(voice.ag.response_length)return 0;
            }
            if(!voice.operation_deadline)segment_stopping(BLUETOOTH_SEGMENT_CANCEL,now);
            if(voice.audio.active_tx<BT_SCO_PACKETS||a->hci.transport.tx_offset<a->hci.transport.tx_length)return 0;
            if(bt_sco_close(&voice.audio,voice.stream,voice.operation_deadline)!=1)return fail();voice.stage=CLOSE;return 0;
        }
        if(now<voice.next_silence)return 0;
        /* Between activations silence is bounded by the response timeout and
         * paced up to the capture window. */
        uint64_t limit=voice.active?voice.capture_deadline:cap(now,p->response_timeout_us,window);
        uint64_t pace=voice.active?voice.capture_deadline:window;
#ifdef SUNSTONE_BLUETOOTH_MSBC
        if(l->codec==2){
            if(p->mode>=4)return 0;
            if(!voice.silence_ready){if(bluetooth_msbc_silence(voice.silence)!=1)return fail();voice.silence_ready=1;}
            unsigned bytes=60-voice.silence_offset;if(bytes>voice.audio.mtu)bytes=voice.audio.mtu;
            uint64_t serial;int r=bt_sco_send(&voice.audio,voice.stream,voice.silence+voice.silence_offset,bytes,limit,&serial);
            if(r<0)return fail();if(r==1){voice.silence_offset+=bytes;
                if(voice.silence_offset==60){voice.silence_offset=0;voice.silence_ready=0;
                    voice.next_silence=bluetooth_cadence(voice.next_silence,now,7500,pace,&voice.silence_restarts);}}
        }else
#endif
        {uint64_t serial;
            int r=bt_sco_send(&voice.audio,voice.stream,voice.silence,voice.silence_bytes,limit,&serial);
            if(r<0)return fail();
            if(r==1)voice.next_silence=bluetooth_cadence(voice.next_silence,now,(voice.silence_bytes/2)*125,pace,&voice.silence_restarts);}
        return 0;
    }
    if(voice.stage==CLOSE&&l->phase==BT_SCO_CLOSED){
#ifdef SUNSTONE_BLUETOOTH_MSBC
        if(bluetooth_playback_mode(a))bluetooth_playback_closed(a);
#endif
        if(voice.idle_close&&!voice.stop){
            if(!l->local_closed)return fail();
            /* Retire copied packets before exposing idle. The lower worker
             * keeps pre-disconnect held records bound to the closed stream. */
            if(bt_sco_result(&voice.audio)||bt_sco_receive(&voice.audio))return 0;
            for(unsigned i=0;i<BT_H4_SLOTS;++i)if(voice.audio.held_rx[i])return 0;
            voice.idle_close=0;idle(a);return 0;
        }
        return finished();
    }
    /* DONE still reaches control_step, bt_sco_step and security_service in the
     * owner poll. Revocation remains observable through the output lifetime. */
    return voice.stage==DONE?1:0;
}

static int stop_fault(void)
{context.stop_phase=STOP_FAULT;return fail();}
int bluetooth_headset_stop_begin(struct bluetooth_application *a,uint64_t now,uint64_t deadline)
{
    if(!a||a->self!=a||!a->profile||a->profile->version<2||
       (a->profile->headset.mode!=2&&a->profile->headset.mode!=3)||xTaskGetCurrentTaskHandle()!=a->owner)return -1;
    if(context.stop_phase==STOP_FAULT)return -1;
    if(context.app&&context.app!=a)return -1;
    if(atomic_load_explicit(&a->failed,memory_order_acquire)||
       (atomic_load_explicit(a->recovery_state,memory_order_acquire)&4)){
        context.stop_phase=STOP_FAULT;return -1;}
    if(context.stop_phase==STOP_DONE)return context.app==a?1:-1;
    if(!voice.app){voice.app=a;context.app=a;voice.last_owner=now;}
    if(context.app!=a||atomic_load_explicit(&a->failed,memory_order_acquire)||
       (atomic_load_explicit(a->recovery_state,memory_order_acquire)&4)||now<voice.last_owner||deadline<=now)return stop_fault();
    if(deadline>a->deadline)deadline=a->deadline;
    if(!context.stop_deadline||deadline<context.stop_deadline)context.stop_deadline=deadline;
    if(context.close_deadline>context.stop_deadline)context.close_deadline=context.stop_deadline;
    if(!context.stop_phase)context.stop_phase=STOP_SCAN;
    voice.stop=1;voice.ag.voice_admit=0;
    atomic_store_explicit(&voice.segment_stop,1,memory_order_release);
    if(voice.acl.state==BT_ACL_ACTIVE){
        if(bt_acl_close_admission(&voice.acl,context.stop_deadline)!=1)return stop_fault();
        if(voice.security.self&&bt_security_stop(&voice.security,context.stop_deadline)!=1)return stop_fault();
        if(voice.audio.initialized&&bt_sco_close_admission(&voice.audio,context.stop_deadline)!=1)return stop_fault();
    }else if(voice.acl.state!=BT_ACL_OFF)return stop_fault();
    return 1;
}
/* Retire only terminal copied records. A held H4 packet or accepted transmit
 * remains with its worker until the matched completion makes it releasable. */
static int stop_audio_records(void)
{
    const struct bt_sco_packet *p=bt_sco_result(&voice.audio);
    if(p&&bt_sco_release_result(&voice.audio,p->serial)!=1)return -1;
    p=bt_sco_receive(&voice.audio);
    if(p){if(bt_sco_release_receive(&voice.audio,p->serial)!=1)return -1;
        if(voice.partial_serial==p->serial){voice.partial_serial=0;voice.partial_offset=0;}}
    return 1;
}
static int stop_security(void)
{return !voice.security.self||bt_security_service(&voice.security,context.close_deadline?context.close_deadline:context.stop_deadline)==1;}
static int stop_control_step(uint64_t deadline)
{
    if(voice.ag.rfcomm&&voice.ag.phase!=BT_HFP_AG_CLOSED)return bt_hfp_ag_step(&voice.ag,64,128,deadline);
    if(voice.rfcomm.l2cap&&voice.rfcomm.phase!=BT_RFCOMM_CLOSED)return bt_rfcomm_step(&voice.rfcomm,64,deadline);
    return bt_l2cap_step(&voice.l2cap,64,deadline);
}
static int stop_audio_service(void)
{
    return !voice.audio.initialized||
        (bt_sco_service(&voice.audio)==1&&stop_audio_records()==1);
}
/* Visit each existing consumer once in its normal order. SCO metadata service
 * between consumers lets a classified request transmit during this owner pass,
 * leaving the next pass for its response within the original command bound. */
static int stop_consumers_step(uint64_t bound)
{
    if(voice.l2cap.state!=BT_L2CAP_ACTIVE)return bt_acl_step(&voice.acl,64,bound);
    if(voice.discovery.l2cap&&voice.discovery.phase!=BT_SDP_CLOSED){
        if(voice.discovery.phase!=BT_SDP_CLOSING&&bt_sdp_close(&voice.discovery,bound)!=1)return -1;
        if(bt_sdp_step(&voice.discovery,64,bound)<0||!stop_audio_service())return -1;
    }
    if(bt_sdp_server_step(&voice.server,64,bound)<0||!stop_audio_service())return -1;
    if(!voice.ag.rfcomm&&voice.rfcomm.l2cap&&voice.rfcomm.phase!=BT_RFCOMM_CLOSED&&
       voice.rfcomm.phase!=BT_RFCOMM_CLOSING&&bt_rfcomm_close(&voice.rfcomm,bound)!=1)return -1;
    if(stop_control_step(bound)<0||!stop_audio_service())return -1;
    if(voice.audio.initialized&&
       (bt_sco_step(&voice.audio,64,bound)!=1||stop_audio_records()!=1))return -1;
    return 1;
}
static int stop_acl_step(uint64_t bound)
{
    /* L2CAP validates final data for a known disconnecting handle. The audio
     * worker then transmits any late refusal queued by its metadata service. */
    if(bt_l2cap_step(&voice.l2cap,64,bound)!=1||!stop_audio_service())return -1;
    if(voice.audio.initialized&&
       (bt_sco_step(&voice.audio,64,bound)!=1||stop_audio_records()!=1))return -1;
    return 1;
}
static int stop_complete(void)
{
    context.stop_phase=STOP_DONE;voice.stage=DONE;voice.active=0;voice.operation_deadline=0;
    atomic_store_explicit(&voice.segment_request,0,memory_order_release);
    atomic_store_explicit(&voice.segment_disconnected,1,memory_order_release);wake();return 1;
}
int bluetooth_headset_stop(struct bluetooth_application *a,uint64_t now,uint64_t deadline)
{
    if(bluetooth_headset_stop_begin(a,now,deadline)!=1)return -1;
    if(context.stop_phase==STOP_DONE)return 1;
    if(!live(a))return stop_fault();
    if(now>=context.stop_deadline||(context.close_deadline&&now>=context.close_deadline))return stop_fault();
    voice.last_owner=now;
    if(context.stop_phase==STOP_SCAN){
        int r=voice.acl.state==BT_ACL_ACTIVE?stop_consumers_step(context.stop_deadline):bt_hci_step(&a->hci,64,context.stop_deadline);
        if(r<0||!stop_security())return stop_fault();
        r=bt_hci_scan_disabled(&a->hci);if(r<0)return stop_fault();if(!r)return 0;
        if(voice.acl.state==BT_ACL_OFF)return stop_complete();
        context.close_deadline=cap(now,2000000,context.stop_deadline);
        if(voice.operation_deadline&&context.close_deadline>voice.operation_deadline)context.close_deadline=voice.operation_deadline;
        if(bt_acl_close_admission(&voice.acl,context.close_deadline)!=1||
           (voice.security.self&&bt_security_stop(&voice.security,context.close_deadline)!=1)||
           (voice.audio.initialized&&bt_sco_close_admission(&voice.audio,context.close_deadline)!=1))return stop_fault();
        context.stop_phase=STOP_VOICE;return 0;
    }
    uint64_t bound=context.close_deadline;
    /* Every accepted channel keeps its consumer until upper closure. One
     * channel's response can arrive behind another channel's owned input. */
    if(context.stop_phase<STOP_ACL&&
       (stop_consumers_step(bound)<0||!stop_security()))return stop_fault();
    if(context.stop_phase==STOP_VOICE){
        if(voice.ag.rfcomm&&voice.ag.phase==BT_HFP_AG_READY){
            if(voice.ag.recognition){if(bt_hfp_ag_voice(&voice.ag,0)<0)return stop_fault();return 0;}
            if(voice.ag.response_length||voice.ag.transmit)return 0;
        }
        if(voice.audio.initialized&&bt_sco_stop(&voice.audio,bound)!=1)return stop_fault();
        context.stop_phase=STOP_AUDIO;return 0;
    }
    if(context.stop_phase==STOP_AUDIO){
        if(voice.audio.initialized){
            int r=bt_sco_quiescent(&voice.audio);if(r<0)return stop_fault();if(!r)return 0;
        }
        context.stop_phase=STOP_CONTROL;return 0;
    }
    if(context.stop_phase==STOP_CONTROL){
        if(voice.rfcomm.error||voice.ag.error)return stop_fault();
        if(voice.ag.rfcomm&&voice.ag.phase!=BT_HFP_AG_CLOSED&&voice.ag.phase!=BT_HFP_AG_CLOSING){
            if(voice.ag.phase==BT_HFP_AG_FAULT||bt_hfp_ag_close(&voice.ag,bound)!=1)return stop_fault();return 0;}
        if(!voice.ag.rfcomm&&voice.rfcomm.l2cap&&voice.rfcomm.phase!=BT_RFCOMM_CLOSED&&voice.rfcomm.phase!=BT_RFCOMM_CLOSING){
            if(voice.rfcomm.phase==BT_RFCOMM_FAULT||bt_rfcomm_close(&voice.rfcomm,bound)!=1)return stop_fault();return 0;}
        if(voice.rfcomm.l2cap){
            if(voice.rfcomm.phase!=BT_RFCOMM_CLOSED||(voice.ag.rfcomm&&voice.ag.phase!=BT_HFP_AG_CLOSED))return 0;
            const struct bt_rfcomm_buffer *p=bt_rfcomm_result(&voice.rfcomm);
            if(p){if(bt_rfcomm_release_result(&voice.rfcomm,p->serial)!=1)return stop_fault();return 0;}
            p=bt_rfcomm_receive(&voice.rfcomm);
            if(p){if(bt_rfcomm_release_receive(&voice.rfcomm,p->serial)!=1)return stop_fault();return 0;}
            if(voice.ag.transmit||voice.ag.receive)return stop_fault();
        }
        context.stop_phase=STOP_SDP;return 0;
    }
    if(context.stop_phase==STOP_SDP){
        if(voice.discovery.l2cap&&voice.discovery.phase!=BT_SDP_CLOSED){
            if(voice.discovery.phase!=BT_SDP_CLOSING){if(bt_sdp_close(&voice.discovery,bound)!=1)return stop_fault();return 0;}
            return 0;
        }
        context.stop_phase=STOP_SERVER;return 0;
    }
    if(context.stop_phase==STOP_SERVER){
        for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i){struct bt_sdp_server_session *s=&voice.server.sessions[i];
            if(!s->channel)continue;if(s->error)return stop_fault();
            const struct bt_l2cap_channel *c=NULL;
            for(unsigned j=0;j<BT_L2CAP_CHANNELS;++j)if(voice.l2cap.channels[j].generation==s->channel)c=&voice.l2cap.channels[j];
            if(!c||c->state==BT_L2CAP_QUARANTINED)return stop_fault();
            if(c->state==BT_L2CAP_CLOSED){int r=bt_sdp_server_release_channel(&voice.server,s->channel);if(r<0)return stop_fault();return 0;}
            if(!s->closing&&c->state==BT_L2CAP_OPEN&&bt_sdp_server_close(&voice.server,s->channel,bound)!=1)return stop_fault();
            return 0;
        }
        context.stop_phase=STOP_CHANNELS;return 0;
    }
    if(context.stop_phase==STOP_CHANNELS){
        for(unsigned i=0;i<BT_L2CAP_CHANNELS;++i){const struct bt_l2cap_channel *c=&voice.l2cap.channels[i];
            if(c->state==BT_L2CAP_UNUSED)continue;
            if(c->state==BT_L2CAP_QUARANTINED||(c->state==BT_L2CAP_CLOSED&&c->completion!=BT_L2CAP_CONFIRMED))return stop_fault();
            if(c->state==BT_L2CAP_OPEN){if(bt_l2cap_close(&voice.l2cap,c->generation,bound)<0)return stop_fault();return 0;}
            if(c->state!=BT_L2CAP_CLOSED)return 0;
        }
        for(unsigned i=0;i<BT_L2CAP_PDUS;++i){
            if(voice.l2cap.tx[i].state!=BT_L2CAP_FREE||voice.l2cap.rx[i].state!=BT_L2CAP_FREE)return 0;}
        for(unsigned i=0;i<BT_ACL_TX_SLOTS;++i)if(voice.l2cap.fragments[i].acl_serial)return 0;
        if(bt_acl_receive(&voice.acl))return 0;
        if(voice.audio.initialized){int r=bt_sco_quiescent(&voice.audio);if(r<0)return stop_fault();if(!r)return 0;}
        if(bt_acl_stop(&voice.acl,bound)!=1)return stop_fault();context.stop_phase=STOP_ACL;return 0;
    }
    if(context.stop_phase==STOP_ACL){
        if(stop_acl_step(bound)!=1)return stop_fault();
        int audio=1;
        if(voice.audio.initialized){
            audio=bt_sco_quiescent(&voice.audio);if(audio<0)return stop_fault();}
        if(!stop_security())return stop_fault();if(!audio)return 0;
        int r=bt_acl_quiescent(&voice.acl);if(r<0)return stop_fault();if(!r)return 0;
        if(voice.security.self){r=bt_security_quiescent(&voice.security);if(r<0)return stop_fault();if(!r)return 0;}
        return stop_complete();
    }
    return stop_fault();
}
