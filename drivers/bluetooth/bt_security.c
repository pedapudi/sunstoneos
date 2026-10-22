/* SPDX-License-Identifier: Apache-2.0 */
#include "bt_security.h"
#include "bt_memory.h"
#include "a733_range.h"
static struct bt_security *active;
const bt_addr_t bt_security_any_address={{0}};
static int fault(struct bt_security *s)
{if(s&&s->self==s){atomic_fetch_or_explicit(s->recovery,4,memory_order_acq_rel);s->fatal=1;}return -1;}
static int owner(const struct bt_security *s)
{return s&&s->self==s&&s==active&&xTaskGetCurrentTaskHandle()==s->owner;}
static int live(struct bt_security *s)
{
    if(!owner(s)||s->fatal||(atomic_load_explicit(s->recovery,memory_order_acquire)&4)||
        s->acl->state!=BT_ACL_ACTIVE||s->acl->hci->security_owner!=s||s->acl->security_context!=s)return 0;
    struct bt_h4 *h=&s->acl->hci->transport;
    if(h->binding.firmware_generation!=s->firmware_generation||h->binding.uart_generation!=s->uart_generation||
        h->binding.current(h->binding.context,s->firmware_generation,s->uart_generation)!=1||
        !a733_deadline_admit(h->now,&s->now,s->deadline))return 0;
    return 1;
}
static struct bt_acl_link *actual(struct bt_security *s,const struct bt_conn *c)
{
    for(unsigned i=0;i<BT_ACL_LINKS;++i){struct bt_acl_link *l=&s->acl->links[i];
        if(l->generation==c->acl_generation&&!memcmp(l->peer.address,c->br.dst.val,6)&&
            l->state!=BT_ACL_UNUSED&&l->state!=BT_ACL_CLOSED&&
            (!c->authenticated||(l->state==BT_ACL_CONNECTED&&l->handle_valid&&l->handle==c->handle)))return l;}
    return NULL;
}
static struct bt_conn *connection(struct bt_security *s,uint64_t generation)
{for(unsigned i=0;i<BT_ACL_LINKS;++i)if(s->connections[i].attached&&s->connections[i].acl_generation==generation)return &s->connections[i];return NULL;}
int bt_security_retain(struct bt_security *s)
{if(!owner(s))return -1;return fault(s);}
/* Upstream port functions execute only synchronously inside this sole owner.
 * Ref counts guard callback/lookup borrows, never free or reclaim connections. */
