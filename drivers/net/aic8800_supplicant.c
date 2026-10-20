/* Adapter for the unmodified, pinned BSD RSN supplicant. Crypto and handshake
 * validation execute in that implementation; firmware actions remain native. */
#include "aic8800_supplicant_port.h"
#include "aic8800_supplicant.h"
#include "aic8800_sdio_transport.h"
#include "utils/common.h"
#include "rsn_supp/wpa.h"
#include "common/wpa_common.h"
#include "common/ieee802_11_defs.h"
static int span(const void *p,size_t n){return p&&n<=UINTPTR_MAX-(uintptr_t)p;}
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{return (uintptr_t)a<(uintptr_t)b+bn&&(uintptr_t)b<(uintptr_t)a+an;}
static const uint8_t own_rsn[]={48,20,1,0,0,15,172,4,1,0,0,15,172,4,1,0,0,15,172,2,0,0};
void aic8800_supplicant_stop(struct aic8800_supplicant *s)
{
    if(!s||!s->initialized||s->state==AIC8800_SUPPLICANT_RETIRED)return;
    s->state=AIC8800_SUPPLICANT_FAULT;if(s->security)aic8800_security_stop(s->security,s->owner);
}
static struct aic8800_supplicant_action *reserve(struct aic8800_supplicant *s)
{
    if(s->state==AIC8800_SUPPLICANT_FAULT||s->count==AIC8800_SUPPLICANT_ACTIONS){aic8800_supplicant_stop(s);return 0;}
    struct aic8800_supplicant_action *a=&s->actions[(s->head+s->count)%AIC8800_SUPPLICANT_ACTIONS];
    aic8800_security_zero(a,sizeof(*a));++s->count;return a;
}
static void set_state(void *ctx,enum wpa_states state)
{
    struct aic8800_supplicant *s=ctx;s->rsn_state=state;s->completed=state==WPA_COMPLETED;
    if(state!=WPA_COMPLETED&&s->state==AIC8800_SUPPLICANT_AUTHORIZED){s->security->authorized=0;s->state=AIC8800_SUPPLICANT_HANDSHAKE;}
}
static enum wpa_states get_state(void *ctx){return (enum wpa_states)((struct aic8800_supplicant *)ctx)->rsn_state;}
static void deauth(void *ctx,u16 reason){(void)reason;aic8800_supplicant_stop(ctx);}
static void reconnect(void *ctx){aic8800_supplicant_stop(ctx);}
static void *network(void *ctx){return ctx;}
static int bssid(void *ctx,u8 *out){struct aic8800_supplicant *s=ctx;os_memcpy(out,s->wifi->target.bssid,6);return 0;}
static int beacon(void *ctx)
{struct aic8800_supplicant *s=ctx;return s->config.beacon_record==s->wifi->target.beacon_evidence?0:-1;}
static void cancel_auth(void *ctx){(void)ctx;/* Native deadline remains until firmware actions finish. */}
static int protection(void *ctx,const u8 *addr,int type,int key_type)
{struct aic8800_supplicant *s=ctx;(void)type;(void)key_type;return os_memcmp(addr,s->wifi->target.bssid,6)?-1:0;}
static int add_pmkid(void *ctx,void *network_ctx,const u8 *addr,const u8 *id,const u8 *cache,
    const u8 *pmk,size_t bytes,u32 lifetime,u8 threshold,int akmp)
{(void)ctx;(void)network_ctx;(void)addr;(void)id;(void)cache;(void)pmk;(void)bytes;(void)lifetime;(void)threshold;(void)akmp;return -1;}
static int remove_pmkid(void *ctx,void *network_ctx,const u8 *addr,const u8 *id,const u8 *cache)
{(void)ctx;(void)network_ctx;(void)addr;(void)id;(void)cache;return -1;}
static int set_key(void *ctx,int link_id,enum wpa_alg alg,const u8 *addr,int key_id,int set_tx,
    const u8 *seq,size_t seq_len,const u8 *key,size_t key_len,enum key_flag flag)
{
    struct aic8800_supplicant *s=ctx;(void)set_tx;
    /* The source treats RX-only NEXT as optional. D80 cannot encode that mode. */
    if(flag&KEY_FLAG_NEXT)return -1;
    unsigned pairwise=!!(flag&KEY_FLAG_PAIRWISE);
    if(link_id!=-1||alg!=WPA_ALG_CCMP||key_len!=16||seq_len!=6||!key||!seq||
        key_id<0||key_id>3||(pairwise&&(key_id||!addr||os_memcmp(addr,s->wifi->target.bssid,6)))||
        !(flag&KEY_FLAG_RX)||(pairwise&&!(flag&KEY_FLAG_TX))) {aic8800_supplicant_stop(s);return -1;}
    struct aic8800_supplicant_action *a=reserve(s);if(!a)return -1;
    a->kind=AIC8800_SUPPLICANT_KEY;a->pairwise=(uint8_t)pairwise;a->key_id=(uint8_t)key_id;
    os_memcpy(a->key,key,16);os_memcpy(a->rsc,seq,6);return 0;
}
static int ether_send(void *ctx,const u8 *dest,u16 proto,const u8 *p,size_t n)
{
    struct aic8800_supplicant *s=ctx;
    if(proto!=0x888e||!dest||!p||n<4||n>AIC8800_DATA_ETHERNET_BYTES-14||os_memcmp(dest,s->wifi->target.bssid,6))
        {aic8800_supplicant_stop(s);return -1;}
    struct aic8800_supplicant_action *a=reserve(s);if(!a)return -1;
    a->kind=AIC8800_SUPPLICANT_EAPOL;a->length=(uint16_t)(14+n);
    os_memcpy(a->bytes,dest,6);os_memcpy(a->bytes+6,s->wifi->mac,6);a->bytes[12]=0x88;a->bytes[13]=0x8e;os_memcpy(a->bytes+14,p,n);return 0;
}
static u8 *alloc_eapol(void *ctx,u8 type,const void *payload,u16 bytes,size_t *length,void **data)
{
    (void)ctx;if(bytes>AIC8800_DATA_ETHERNET_BYTES-18)return 0;
    u8 *p=os_zalloc(4+bytes);if(!p)return 0;p[0]=2;p[1]=type;p[2]=(u8)(bytes>>8);p[3]=(u8)bytes;
    if(payload)os_memcpy(p+4,payload,bytes);
    *length=4+bytes;if(data)*data=p+4;return p;
}
int aic8800_supplicant_config_valid(const struct aic8800_supplicant_config *cfg)
{
    if(!span(cfg,sizeof(*cfg))||!cfg->entropy||!cfg->entropy_record||!cfg->beacon_record||
       !cfg->ticks_per_second||cfg->ticks_per_second>UINT64_MAX/1000000||
       !cfg->command_timeout||cfg->command_timeout>UINT64_MAX/8||
       cfg->ap_rsn_bytes<2||cfg->ap_rsn_bytes>sizeof(cfg->ap_rsn))return 0;
    struct wpa_ie_data ie;
    return !wpa_parse_wpa_ie_rsn(cfg->ap_rsn,cfg->ap_rsn_bytes,&ie)&&ie.group_cipher==WPA_CIPHER_CCMP&&
        (ie.pairwise_cipher&WPA_CIPHER_CCMP)&&(ie.key_mgmt&WPA_KEY_MGMT_PSK)&&!(ie.capabilities&WPA_CAPABILITY_MFPR);
}
int aic8800_supplicant_init(struct aic8800_supplicant *s,const struct aic8800_supplicant_config *cfg)
{
    if(!span(s,sizeof(*s))||!aic8800_supplicant_config_valid(cfg)||
       overlap(s,sizeof(*s),cfg,sizeof(*cfg))||s->initialized)return 0;
    aic8800_security_zero(s,sizeof(*s));s->config=*cfg;s->initialized=1;return 1;
}
int aic8800_supplicant_claim(struct aic8800_supplicant *s,struct aic8800_sdio_transport *owner,
    struct aic8800_security *security,uint64_t now,uint64_t deadline)
{
    if(!s||!s->initialized||s->owner||!owner||!owner->wifi||!owner->data||!security||
        s->state!=AIC8800_SUPPLICANT_NEW||deadline<=now||deadline>owner->deadline||
        s->config.beacon_record!=owner->wifi->target.beacon_evidence)return 0;
    if(!aic8800_security_claim(security,owner,owner->wifi,s,now))return 0;
    s->security=security;s->data=owner->data;s->wifi=owner->wifi;s->owner=owner;s->generation=s->wifi->association_generation;
    s->last_now=now;s->deadline=deadline;s->state=AIC8800_SUPPLICANT_HANDSHAKE;s->rsn_state=WPA_ASSOCIATED;
    if(!aic8800_supplicant_os_enter(s))goto fail;
    struct wpa_sm_ctx *ctx=os_zalloc(sizeof(*ctx));if(!ctx)goto leave_fail;
    *ctx=(struct wpa_sm_ctx){.ctx=s,.msg_ctx=s,.set_state=set_state,.get_state=get_state,.deauthenticate=deauth,
        .reconnect=reconnect,.set_key=set_key,.get_network_ctx=network,.get_bssid=bssid,.ether_send=ether_send,
        .get_beacon_ie=beacon,.cancel_auth_timeout=cancel_auth,.alloc_eapol=alloc_eapol,.add_pmkid=add_pmkid,
        .remove_pmkid=remove_pmkid,.mlme_setprotection=protection};
    s->rsn=wpa_sm_init(ctx);if(!s->rsn)goto leave_fail;
    struct rsn_supp_config config={.network_ctx=s,.allowed_pairwise_cipher=WPA_CIPHER_CCMP,
        .ssid=s->wifi->target.ssid,.ssid_len=s->wifi->target.ssid_bytes};
    wpa_sm_set_config(s->rsn,&config);wpa_sm_set_fast_reauth(s->rsn,0);wpa_sm_set_own_addr(s->rsn,s->wifi->mac);
    wpa_sm_set_pmk(s->rsn,s->config.pmk,32,0,0);
    if(wpa_sm_set_param(s->rsn,WPA_PARAM_PROTO,WPA_PROTO_RSN)||wpa_sm_set_param(s->rsn,WPA_PARAM_PAIRWISE,WPA_CIPHER_CCMP)||
        wpa_sm_set_param(s->rsn,WPA_PARAM_GROUP,WPA_CIPHER_CCMP)||wpa_sm_set_param(s->rsn,WPA_PARAM_KEY_MGMT,WPA_KEY_MGMT_PSK)||
        wpa_sm_set_param(s->rsn,WPA_PARAM_RSN_ENABLED,1)||wpa_sm_set_assoc_wpa_ie(s->rsn,own_rsn,sizeof(own_rsn))||
        wpa_sm_set_ap_rsn_ie(s->rsn,s->config.ap_rsn,s->config.ap_rsn_bytes))goto leave_fail;
    wpa_sm_notify_assoc(s->rsn,s->wifi->target.bssid);aic8800_supplicant_os_leave(s);return 1;
leave_fail:aic8800_supplicant_os_leave(s);
fail:aic8800_supplicant_stop(s);return 0;
}
void aic8800_supplicant_observe(struct aic8800_supplicant *s,uint64_t now)
{
    if(!s||!s->owner||s->state==AIC8800_SUPPLICANT_FAULT||s->state==AIC8800_SUPPLICANT_RETIRED)return;
    if(now<s->last_now||s->generation!=s->wifi->association_generation||
        (s->state!=AIC8800_SUPPLICANT_AUTHORIZED&&now>=s->deadline)||
        s->security->state==AIC8800_SECURITY_FAULT||s->data->state==AIC8800_DATA_FAULT){aic8800_supplicant_stop(s);return;}
    s->last_now=now;
    if(s->wifi->link!=AIC8800_WIFI_SECURITY_PENDING&&s->wifi->link!=AIC8800_WIFI_SECURED&&
        s->state!=AIC8800_SUPPLICANT_RETIRING){
        /* Idle link loss revokes authorization; explicit retirement owns deletes. */
        s->security->authorized=0;
        if(s->count||s->action_active||s->port_pending)aic8800_supplicant_stop(s);
    }
}
static uint64_t command_deadline(struct aic8800_supplicant *s,uint64_t now)
{
    if(s->config.command_timeout>UINT64_MAX-now)return 0;
    uint64_t deadline=now+s->config.command_timeout;
    if(deadline>s->deadline)deadline=s->deadline;
    if(deadline>s->owner->deadline)deadline=s->owner->deadline;
    return deadline;
}
static void pop(struct aic8800_supplicant *s)
{aic8800_security_zero(&s->actions[s->head],sizeof(s->actions[0]));s->head=(s->head+1)%AIC8800_SUPPLICANT_ACTIONS;--s->count;s->action_active=0;}
void aic8800_supplicant_step(struct aic8800_supplicant *s,uint64_t now)
{
    if(!s||!s->owner)return;
    aic8800_supplicant_observe(s,now);
    if(s->state==AIC8800_SUPPLICANT_FAULT||s->state==AIC8800_SUPPLICANT_RETIRED)return;
    uint64_t deadline=command_deadline(s,now);if(!deadline){aic8800_supplicant_stop(s);return;}
    if(s->state==AIC8800_SUPPLICANT_RETIRING){
        if(s->security->state==AIC8800_SECURITY_DONE){
            if(s->security->operation==AIC8800_SECURITY_PORT)s->retire_closed=1;
            s->security->state=AIC8800_SECURITY_IDLE;return;
        }
        if(s->security->state!=AIC8800_SECURITY_IDLE)return;
        if(!s->retire_closed){if(!aic8800_security_port(s->security,s->owner,0,now,deadline))aic8800_supplicant_stop(s);return;}
        for(unsigned i=0;i<AIC8800_SECURITY_KEYS;++i)if(s->security->keys[i].valid){
            if(!aic8800_security_delete(s->security,s->owner,i,now,deadline))aic8800_supplicant_stop(s);
            return;}
        if(!aic8800_supplicant_os_enter(s)){aic8800_supplicant_stop(s);return;}
        wpa_sm_deinit(s->rsn);s->rsn=0;aic8800_supplicant_os_leave(s);
        if(s->state==AIC8800_SUPPLICANT_FAULT)return;
        aic8800_security_zero(s->config.pmk,sizeof(s->config.pmk));aic8800_security_zero(s->small,sizeof(s->small));
        aic8800_security_zero(s->large,sizeof(s->large));aic8800_security_zero(s->used,sizeof(s->used));
        aic8800_security_zero(s->timers,sizeof(s->timers));
        s->security->retired=1;s->state=AIC8800_SUPPLICANT_RETIRED;return;
    }
    if(s->count){
        struct aic8800_supplicant_action *a=&s->actions[s->head];
        if(!s->action_active){
            int ok=a->kind==AIC8800_SUPPLICANT_EAPOL?
                aic8800_data_send(s->data,s->owner,a->bytes,a->length,now,deadline):
                aic8800_security_add(s->security,s->owner,a->pairwise,a->key_id,a->key,a->rsc,now,deadline);
            if(!ok){aic8800_supplicant_stop(s);return;}s->action_active=1;return;
        }
        if(a->kind==AIC8800_SUPPLICANT_EAPOL&&s->data->state==AIC8800_DATA_DONE){
            const struct aic8800_data_result *r=&s->data->result;
            if(!r->bus_transferred||!r->firmware_status_valid||!r->tx_done||!r->acknowledged||r->retry_required||r->software_retry_required)
                {aic8800_supplicant_stop(s);return;}
            s->data->state=AIC8800_DATA_IDLE;pop(s);
        }else if(a->kind==AIC8800_SUPPLICANT_KEY&&s->security->state==AIC8800_SECURITY_DONE){s->security->state=AIC8800_SECURITY_IDLE;pop(s);}
        return;
    }
    if(s->completed&&s->state!=AIC8800_SUPPLICANT_AUTHORIZED){
        if(!s->port_pending){
            if(!aic8800_security_port(s->security,s->owner,1,now,deadline)){aic8800_supplicant_stop(s);return;}
            s->port_pending=1;
        }else if(s->security->state==AIC8800_SECURITY_DONE){
            s->security->state=AIC8800_SECURITY_IDLE;s->port_pending=0;s->state=AIC8800_SUPPLICANT_AUTHORIZED;
        }
        return;
    }
    if(s->deferred_bytes&&s->data->state==AIC8800_DATA_IDLE) {
        struct aic8800_data_frame frame={.ethernet=s->deferred_rx,.bytes=s->deferred_bytes,
            .eapol=1,.encrypted=s->deferred_encrypted,.generation=s->generation};
        s->deferred_bytes=0;
        if(!aic8800_supplicant_receive(s,&frame,now))aic8800_supplicant_stop(s);
        aic8800_security_zero(s->deferred_rx,sizeof(s->deferred_rx));return;
    }
    if(!aic8800_supplicant_os_enter(s)){aic8800_supplicant_stop(s);return;}
    (void)aic8800_supplicant_os_timer(s);aic8800_supplicant_os_leave(s);
}
int aic8800_supplicant_receive(struct aic8800_supplicant *s,const struct aic8800_data_frame *frame,uint64_t now)
{
    if(!s||!frame||!frame->eapol||frame->bytes<18||frame->bytes>AIC8800_DATA_ETHERNET_BYTES||
        frame->generation!=s->generation||s->state==AIC8800_SUPPLICANT_RETIRING||
        s->state==AIC8800_SUPPLICANT_RETIRED)return 0;
    aic8800_supplicant_observe(s,now);if(s->state==AIC8800_SUPPLICANT_FAULT)return 0;
    if(os_memcmp(frame->ethernet+6,s->wifi->target.bssid,6))return 0;
    /* Retain receive order and keep the bus free for outstanding confirmations.
     * Excess AP retransmissions are discarded; the AP may retry them later. */
    if(s->deferred_bytes){if(s->dropped_eapol!=UINT32_MAX)++s->dropped_eapol;return 1;}
    if(s->count||s->action_active||s->port_pending||s->data->state!=AIC8800_DATA_IDLE) {
        os_memcpy(s->deferred_rx,frame->ethernet,frame->bytes);s->deferred_bytes=frame->bytes;
        s->deferred_encrypted=frame->encrypted;return 1;
    }
    if(s->state==AIC8800_SUPPLICANT_AUTHORIZED){
        uint64_t ticks=8*s->config.command_timeout;if(ticks>UINT64_MAX-now){aic8800_supplicant_stop(s);return 0;}
        s->deadline=now+ticks;if(s->deadline>s->owner->deadline)s->deadline=s->owner->deadline;
    }
    if(!aic8800_supplicant_os_enter(s)){aic8800_supplicant_stop(s);return 0;}
    (void)wpa_sm_rx_eapol(s->rsn,frame->ethernet+6,frame->ethernet+14,frame->bytes-14,
        frame->encrypted?FRAME_ENCRYPTED:FRAME_NOT_ENCRYPTED);
    aic8800_supplicant_os_leave(s);return s->state!=AIC8800_SUPPLICANT_FAULT;
}
int aic8800_supplicant_retire(struct aic8800_supplicant *s,uint64_t now,uint64_t deadline)
{
    if(!s||!s->owner||s->state==AIC8800_SUPPLICANT_FAULT||s->state==AIC8800_SUPPLICANT_RETIRED||
        s->state==AIC8800_SUPPLICANT_RETIRING||s->count||s->action_active||s->port_pending||s->deferred_bytes||
        s->data->state!=AIC8800_DATA_IDLE||s->security->state!=AIC8800_SECURITY_IDLE||deadline<=now||
        now<s->last_now||deadline>s->owner->deadline)return 0;
    s->last_now=now;s->deadline=deadline;s->security->authorized=0;s->security->retiring=1;
    s->state=AIC8800_SUPPLICANT_RETIRING;return 1;
}

