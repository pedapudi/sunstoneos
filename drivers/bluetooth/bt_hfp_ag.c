/* SPDX-License-Identifier: Apache-2.0 */
#include "bt_hfp_ag.h"
#include "bt_memory.h"
static uint64_t stamp(const struct bt_hfp_ag *q){return q->rfcomm->l2cap->acl->hci->transport.now;}
static int fail(struct bt_hfp_ag *q,enum bt_hfp_ag_error e)
{q->error=e;q->phase=BT_HFP_AG_FAULT;return -1;}
static int values(const uint8_t *p)
{return p && p[0]<=1 && p[1]<=1 && p[2]<=3 && p[3]<=2 && p[4]<=5 && p[5]<=1 && p[6]<=5;}
static int identity(const struct bt_hfp_ag *q)
{return q->rfcomm && q->rfcomm->registry==q->registry && q->rfcomm->link==q->link && q->rfcomm->channel==q->channel;}
static int external(const struct bt_hfp_ag *q,const void *p,size_t n)
{
    const struct bt_h4 *h=&q->rfcomm->l2cap->acl->hci->transport;
    if(!bt_range_disjoint(q,sizeof(*q),p,n) || !bt_range_disjoint(q->rfcomm,sizeof(*q->rfcomm),p,n) ||
       !bt_range_disjoint(q->registry,sizeof(*q->registry),p,n) || !bt_range_disjoint(q->rfcomm->l2cap,sizeof(*q->rfcomm->l2cap),p,n) ||
       !bt_range_disjoint(q->rfcomm->l2cap->acl,sizeof(*q->rfcomm->l2cap->acl),p,n) ||
       !bt_range_disjoint(q->rfcomm->l2cap->acl->hci,sizeof(*q->rfcomm->l2cap->acl->hci),p,n) ||
       !bt_range_disjoint(h->binding.uart,sizeof(*h->binding.uart),p,n))return 0;
    for(unsigned i=0;i<h->count;++i)if(!bt_range_disjoint(h->records[i].storage.bytes,h->records[i].storage.capacity,p,n))return 0;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(q->registry->session[i] && !bt_range_disjoint(q->registry->session[i],sizeof(struct bt_rfcomm),p,n))return 0;
    return 1;
}
int bt_hfp_ag_init_calls(struct bt_hfp_ag *q,struct bt_rfcomm *r,const uint8_t *p,uint64_t deadline,uint64_t timeout,
    unsigned codecs,uint64_t ring)
{
    if((codecs&&codecs!=6) || ring>60000000 || (ring && p && (p[1]||p[2]||p[3])) || !q || !r || !p || !bt_range_disjoint(q,sizeof(*q),r,sizeof(*r)) || !bt_range_disjoint(q,sizeof(*q),p,7) ||
       q->rfcomm || q->phase!=BT_HFP_AG_OFF || r->phase!=BT_RFCOMM_OPEN || !r->registry || !r->l2cap ||
       (r->incoming ? r->service_uuid!=0x111f : r->service_uuid!=0x111e) || !timeout || timeout>60000000 ||
       !values(p) || deadline<=r->l2cap->acl->hci->transport.now)return -1;
    struct bt_hfp_ag temp={.rfcomm=r,.registry=r->registry,.link=r->link,.channel=r->channel,
        .setup_deadline=deadline,.timeout_us=timeout,.now=r->l2cap->acl->hci->transport.now,
        .active=127,.pending_active=127,.local_codecs=codecs,.calls=!!ring,.ring_us=ring,.phase=BT_HFP_AG_INITIALIZING};
    if(!external(&temp,p,7) || !external(&temp,q,sizeof(*q)))return -1;
    for(unsigned i=0;i<BT_RFCOMM_DATA;++i)if(r->tx[i].state!=BT_RFCOMM_FREE)return -1;
    if(r->registry->authorize_service(r->registry->context,r->link,r->service_uuid,r->server)!=1)return -1;
    for(unsigned i=0;i<7;++i)temp.indicators[i]=temp.notified[i]=p[i];
    *q=temp;return 1;
}
int bt_hfp_ag_init_codecs(struct bt_hfp_ag *q,struct bt_rfcomm *r,const uint8_t *p,uint64_t d,uint64_t t,unsigned codecs)
{return bt_hfp_ag_init_calls(q,r,p,d,t,codecs,0);}
int bt_hfp_ag_init(struct bt_hfp_ag *q,struct bt_rfcomm *r,const uint8_t *p,uint64_t d,uint64_t t)
{return bt_hfp_ag_init_calls(q,r,p,d,t,0,0);}
static void text(struct bt_hfp_ag *q,const char *s)
{while(*s && q->response_length<BT_HFP_AG_RESPONSE)q->response[q->response_length++]=(uint8_t)*s++;}
static void number(struct bt_hfp_ag *q,uint32_t x)
{char p[10];unsigned n=0;do{p[n++]=(char)('0'+x%10);x/=10;}while(x);while(n)q->response[q->response_length++]=(uint8_t)p[--n];}
static int equal(const uint8_t *p,size_t n,const char *s)
{size_t i=0;for(;s[i];++i)if(i>=n || p[i]!=(uint8_t)s[i])return 0;return i==n;}
static int prefix(const uint8_t *p,size_t n,const char *s,size_t *at)
{size_t i=0;for(;s[i];++i)if(i>=n || p[i]!=(uint8_t)s[i])return 0;*at=i;return 1;}
static int decimal(const uint8_t *p,size_t n,size_t *at,uint32_t *v)
{
    size_t start=*at;uint32_t x=0;
    while(*at<n && p[*at]>='0' && p[*at]<='9'){
        unsigned d=p[*at]-'0';if(x>(UINT32_MAX-d)/10)return 0;x=x*10+d;++*at;}
    if(*at==start)return 0;*v=x;return 1;
}
static int bia(const uint8_t *p,size_t n,size_t at,unsigned *mask)
{
    unsigned index=0,m=*mask;
    while(at<n){if(p[at]!=','){
        if(p[at]!='0' && p[at]!='1')return 0;
        if(index<7){if(p[at]=='1')m|=1u<<index;else m&=~(1u<<index);}++at;
        if(at<n && p[at]!=',')return 0;}
        if(at<n)++at;++index;}
    *mask=m|14;return 1;
}
static int cmer(const uint8_t *p,size_t n,size_t at,unsigned *mode,unsigned *ind)
{
    /* Omitted values retain state; keyp/disp/bfr are irrelevant to HFP. */
    uint32_t x;unsigned field=0;
    for(;;){
        if(field>=5)return 0;
        if(at<n && p[at]!=','){
            if(!decimal(p,n,&at,&x))return 0;
            if((field==0 && x!=3) || (field==3 && x>1))return 0;
            if(field==0)*mode=x;
            if(field==3)*ind=x;
        }
        if(at==n)break;
        if(p[at++]!=',')return 0;
        ++field;
    }
    return *mode==3;
}
/* Every response starts from committed state; acceptance commits pending state. */
static void hold(struct bt_hfp_ag *q)
{
    q->pending_stage=q->stage;q->pending_features=q->hf_features;
    q->pending_codecs=q->peer_codecs;q->pending_codec=q->selected_codec;
    q->pending_reporting=q->reporting;q->pending_active=q->active;q->pending_mode=q->cmer_mode;
    q->pending_control=q->control;
}
/* 27.007 dial digits; HFP numbers are quoted, so no quote can be admitted. */
static int dial(const uint8_t *p,size_t n)
{
    if(!n || n>BT_HFP_AG_NUMBER)return 0;
    for(size_t i=0;i<n;++i)if(!((p[i]>='0'&&p[i]<='9')||p[i]=='*'||p[i]=='#'||p[i]=='+'||(p[i]>='A'&&p[i]<='D')))return 0;
    return 1;
}
static void quoted(struct bt_hfp_ag *q,const struct bt_hfp_ag_call *c)
{text(q,"\"");for(unsigned i=0;i<c->length;++i)q->response[q->response_length++]=c->number[i];text(q,"\",");number(q,c->type);}
/* The call indicators follow the single call; callheld stays zero. */
static void call_indicators(struct bt_hfp_ag *q)
{
    const uint8_t value[3]={(uint8_t)q->control.call.call,(uint8_t)q->control.call.setup,0};
    for(unsigned i=1;i<4;++i){q->indicators[i]=value[i-1];
        if(q->indicators[i]!=q->notified[i])q->changed|=1u<<i;else q->changed&=~(1u<<i);}
}
/* Call-control commands after service-level setup. Returns1 OK, otherwise
 * ERROR; *cme selects a +CME code once the HF has enabled extended errors. */