struct bt_conn *bt_conn_lookup_addr_br(const bt_addr_t *address)
{
    struct bt_security *s=active;if(!live(s))return NULL;
    for(unsigned i=0;i<BT_ACL_LINKS;++i){struct bt_conn *c=&s->connections[i];
        if(c->attached&&bt_addr_eq(&c->br.dst,address)&&actual(s,c)){++c->references;return c;}}
    return NULL;
}
struct bt_conn *bt_conn_lookup_handle(uint16_t handle,unsigned type)
{
    struct bt_security *s=active;if(type!=BT_CONN_TYPE_BR||!live(s))return NULL;
    for(unsigned i=0;i<BT_ACL_LINKS;++i){struct bt_conn *c=&s->connections[i];struct bt_acl_link *l=actual(s,c);
        if(c->attached&&l&&l->state==BT_ACL_CONNECTED&&l->handle_valid&&l->handle==handle){c->handle=handle;++c->references;return c;}}
    return NULL;
}
void bt_conn_unref(struct bt_conn *c)
{if(!owner(active)||!c||!c->references){(void)fault(active);return;}--c->references;}
bool bt_conn_is_type(struct bt_conn *c,unsigned type){return c&&c->attached&&type==BT_CONN_TYPE_BR;}
int bt_conn_disconnect(struct bt_conn *c,uint8_t reason)
{(void)c;(void)reason;return fault(active);}
void bt_conn_security_changed(struct bt_conn *c,uint8_t status,enum bt_security_err error)
{(void)c;(void)error;if(status)(void)fault(active);}
enum bt_security_err bt_security_err_get(uint8_t status)
{return status?BT_SECURITY_ERR_AUTHENTICATION:BT_SECURITY_ERR_SUCCESS;}
unsigned bt_l2cap_br_get_max_sec_level(void){return active->policy.minimum_level;}
bool br_sufficient_key_size(struct bt_conn *c)
{
    /* Adapted from Zephyr br.c br_sufficient_key_size: the blocking command
     * has already completed for this exact live handle before this query. */
    return c->key_verified&&(c->required_sec_level==BT_SECURITY_L4?c->key_size==16:c->key_size>=16);
}
struct net_buf *bt_hci_cmd_alloc(int timeout)
{
    (void)timeout;struct bt_security *s=active;
    if(!live(s)||s->allocated||s->count>=BT_SECURITY_COMMANDS){(void)fault(s);return NULL;}
    s->allocation=(struct net_buf){0};s->allocation.data=s->allocation.bytes;s->allocated=1;return &s->allocation;
}
void *net_buf_add(struct net_buf *b,size_t n)
{
    if(!owner(active)||b!=&active->allocation||!active->allocated||n>sizeof(b->bytes)-b->len){(void)fault(active);return NULL;}
    void *p=b->data+b->len;b->len+=n;return p;
}
int bt_hci_cmd_send_sync(uint16_t opcode,struct net_buf *b,struct net_buf **response)
{
    struct bt_security *s=active;
    if(!live(s)||b!=&s->allocation||!s->allocated||response||s->count>=BT_SECURITY_COMMANDS)return fault(s);
    /* This port advertises NoInputNoOutput. Every positive confirmation is
     * Just Works, including the upstream initiator auto-confirm path. */
    if(opcode==BT_HCI_OP_USER_CONFIRM_REPLY&&!s->policy.allow_just_works)
        opcode=BT_HCI_OP_USER_CONFIRM_NEG_REPLY;
    struct bt_security_command *q=&s->commands[(s->first+s->count)%BT_SECURITY_COMMANDS];
    *q=(struct bt_security_command){.opcode=opcode,.length=(unsigned)b->len};memcpy(q->bytes,b->data,b->len);
    ++s->count;s->allocated=0;return 0;
}
static void pin_entry(struct bt_conn *c,bool secure)
{
    if((secure&&strlen((const char *)active->policy.pin)!=16)||!active->policy.pin[0]){
        if(bt_ssp_auth_cancel(c)) (void)fault(active);
    } else if(bt_conn_auth_pincode_entry(c,(const char *)active->policy.pin))(void)fault(active);
}
static void pairing_confirm(struct bt_conn *c)
{if((active->policy.allow_just_works?bt_ssp_auth_pairing_confirm(c):bt_ssp_auth_cancel(c))!=0)(void)fault(active);}
static const struct bt_conn_auth_cb callbacks={.pincode_entry=pin_entry,.pairing_confirm=pairing_confirm};
const struct bt_conn_auth_cb *bt_auth=&callbacks;
static int prepare(void *ctx)
{struct bt_security *s=ctx;return live(s)?1:fault(s);}
static int release(struct bt_security *s,const struct bt_h4_record *r)
{return bt_hci_release(s->acl->hci,r->serial)==1?1:fault(s);}
static unsigned le16(const uint8_t *p){return (unsigned)p[0]|(unsigned)p[1]<<8;}
static int queue(struct bt_security *s,unsigned op,const uint8_t *p,unsigned n)
{
    struct net_buf *b=bt_hci_cmd_alloc(K_FOREVER);if(!b)return -1;
    void *out=net_buf_add(b,n);if(!out&&n)return -1;if(n)memcpy(out,p,n);
    return bt_hci_cmd_send_sync((uint16_t)op,b,NULL)==0?1:fault(s);
}
static int event(void *ctx,const struct bt_h4_record *r)
{
    struct bt_security *s=ctx;if(r->type!=4)return 0;
    const uint8_t *p=r->storage.bytes;unsigned e=p[0],n=p[1];
    if(e==0x0e||e==0x0f){
        if(!s->pending)return 0;
        struct bt_security_command *q=&s->commands[s->first];unsigned op=le16(p+(e==0x0f?4:3));
        if(op!=q->opcode||r->tag!=s->tag)return 0;
        if(!live(s)||s->acl->hci->command!=BT_HCI_IDLE||p[e==0x0f?2:5])return fault(s);
        if((q->opcode==0x0411||q->opcode==0x0413)?(e!=0x0f||n!=4):(e!=0x0e||n<4))return fault(s);
        if(q->opcode==0x0c55){if(n!=5||p[6]!=1)return fault(s);}
        else if(q->opcode==0x0c79){if(n!=5||p[6]!=1)return fault(s);}
        else if(q->opcode==0x1004){
            if(n!=14||p[6]!=q->bytes[0])return fault(s);s->extended_pages=p[7];
            if(q->bytes[0]==2)s->secure_supported=!!(p[9]&1);
        } else if(q->opcode==0x1408){
            if(n!=7)return fault(s);struct bt_conn *c=bt_conn_lookup_handle((uint16_t)le16(p+6),BT_CONN_TYPE_BR);
            if(!c||le16(q->bytes)!=c->handle)return fault(s);c->key_size=p[8];c->key_verified=1;
            bool secure=bt_br_update_sec_level(c);bt_conn_unref(c);if(!secure)return fault(s);
        } else if(e==0x0e){
            unsigned expected=(q->opcode>=0x040b&&q->opcode<=0x040e)||(q->opcode>=0x042b&&q->opcode<=0x042f)||q->opcode==0x0434?10:4;
            if(n!=expected)return fault(s);
            if(expected==10&&memcmp(p+6,q->bytes,6))return fault(s);
        }
        if(release(s,r)!=1||bt_hci_security_acknowledge(s->acl->hci,s,s->tag)!=1)return fault(s);
        s->pending=0;s->first=(s->first+1)%BT_SECURITY_COMMANDS;--s->count;
        if(!s->configured)++s->configuration_stage;
        return 1;
    }
    if(e==0x05){
        if(n!=4)return fault(s);struct bt_conn *c=bt_conn_lookup_handle((uint16_t)le16(p+3),BT_CONN_TYPE_BR);
        if(c){c->authenticated=0;c->key_verified=0;c->encrypt=0;bt_conn_unref(c);}
        return 0; /* Existing ACL/SCO disconnect owns credit/terminal commit. */
    }
    if(e==0x08||e==0x30){
        if(n!=(e==0x08?4u:3u)||p[2])return fault(s);
        struct bt_conn *c=bt_conn_lookup_handle((uint16_t)le16(p+3),BT_CONN_TYPE_BR);if(!c)return fault(s);
        if(e==0x08)c->encrypt=p[5];c->key_verified=0;
        if(!c->authenticated||!c->encrypt||c->encrypt>2||!c->br.link_key||
            (!!(c->br.link_key->flags&BT_LINK_KEY_SC)!=(c->encrypt==2))){bt_conn_unref(c);return fault(s);}
        uint8_t handle[2]={(uint8_t)c->handle,(uint8_t)(c->handle>>8)};
        int result=queue(s,0x1408,handle,2);bt_conn_unref(c);if(result!=1)return -1;return release(s,r);
    }
    void (*consume)(struct net_buf *)=NULL;unsigned expected=0;
    switch(e){
    case 0x06:consume=bt_hci_auth_complete;expected=3;break;
    case 0x16:consume=bt_hci_pin_code_req;expected=6;break;
    case 0x17:consume=bt_hci_link_key_req;expected=6;break;
    case 0x18:consume=bt_hci_link_key_notify;expected=23;break;
    case 0x31:consume=bt_hci_io_capa_req;expected=6;break;
    case 0x32:consume=bt_hci_io_capa_resp;expected=9;break;
    case 0x33:consume=bt_hci_user_confirm_req;expected=10;break;
    case 0x34:consume=bt_hci_user_passkey_req;expected=6;break;
    case 0x36:consume=bt_hci_ssp_complete;expected=7;break;
    case 0x3b:consume=bt_hci_user_passkey_notify;expected=10;break;
    default:return 0;
    }
    if(!live(s)||!s->configured||n!=expected)return fault(s);
    struct bt_conn *c=e==0x06?bt_conn_lookup_handle((uint16_t)le16(p+3),BT_CONN_TYPE_BR):
        bt_conn_lookup_addr_br((const bt_addr_t *)(const void *)(p+2+(e==0x36)));
    if(!c)return fault(s);
    /* No repair/rekey lifecycle is admitted in this controller lifetime.
     * Existing key or derived proof cannot survive a replacement handshake. */
    if((e!=0x06&&c->authenticated)||(e==0x18&&c->br.link_key)){
        bt_conn_unref(c);return fault(s);
    }
    if(e==0x06){if(p[2]||c->authenticated){bt_conn_unref(c);return fault(s);}c->authenticated=1;}
    if(e==0x18&&p[24]!=0&&p[24]!=4&&p[24]!=5&&p[24]!=7&&p[24]!=8){bt_conn_unref(c);return fault(s);}
    bt_conn_unref(c);
    struct net_buf input={.data=(uint8_t *)(uintptr_t)(p+2),.len=n};consume(&input);
    if(s->fatal)return -1;return release(s,r);
}
static const struct bt_acl_security_ops operations={prepare,event};
int bt_security_init(struct bt_security *s,struct bt_acl *a,TaskHandle_t task,
                    atomic_uint *recovery,const struct bt_security_policy *policy,uint64_t timeout)
{
    if(!s||active||!a||!a->hci||a->state!=BT_ACL_ACTIVE||!task||task!=xTaskGetCurrentTaskHandle()||
        !recovery||!policy||!timeout||timeout>60000000||policy->deadline<=a->hci->transport.now||
        policy->minimum_level<2||policy->minimum_level>3||policy->allow_just_works>1||
        policy->pin[16]||!(a->hci->identity.features[6]&8)||
        (atomic_load_explicit(recovery,memory_order_acquire)&4)||
        !bt_range_disjoint_empty(s,sizeof(*s),a,sizeof(*a))||!bt_range_disjoint_empty(s,sizeof(*s),a->hci,sizeof(*a->hci))||
        !bt_range_disjoint_empty(s,sizeof(*s),policy,sizeof(*policy))||!bt_range_disjoint_empty(s,sizeof(*s),recovery,sizeof(*recovery)))return -1;
    unsigned any=0;for(unsigned i=0;i<6;++i)any|=policy->peer[i];if(!any)return -1;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(a->links[i].state!=BT_ACL_UNUSED)return -1;
    for(unsigned i=0;i<a->hci->transport.count;++i)if(!bt_range_disjoint_empty(s,sizeof(*s),a->hci->transport.records[i].storage.bytes,a->hci->transport.records[i].storage.capacity))return -1;
    *s=(struct bt_security){.self=s,.acl=a,.owner=task,.recovery=recovery,.policy=*policy,.deadline=policy->deadline,
        .timeout_us=timeout,.firmware_generation=a->hci->transport.binding.firmware_generation,
        .uart_generation=a->hci->transport.binding.uart_generation,.now=a->hci->transport.now};
    if(bt_hci_security_register(a->hci,s)!=1)return -1;
    active=s;
    if(bt_acl_security_register(a,&operations,s)!=1)return fault(s);
    return 1;
}
int bt_security_attach(struct bt_security *s,uint64_t generation)
{
    if(!live(s)||!s->configured||connection(s,generation))return -1;
    struct bt_acl_link *l=NULL;
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(s->acl->links[i].generation==generation)l=&s->acl->links[i];
    if(!l||memcmp(l->peer.address,s->policy.peer,6)||l->state==BT_ACL_UNUSED||l->state==BT_ACL_CLOSED)return -1;
    /* A later generation of the peer follows an attempt that closed before any
     * handle existed. That entry stays attached but unreachable; it must hold
     * no key, borrow or security result, and no copied command may remain. */
    for(unsigned i=0;i<BT_ACL_LINKS;++i){const struct bt_conn *c=&s->connections[i];
        if(c->attached&&(actual(s,c)||c->authenticated||c->encrypt||c->br.link_key||c->references||s->pending||s->count))return -1;}
    for(unsigned i=0;i<BT_ACL_LINKS;++i)if(!s->connections[i].attached){
        struct bt_conn *c=&s->connections[i];*c=(struct bt_conn){.acl_generation=generation,
            .firmware_generation=s->firmware_generation,.uart_generation=s->uart_generation,.attached=1,
            .required_sec_level=s->policy.minimum_level,.sec_level=BT_SECURITY_L1};
        memcpy(c->br.dst.val,l->peer.address,6);atomic_set_bit(c->flags,BT_CONN_BR_BONDABLE);return 1;}
    return 0;
}
int bt_security_start(struct bt_security *s,uint64_t generation)
{
    if(!live(s)||!s->configured||s->entered)return -1;
    struct bt_conn *c=connection(s,generation);struct bt_acl_link *l=c?actual(s,c):NULL;
    if(!l||l->state!=BT_ACL_CONNECTED||!l->handle_valid)return -1;
    c->handle=l->handle;s->entered=1;int result=bt_ssp_start_security(c);s->entered=0;
    return result==0?1:result==-EBUSY?0:fault(s);
}
int bt_security_service(struct bt_security *s,uint64_t deadline)
{
    if(!live(s)||s->entered||!deadline||deadline<=s->now)return fault(s);
    if(deadline>s->deadline)deadline=s->deadline;
    if(s->pending)return s->now<s->command_deadline?1:fault(s);
    if(!s->configured&&!s->count){
        unsigned op;uint8_t byte=1;unsigned n=1;
        switch(s->configuration_stage){
        case 0:op=0x0c56;break;
        case 1:op=0x0c55;n=0;break;
        case 2:op=0x1004;byte=0;break;
        case 3:if(s->extended_pages<2){s->configured=1;return 1;}op=0x1004;byte=2;break;
        case 4:if(!s->secure_supported){s->configured=1;return 1;}op=0x0c7a;break;
        case 5:op=0x0c79;n=0;break;
        default:s->configured=1;return 1;
        }
        if(queue(s,op,&byte,n)!=1)return -1;
    }
    if(!s->count)return 1;
    struct bt_hci *h=s->acl->hci;
    if(h->command!=BT_HCI_IDLE||s->acl->command_link<BT_ACL_LINKS||
        (s->acl->synchronous&&s->acl->synchronous->acknowledgement_pending(s->acl->synchronous_context)))return 1;
    uint64_t bound=s->now>UINT64_MAX-s->timeout_us?UINT64_MAX:s->now+s->timeout_us;if(bound>deadline)bound=deadline;
    struct bt_security_command *q=&s->commands[s->first];
    int result=bt_hci_security_request(h,s,q->opcode,q->bytes,q->length,bound);
    if(result<0)return fault(s);
    if(result==1){s->pending=1;s->tag=h->command_tag;s->command_deadline=bound;}
    return 1;
}
int bt_security_step(struct bt_security *s,unsigned budget,uint64_t deadline)
{
    if(!live(s)||s->entered)return fault(s);
    if(bt_acl_step(s->acl,budget,deadline)!=1)return fault(s);
    return bt_security_service(s,deadline);
}
int bt_security_authorized(struct bt_security *s,uint64_t link,uint64_t *generation,unsigned *sc)
{
    if(!live(s)||!generation||!sc||!bt_range_disjoint_empty(s,sizeof(*s),generation,sizeof(*generation))||
        !bt_range_disjoint_empty(s,sizeof(*s),sc,sizeof(*sc))||!bt_range_disjoint_empty(generation,sizeof(*generation),sc,sizeof(*sc)))return -1;
    struct bt_conn *c=connection(s,link);struct bt_acl_link *l=c?actual(s,c):NULL;
    if(!l||l->state!=BT_ACL_CONNECTED||!c->authenticated||!c->encrypt||!c->key_verified||
        !c->br.link_key||c->sec_level<c->required_sec_level)return 0;
    *generation=c->acl_generation;*sc=c->encrypt==2;return 1;
}
