/* Bounded D80 key/controlled-port worker. The pinned firmware ABI uses no
 * receive replay-counter field; host packet numbers are therefore retained. */
#include "aic8800_security.h"
static uint64_t pn48(const uint8_t *p)
{uint64_t v=0;for(unsigned i=0;i<6;++i)v|=(uint64_t)p[i]<<(8*i);return v;}
static int equal(const uint8_t *a,const uint8_t *b,unsigned n)
{unsigned diff=0;for(unsigned i=0;i<n;++i)diff|=a[i]^b[i];return !diff;}
void aic8800_security_zero(void *p,size_t n)
{volatile uint8_t *q=p;while(n--)*q++=0;}
static int same_link(const struct aic8800_security *s)
{return s->wifi&&s->wifi->association_generation==s->generation&&s->wifi->state==AIC8800_WIFI_READY&&
    (s->wifi->link==AIC8800_WIFI_SECURITY_PENDING||s->wifi->link==AIC8800_WIFI_SECURED);}
static int active(const struct aic8800_security *s)
{return s->state==AIC8800_SECURITY_REQUEST||s->state==AIC8800_SECURITY_WAITING;}
void aic8800_security_stop(struct aic8800_security *s,const void *owner)
{if(s&&s->initialized&&s->owner==owner){s->authorized=0;s->state=AIC8800_SECURITY_FAULT;}}
void aic8800_security_observe(struct aic8800_security *s,const void *owner,uint64_t now)
{
    if(!s||s->owner!=owner||s->state==AIC8800_SECURITY_FAULT)return;
    if(now<s->last_now||(active(s)&&now>=s->deadline)||(s->completion_deadline&&now>=s->completion_deadline))
        {aic8800_security_stop(s,owner);return;}
    s->last_now=now;s->completion_deadline=0;
    if(!same_link(s)) {
        s->authorized=0;
        if(s->wifi->association_generation!=s->generation||(!s->retiring&&active(s)))aic8800_security_stop(s,owner);
    }
}
int aic8800_security_init(struct aic8800_security *s)
{if(!s||s->initialized)return 0;struct aic8800_security initial={0};initial.initialized=1;*s=initial;return 1;}
int aic8800_security_claim(struct aic8800_security *s,const void *owner,struct aic8800_wifi *w,
    struct aic8800_supplicant *supplicant,uint64_t now)
{
    if(!s||!s->initialized||s->owner||!owner||!w||w->owner!=owner||!supplicant||
        w->state!=AIC8800_WIFI_READY||w->link!=AIC8800_WIFI_SECURITY_PENDING||
        w->target.security!=AIC8800_WIFI_WPA2_PSK_CCMP||!w->association_generation)return 0;
    s->owner=owner;s->wifi=w;s->supplicant=supplicant;s->generation=w->association_generation;s->last_now=now;return 1;
}
static void encode(struct aic8800_security *s,uint16_t id,unsigned bytes)
{
    for(unsigned i=0;i<sizeof(s->request);++i)s->request[i]=0;
    aic8800_sdio_command_header(s->request,id,(uint16_t)bytes);
    s->expected_cfm=id+1;s->state=AIC8800_SECURITY_REQUEST;s->authorized=0;
}
static int admit(struct aic8800_security *s,const void *owner,uint64_t now,uint64_t deadline)
{
    return s&&s->initialized&&s->owner==owner&&s->state==AIC8800_SECURITY_IDLE&&!s->retired&&
        now>=s->last_now&&deadline>now&&s->wifi->association_generation==s->generation;
}
int aic8800_security_add(struct aic8800_security *s,const void *owner,unsigned pairwise,unsigned key_id,
    const uint8_t key[16],const uint8_t rsc[6],uint64_t now,uint64_t deadline)
{
    if(!admit(s,owner,now,deadline)||s->retiring||!same_link(s)||pairwise>1||key_id>3||
        (pairwise&&key_id)||!key||!rsc)return 0;
    unsigned slot=pairwise?0:1+key_id;
    if(s->keys[slot].valid&&equal(s->keys[slot].material,key,16)) {
        /* Already installed: do not reinstall or reset replay state. */
        s->state=AIC8800_SECURITY_DONE;return 1;
    }
    s->pending_key=(struct aic8800_security_key){.generation=s->generation,.pairwise=(uint8_t)pairwise,.key_id=(uint8_t)key_id};
    for(unsigned i=0;i<16;++i)s->pending_key.material[i]=key[i];
    for(unsigned i=0;i<AIC8800_SECURITY_TIDS;++i)s->pending_key.pn[i]=pn48(rsc);
    s->slot=(uint8_t)slot;s->operation=AIC8800_SECURITY_ADD;s->last_now=now;s->deadline=deadline;
    encode(s,36,44);uint8_t *p=s->request+16;p[0]=(uint8_t)key_id;p[1]=pairwise?s->wifi->ap_index:0xff;
    p[4]=16;for(unsigned i=0;i<16;++i)p[8+i]=key[i];p[40]=2;p[41]=s->wifi->vif;p[43]=(uint8_t)pairwise;return 1;
}
int aic8800_security_delete(struct aic8800_security *s,const void *owner,unsigned slot,uint64_t now,uint64_t deadline)
{
    if(!admit(s,owner,now,deadline)||!s->retiring||slot>=AIC8800_SECURITY_KEYS||!s->keys[slot].valid)return 0;
    s->slot=(uint8_t)slot;s->operation=AIC8800_SECURITY_DELETE;s->last_now=now;s->deadline=deadline;
    encode(s,38,1);s->request[16]=s->keys[slot].hardware_index;return 1;
}
int aic8800_security_port(struct aic8800_security *s,const void *owner,int opened,uint64_t now,uint64_t deadline)
{
    if(!admit(s,owner,now,deadline)||(opened!=0&&opened!=1)||(opened&&(s->retiring||!same_link(s))))return 0;
    unsigned group=0;for(unsigned i=1;i<AIC8800_SECURITY_KEYS;++i)group|=s->keys[i].valid;
    if(opened&&(!s->keys[0].valid||!group))return 0;
    s->operation=AIC8800_SECURITY_PORT;s->requested_open=(uint8_t)opened;s->last_now=now;s->deadline=deadline;
    encode(s,5124,2);s->request[16]=s->wifi->ap_index;s->request[17]=(uint8_t)opened;return 1;
}
int aic8800_security_sent(struct aic8800_security *s,const void *owner)
{if(!s||s->owner!=owner||s->state!=AIC8800_SECURITY_REQUEST)return 0;s->state=AIC8800_SECURITY_WAITING;return 1;}
enum aic8800_codec_result aic8800_security_response(struct aic8800_security *s,const void *owner,
    const struct aic8800_sdio_response *r,uint64_t now)
{
    if(!s||s->owner!=owner||!r)return AIC8800_CODEC_UNRELATED;
    aic8800_security_observe(s,owner,now);if(s->state==AIC8800_SECURITY_FAULT)return AIC8800_CODEC_MALFORMED;
    if(s->state!=AIC8800_SECURITY_WAITING||!r->has_message||r->message_id!=s->expected_cfm)return AIC8800_CODEC_UNRELATED;
    if(r->parameter_bytes>sizeof(s->last_cfm)||(s->operation==AIC8800_SECURITY_ADD&&r->parameter_bytes!=4))goto fail;
    s->last_cfm_bytes=r->parameter_bytes;for(unsigned i=0;i<r->parameter_bytes;++i)s->last_cfm[i]=r->parameters[i];
    if(s->operation==AIC8800_SECURITY_ADD) {
        if(r->parameters[0])goto fail;
        for(unsigned i=0;i<AIC8800_SECURITY_KEYS;++i)if(i!=s->slot&&s->keys[i].valid&&s->keys[i].hardware_index==r->parameters[1])goto fail;
        s->pending_key.hardware_index=r->parameters[1];s->pending_key.valid=1;s->keys[s->slot]=s->pending_key;
        aic8800_security_zero(&s->pending_key,sizeof(s->pending_key));
    } else if(s->operation==AIC8800_SECURITY_DELETE) aic8800_security_zero(&s->keys[s->slot],sizeof(s->keys[0]));
    else {
        s->port_confirmed=s->requested_open;s->authorized=s->requested_open;
        if(s->requested_open)s->wifi->link=AIC8800_WIFI_SECURED;
    }
    aic8800_security_zero(s->request,sizeof(s->request));
    s->completion_deadline=s->deadline;s->state=AIC8800_SECURITY_DONE;return AIC8800_CODEC_OK;
fail:aic8800_security_stop(s,owner);return AIC8800_CODEC_MALFORMED;
}
int aic8800_security_open(const struct aic8800_security *s)
{return s&&s->initialized&&s->state!=AIC8800_SECURITY_FAULT&&s->authorized&&s->port_confirmed&&same_link(s);}
int aic8800_security_rx_key(struct aic8800_security *s,unsigned group,unsigned key_id,unsigned tid,uint64_t pn,unsigned *slot)
{
    if(!s||s->state==AIC8800_SECURITY_FAULT||!same_link(s)||group>1||key_id>3||tid>=AIC8800_SECURITY_TIDS||!pn||!slot||(!group&&key_id))return 0;
    unsigned at=group?1+key_id:0;const struct aic8800_security_key *k=&s->keys[at];
    if(!k->valid||k->generation!=s->generation||pn<=k->pn[tid])return 0;
    *slot=at;return 1;
}
void aic8800_security_rx_commit(struct aic8800_security *s,unsigned slot,unsigned tid,uint64_t pn)
{if(s&&slot<AIC8800_SECURITY_KEYS&&tid<AIC8800_SECURITY_TIDS&&pn>s->keys[slot].pn[tid])s->keys[slot].pn[tid]=pn;}