static int calls(struct bt_hfp_ag *q,const uint8_t *p,size_t n,unsigned *cme)
{
    struct bt_hfp_ag_control *c=&q->pending_control;struct bt_hfp_ag_call *call=&c->call;
    size_t at=0;uint32_t value=0;unsigned busy=call->call||call->setup;
    if(prefix(p,n,"AT+CMEE=",&at) && decimal(p,n,&at,&value) && at==n && value<=1){c->cmee=value;return 1;}
    if(prefix(p,n,"AT+CLIP=",&at) && decimal(p,n,&at,&value) && at==n && value<=1){c->clip=value;return 1;}
    if(prefix(p,n,"AT+CCWA=",&at) && decimal(p,n,&at,&value) && at==n && value<=1){c->ccwa=value;return 1;}
    if(equal(p,n,"AT+COPS=3,0")){c->format=1;return 1;}
    /* No network operator exists, so mode0 is reported without a name. */
    if(equal(p,n,"AT+COPS?")){text(q,"\r\n+COPS: 0\r\n");return 1;}
    if(equal(p,n,"AT+CNUM"))return 1;
    if(equal(p,n,"AT+CLCC")){
        if(busy){text(q,"\r\n+CLCC: 1,");number(q,call->direction);text(q,",");
            number(q,call->call?0:call->setup==1?4:call->setup);text(q,",0,0");
            if(call->length){text(q,",");quoted(q,call);}text(q,"\r\n");}
        return 1;
    }
    if(equal(p,n,"AT+BCC")){
        if(!q->local_codecs || !(q->hf_features&128) || !q->peer_codecs || q->offered_codec){*cme=3;return 0;}
        ++c->audio_requests;return 1;
    }
    if(equal(p,n,"ATA")){if(call->setup!=1 || call->call){*cme=3;return 0;}call->call=1;call->setup=0;return 1;}
    if(equal(p,n,"AT+CHUP")){if(!busy){*cme=3;return 0;}*call=(struct bt_hfp_ag_call){0};return 1;}
    if(prefix(p,n,"AT+VTS=",&at)){
        if(n-at!=1 || !dial(p+at,1) || p[at]=='+')return 0;
        if(!call->call){*cme=3;return 0;}
        c->dtmf=p[at];++c->dtmf_count;return 1;
    }
    const struct bt_hfp_ag_call *source=0;struct bt_hfp_ag_call dialed={0};
    /* No phone book exists, so every memory location is empty. */
    if(prefix(p,n,"ATD>",&at))return 0;
    if(equal(p,n,"AT+BLDN")){if(!c->last.length)return 0;source=&c->last;}
    else if(prefix(p,n,"ATD",&at)){
        if(n<5 || p[n-1]!=';' || !dial(p+at,n-at-1))return 0;
        dialed.length=(unsigned)(n-at-1);dialed.type=p[at]=='+'?145:129;
        for(unsigned i=0;i<dialed.length;++i)dialed.number[i]=p[at+i];source=&dialed;
    } else return 0;
    if(busy){*cme=3;return 0;}
    if(!q->indicators[0]){*cme=30;return 0;}
    *call=*source;call->direction=0;call->call=0;call->setup=2;c->last=*call;return 1;
}
static void response(struct bt_hfp_ag *q)
{
    const uint8_t *p=q->line;size_t n=q->line_length,at=0;uint32_t value=0;unsigned ok=0,cme=0;
    hold(q);q->notify_index=0;
    if(!q->discard && prefix(p,n,"AT+BRSF=",&at) && decimal(p,n,&at,&value) && at==n && !q->stage){
        q->pending_features=value;q->pending_stage=1;text(q,"\r\n+BRSF: ");
        number(q,(q->local_codecs?512u:0u)|(q->calls?BT_HFP_AG_CALL_FEATURES:0u));text(q,"\r\n");ok=1;
    }else if(!q->discard && q->stage && q->local_codecs && (q->hf_features&128) && prefix(p,n,"AT+BAC=",&at)){
        unsigned mask=0;ok=1;
        do {if(!decimal(p,n,&at,&value)||!value||value>255){ok=0;break;}
            if(value<3)mask|=1u<<value;
            if(at==n)break;if(p[at++]!=','||at==n){ok=0;break;}
        }while(at<n);
        if(!(mask&2))ok=0;
        /* HFP lets the HF resend its list at any time. A selection that the
         * new list still contains survives; otherwise reselection is required. */
        if(ok){q->pending_codecs=mask;q->pending_codec=mask&(1u<<q->selected_codec)?q->selected_codec:0;
            q->offered_codec=0;q->codec_deadline=0;}
    }else if(!q->discard && q->stage>=4 && q->offered_codec && prefix(p,n,"AT+BCS=",&at) &&
             decimal(p,n,&at,&value) && at==n && value==q->offered_codec && (q->peer_codecs&(1u<<value))){
        q->pending_codec=value;ok=1;
    }else if(!q->discard && q->stage && (!(q->local_codecs&&(q->hf_features&128))||q->peer_codecs) && equal(p,n,"AT+CIND=?")){
        text(q,"\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),(\"callheld\",(0-2)),(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))\r\n");
        if(q->stage<2)q->pending_stage=2;ok=1;
    }else if(!q->discard && q->stage>=2 && equal(p,n,"AT+CIND?")){
        text(q,"\r\n+CIND: ");for(unsigned i=0;i<7;++i){if(i)text(q,",");number(q,q->indicators[i]);}
        text(q,"\r\n");if(q->stage<3)q->pending_stage=3;ok=1;
    }else if(!q->discard && q->stage>=3 && prefix(p,n,"AT+CMER=",&at) && cmer(p,n,at,&q->pending_mode,&q->pending_reporting)){
        if(q->pending_reporting)q->pending_stage=4;ok=1;
    }else if(!q->discard && q->stage>=2 && prefix(p,n,"AT+BIA=",&at) && bia(p,n,at,&q->pending_active))ok=1;
    else if(!q->discard && q->stage>=4 && prefix(p,n,"AT+VGM=",&at) && decimal(p,n,&at,&value) && at==n && value<=15){
        q->microphone_gain=value;q->gain_valid|=1;ok=1;
    }else if(!q->discard && q->stage>=4 && prefix(p,n,"AT+VGS=",&at) && decimal(p,n,&at,&value) && at==n && value<=15){
        q->speaker_gain=value;q->gain_valid|=2;ok=1;
    }else if(!q->discard && q->stage>=4 && q->calls)ok=(unsigned)calls(q,p,n,&cme);
    if(!ok){hold(q);q->response_length=0;}
    if(!ok && cme && q->control.cmee){text(q,"\r\n+CME ERROR: ");number(q,cme);text(q,"\r\n");}
    else text(q,ok ? "\r\nOK\r\n" : "\r\nERROR\r\n");
    q->response_deadline=a733_deadline_after_saturating(q->now,q->timeout_us);q->line_length=0;q->discard=0;q->line_deadline=0;
}
static void accepted(struct bt_hfp_ag *q)
{
    q->stage=q->pending_stage;q->hf_features=q->pending_features;
    q->peer_codecs=q->pending_codecs;q->selected_codec=q->pending_codec;
    if(q->selected_codec){q->codec_deadline=0;q->offered_codec=0;}
    q->reporting=q->pending_reporting;q->active=q->pending_active;q->cmer_mode=q->pending_mode;
    if(q->calls){q->control=q->pending_control;call_indicators(q);}
    if(q->notify_index){unsigned i=q->notify_index-1;q->notified[i]=(uint8_t)q->notify_value;
        if(q->notified[i]==q->indicators[i])q->changed&=~(1u<<i);}
    if(q->stage==4 && q->phase==BT_HFP_AG_INITIALIZING)q->phase=BT_HFP_AG_READY;
    q->response_accepted=1;
}
static int tx_result(struct bt_hfp_ag *q)
{
    if(!q->transmit)return 1;
    struct bt_rfcomm_buffer *b=0;
    for(unsigned i=0;i<BT_RFCOMM_DATA;++i)if(q->rfcomm->tx[i].serial==q->transmit && q->rfcomm->tx[i].state!=BT_RFCOMM_FREE)b=&q->rfcomm->tx[i];
    if(!b)return fail(q,BT_HFP_AG_IDENTITY);
    if(b->accepted && q->response_cursor+q->chunk==q->response_length && !q->response_accepted &&
       q->phase!=BT_HFP_AG_CLOSING)accepted(q);
    if(b->state==BT_RFCOMM_RETIRED || b->state==BT_RFCOMM_CANCELLED){
        unsigned failed=b->state==BT_RFCOMM_CANCELLED;
        if(failed && q->phase!=BT_HFP_AG_CLOSING)return fail(q,BT_HFP_AG_LOWER);
        if(bt_rfcomm_release_result(q->rfcomm,q->transmit)!=1)return fail(q,BT_HFP_AG_LOWER);
        q->transmit=0;q->response_cursor+=q->chunk;q->chunk=0;
        if(q->response_cursor==q->response_length){q->response_cursor=0;q->response_length=0;q->response_accepted=0;}
    }
    return 1;
}
static int input(struct bt_hfp_ag *q,unsigned budget)
{
    const struct bt_rfcomm_buffer *b=bt_rfcomm_receive(q->rfcomm);
    if(!b)return q->receive ? fail(q,BT_HFP_AG_IDENTITY) : 1;
    if(q->receive && q->receive!=b->serial)return fail(q,BT_HFP_AG_IDENTITY);
    q->receive=b->serial;
    while(budget-- && q->receive_cursor<b->length && !q->response_length){
        uint8_t c=b->bytes[q->receive_cursor++];
        if(c=='\r'){response(q);break;}
        if(c=='\n' && !q->line_length && !q->discard)continue;
        if(!q->line_deadline)q->line_deadline=a733_deadline_after_saturating(q->now,q->timeout_us);
        if(c<' ' || c>126){q->discard=1;continue;}
        if(q->discard || c==' ')continue;
        if(q->line_length==BT_HFP_AG_LINE){q->discard=1;continue;}
        if(c>='a' && c<='z')c=(uint8_t)(c-'a'+'A');
        q->line[q->line_length++]=c;
    }
    if(q->receive_cursor==b->length){if(bt_rfcomm_release_receive(q->rfcomm,b->serial)!=1)return fail(q,BT_HFP_AG_LOWER);
        q->receive=0;q->receive_cursor=0;}
    return 1;
}
static int publish(struct bt_hfp_ag *q)
{
    if(q->transmit || !q->response_length)return 1;
    size_t n=q->response_length-q->response_cursor;if(n>q->rfcomm->mtu)n=q->rfcomm->mtu;
    uint64_t serial=0;int r=bt_rfcomm_send(q->rfcomm,q->response+q->response_cursor,n,q->response_deadline,&serial);
    if(r<0)return fail(q,BT_HFP_AG_LOWER);
    if(r==1){q->transmit=serial;q->chunk=n;}return 1;
}
int bt_hfp_ag_select_codec(struct bt_hfp_ag *q,unsigned codec)
{
    if(!q||!identity(q)||q->phase!=BT_HFP_AG_READY||!q->local_codecs||!(q->hf_features&128)||
       codec<1||codec>2||!(q->peer_codecs&q->local_codecs&(1u<<codec)))return -1;
    if(q->response_length||q->line_deadline||q->offered_codec)return 0;
    hold(q);q->selected_codec=q->pending_codec=0;q->offered_codec=codec;
    q->codec_deadline=a733_deadline_after_saturating(q->now,q->timeout_us);
    text(q,"\r\n+BCS: ");number(q,codec);text(q,"\r\n");q->response_deadline=q->codec_deadline;return 1;
}
static void notify(struct bt_hfp_ag *q)
{
    if(q->phase!=BT_HFP_AG_READY)return;
    if(q->reporting)for(unsigned i=0;i<7;++i)if((q->changed&q->active)&(1u<<i)){
        hold(q);text(q,"\r\n+CIEV: ");number(q,i+1);text(q,",");number(q,q->indicators[i]);text(q,"\r\n");
        q->notify_value=q->indicators[i];q->notify_index=i+1;
        q->response_deadline=a733_deadline_after_saturating(q->now,q->timeout_us);return;}
    /* Any unreported callsetup change returned above, so RING follows the
     * reported callsetup1, or starts at once without reports. */
    const struct bt_hfp_ag_call *c=&q->control.call;
    if(q->calls && c->setup==1 && q->now>=q->next_ring){
        hold(q);q->notify_index=0;text(q,"\r\nRING\r\n");
        if(q->control.clip && c->length){text(q,"\r\n+CLIP: ");quoted(q,c);text(q,"\r\n");}
        q->next_ring=a733_deadline_after_saturating(q->now,q->ring_us);q->response_deadline=a733_deadline_after_saturating(q->now,q->timeout_us);
    }
}
int bt_hfp_ag_step(struct bt_hfp_ag *q,unsigned budget,unsigned bytes,uint64_t deadline)
{
    if(!q || !q->rfcomm || !budget || budget>64 || !bytes || bytes>512 || q->phase==BT_HFP_AG_OFF || q->phase==BT_HFP_AG_FAULT)return -1;
    if(!identity(q))return fail(q,BT_HFP_AG_IDENTITY);
    if(q->phase==BT_HFP_AG_CLOSED)return 0;
    uint64_t now=stamp(q);if(now<q->now)return fail(q,BT_HFP_AG_LOWER);q->now=now;
    if(q->phase!=BT_HFP_AG_CLOSING && ((q->codec_deadline && now>=q->codec_deadline) || (q->phase==BT_HFP_AG_INITIALIZING && now>=q->setup_deadline) ||
       (q->line_deadline && now>=q->line_deadline) || (q->response_length && !q->response_accepted && now>=q->response_deadline)))return fail(q,BT_HFP_AG_EXPIRED);
    if(bt_rfcomm_step(q->rfcomm,budget,deadline)<0)return fail(q,BT_HFP_AG_LOWER);
    now=stamp(q);if(now<q->now)return fail(q,BT_HFP_AG_LOWER);q->now=now;
    if(q->phase!=BT_HFP_AG_CLOSING && q->rfcomm->phase!=BT_RFCOMM_OPEN){
        if(q->rfcomm->phase==BT_RFCOMM_CLOSING || q->rfcomm->phase==BT_RFCOMM_CLOSED)q->phase=BT_HFP_AG_CLOSING;
        else return fail(q,BT_HFP_AG_LOWER);}
    if(q->phase!=BT_HFP_AG_CLOSING && ((q->codec_deadline && now>=q->codec_deadline) || (q->phase==BT_HFP_AG_INITIALIZING && now>=q->setup_deadline) ||
       (q->line_deadline && now>=q->line_deadline) || (q->response_length && !q->response_accepted && now>=q->response_deadline)))return fail(q,BT_HFP_AG_EXPIRED);
    if(tx_result(q)<0)return -1;
    if(q->phase==BT_HFP_AG_CLOSING){
        if(q->rfcomm->phase==BT_RFCOMM_CLOSED && !q->transmit){
            const struct bt_rfcomm_buffer *b=bt_rfcomm_receive(q->rfcomm);
            if(b){if(bt_rfcomm_release_receive(q->rfcomm,b->serial)!=1)return fail(q,BT_HFP_AG_LOWER);return 1;}
            q->receive=0;q->receive_cursor=0;q->response_length=0;q->phase=BT_HFP_AG_CLOSED;return 0;}
        return 1;}
    if(!q->response_length && input(q,bytes)<0)return -1;
    if(!q->response_length && !q->line_deadline)notify(q);
    return publish(q);
}
int bt_hfp_ag_call(struct bt_hfp_ag *q,unsigned event,const char *text_number,unsigned type)
{
    if(!q || !q->rfcomm || !q->calls || q->phase!=BT_HFP_AG_READY || !identity(q) ||
       (event!=BT_HFP_AG_CALL_INCOMING && (text_number || type)))return -1;
    struct bt_hfp_ag_call next=q->control.call;
    if(event==BT_HFP_AG_CALL_INCOMING){
        size_t n=0;if(text_number){if(!external(q,text_number,1))return -1;
            while(n<=BT_HFP_AG_NUMBER && text_number[n])++n;
            if(!external(q,text_number,n+1))return -1;}
        if(next.call || next.setup || (text_number && (!dial((const uint8_t *)text_number,n) || type<128 || type>175)) ||
           (!text_number && type))return -1;
        next=(struct bt_hfp_ag_call){.length=(unsigned)n,.type=type,.direction=1,.setup=1};
        for(size_t i=0;i<n;++i)next.number[i]=(uint8_t)text_number[i];
    } else if(event==BT_HFP_AG_CALL_ALERTING){if(next.setup!=2)return -1;next.setup=3;}
    else if(event==BT_HFP_AG_CALL_ACTIVE){if(!next.setup || next.call)return -1;next.setup=0;next.call=1;}
    else if(event==BT_HFP_AG_CALL_END){if(!next.call && !next.setup)return -1;next=(struct bt_hfp_ag_call){0};}
    else return -1;
    if(q->response_length)return 0;
    q->control.call=next;q->pending_control=q->control;call_indicators(q);
    if(event==BT_HFP_AG_CALL_INCOMING)q->next_ring=q->now;return 1;
}
int bt_hfp_ag_indicators(struct bt_hfp_ag *q,const uint8_t *p)
{
    if(!q || !q->rfcomm || (q->phase!=BT_HFP_AG_INITIALIZING && q->phase!=BT_HFP_AG_READY) ||
       !external(q,p,7) || !values(p) || !identity(q))return -1;
    if(q->calls && (p[1]!=q->control.call.call || p[2]!=q->control.call.setup || p[3]))return -1;
    for(unsigned i=0;i<7;++i){q->indicators[i]=p[i];if(p[i]!=q->notified[i])q->changed|=1u<<i;else q->changed&=~(1u<<i);}return 1;
}
int bt_hfp_ag_close(struct bt_hfp_ag *q,uint64_t deadline)
{
    if(!q || !q->rfcomm || !identity(q) || q->phase==BT_HFP_AG_CLOSED || q->phase==BT_HFP_AG_OFF)return -1;
    if(bt_rfcomm_close(q->rfcomm,deadline)!=1)return -1;q->phase=BT_HFP_AG_CLOSING;return 1;
}
int bt_hfp_ag_release(struct bt_hfp_ag *q)
{
    if(!q || q->phase!=BT_HFP_AG_CLOSED || q->transmit || q->receive || !identity(q) || q->rfcomm->phase!=BT_RFCOMM_CLOSED)return -1;
    *q=(struct bt_hfp_ag){0};return 1;
}