int aic8800_supplicant_rearm(struct aic8800_supplicant *s,struct aic8800_security *sec,
    struct aic8800_sdio_transport *t,const struct aic8800_supplicant_config *cfg)
{
    if(!s||!sec||!t||!s->initialized||!sec->initialized||!t->wifi||!t->card||!t->card->host||
       !aic8800_transport_attachment_ready(t)||!aic8800_supplicant_config_valid(cfg)||
       overlap(s,sizeof(*s),cfg,sizeof(*cfg))||overlap(sec,sizeof(*sec),cfg,sizeof(*cfg))||
       s->owner!=t||s->security!=sec||sec->owner!=t||sec->supplicant!=s||
       s->data!=t->data||s->wifi!=t->wifi||sec->wifi!=t->wifi||
       s->generation!=sec->generation||s->generation==UINT64_MAX||t->wifi->association_generation!=s->generation+1||
       s->state!=AIC8800_SUPPLICANT_RETIRED||!s->retire_closed||!sec->retired||
       sec->state!=AIC8800_SECURITY_IDLE||sec->authorized||sec->port_confirmed||
       s->entered||s->rsn||s->count||s->action_active||s->port_pending||s->deferred_bytes||
       t->supplicant||t->security||t->data->security||!aic8800_transport_attachment_ready(t))return 0;
    for(unsigned i=0;i<AIC8800_SECURITY_KEYS;++i)if(sec->keys[i].valid)return 0;
    for(unsigned i=0;i<AIC8800_SUPPLICANT_TIMERS;++i)
        if(s->timers[i].handler||s->timers[i].context||s->timers[i].argument||s->timers[i].due)return 0;
    for(unsigned i=0;i<AIC8800_SUPPLICANT_SMALL_SLOTS+AIC8800_SUPPLICANT_LARGE_SLOTS;++i)if(s->used[i])return 0;
    aic8800_security_zero(sec,sizeof(*sec));aic8800_security_zero(s,sizeof(*s));
    return aic8800_security_init(sec)&&aic8800_supplicant_init(s,cfg);
}
