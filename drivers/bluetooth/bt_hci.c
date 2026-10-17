/* SPDX-License-Identifier: Apache-2.0 */
/* Original implementation of Bluetooth Core H4/HCI wire formats.
 * Protocol values and field lengths are checked against separately retained
 * Linux reference structures; no Linux implementation code is included. */
#include "bt_hci.h"
#define HCI_OP_RESET 0x0c03u
#define HCI_OP_READ_LOCAL_VERSION 0x1001u
#define HCI_OP_READ_LOCAL_COMMANDS 0x1002u
#define HCI_OP_READ_LOCAL_FEATURES 0x1003u
#define HCI_OP_READ_BUFFER_SIZE 0x1005u
#define HCI_OP_READ_BD_ADDR 0x1009u
#define HCI_EV_DISCONN_COMPLETE 0x05u
#define HCI_EV_CMD_COMPLETE 0x0eu
#define HCI_EV_CMD_STATUS 0x0fu
#define HCI_EV_HARDWARE_ERROR 0x10u
static const uint16_t opcodes[]={HCI_OP_RESET,HCI_OP_READ_LOCAL_VERSION,
    HCI_OP_READ_LOCAL_COMMANDS,HCI_OP_READ_LOCAL_FEATURES,HCI_OP_READ_BUFFER_SIZE,HCI_OP_READ_BD_ADDR};
static const unsigned returns[]={1,9,65,9,8,7};
static const uint16_t config_opcodes[]={0x0c01,0x0c05,0x0c16,0x0c15,0x0c1c,0x0c1b,0x0c1a,0x0c19};
static const unsigned config_returns[]={1,1,1,3,1,5,1,2};
static const uint16_t audio_opcodes[]={0x0c01,0x0c05,0x0c16,0x0c15,0x0c1c,0x0c1b,0x0c2f,0x0c2e,0x0c5b,0x0c5a,0x0c1a,0x0c19};
static const unsigned audio_returns[]={1,1,1,3,1,5,1,2,1,2,1,2};
static unsigned config_count(const struct bt_hci *h){return h->incoming_config.synchronous_audio ? 12 : 8;}
static uint16_t config_opcode(const struct bt_hci *h,unsigned stage)
{return h->incoming_config.synchronous_audio ? audio_opcodes[stage] : config_opcodes[stage];}
static unsigned config_return(const struct bt_hci *h,unsigned stage)
{return h->incoming_config.synchronous_audio ? audio_returns[stage] : config_returns[stage];}
/* Core5.4 §7.3.1: all defined Classic events; reserved and LE bits stay0.
 * Command Complete/Status and Number Of Completed Packets are unmaskable. */
static const uint8_t config_mask[]={0xff,0x9f,0xfb,0xbf,0x07,0xf8,0xbf,0x1d};
static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0]|((uint16_t)p[1]<<8); }
static int overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    return an && bn && ((uintptr_t)a <= (uintptr_t)b ?
        (uintptr_t)b-(uintptr_t)a < an : (uintptr_t)a-(uintptr_t)b < bn);
}
static int fail(struct bt_hci *h)
{ bt_h4_fail(&h->transport);h->state=BT_HCI_FAULT;return -1; }
int bt_hci_init(struct bt_hci *h, const struct bt_h4_binding *b,
                const struct bt_h4_storage *s, unsigned count,
                uint64_t deadline, uint64_t timeout)
{
    if(!h || !b || !s || count<2 || count>BT_H4_SLOTS || !timeout || !deadline ||
       h->state!=BT_HCI_CLOSED || !b->uart ||
       overlaps(h,sizeof(*h),b,sizeof(*b)) || overlaps(h,sizeof(*h),s,count*sizeof(*s)) ||
       overlaps(h,sizeof(*h),b->uart,sizeof(*b->uart))) return -1;
    for(unsigned i=0;i<count;++i) if(overlaps(h,sizeof(*h),s[i].bytes,s[i].capacity)) return -1;
    /* H4 validates all storage before mutation; its initial callback may fail
     * after admission. Such failure retains the outer lifetime too. */
    int result=bt_h4_init(&h->transport,b,s,count,deadline);
    if(result!=1) return h->transport.state==BT_H4_FAULT ? fail(h) : -1;
    h->self=h;h->acl_owner=0;h->synchronous_owner=0;h->incoming_config=(struct bt_hci_incoming_config){0};
    h->configuration_deadline=0;h->configuration_stage=0;h->configured=0;h->synchronous_ready=0;
    h->identity=(struct bt_hci_identity){0};h->initialization_deadline=deadline;
    h->command_timeout_us=timeout;h->command_deadline=0;h->command_tag=0;
    h->stage=0;h->external_command=0;h->credits=1;h->opcode=0;h->last_status=0;h->command_accepted=0;
    h->command=BT_HCI_IDLE;h->state=BT_HCI_INITIALIZING;return 1;
}
int bt_hci_synchronous_supported(const struct bt_hci *h)
{
    if(!h || h->self!=h || h->identity.valid!=63)return 0;
    const uint8_t *c=h->identity.supported_commands,*f=h->identity.features,*b=h->identity.buffer_size;
    return h->identity.version[0]>=7 && h->identity.version[0]<=13 && (c[29]&0x18)==0x18 &&
        (c[2]&0x20) && (c[10]&0x18)==0x18 && (c[18]&0x0c)==0x0c &&
        (c[16]&0x20) && (f[1]&8) && (f[2]&1) && b[2]>=2 && le16(b+5);
}
int bt_hci_configuration_complete(const struct bt_hci *h)
{return h && h->self==h && h->configured && h->configuration_stage==config_count(h) &&
    h->command_tag==6+config_count(h) && h->synchronous_ready==h->incoming_config.synchronous_audio;}
static int config_authority(struct bt_hci *h)
{
    const struct bt_hci_incoming_config *c=&h->incoming_config;
    return c->current(c->context,&h->transport.binding,c->service_generation,
                      c->receive_latency_us)==1 ? 1 : fail(h);
}
int bt_hci_configure_incoming(struct bt_hci *h,const struct bt_hci_incoming_config *c,
                              uint64_t deadline)
{
    if(!h || h->self!=h || !c || !c->current || !c->service_generation || !c->receive_latency_us || c->synchronous_audio>1 ||
       (c->synchronous_audio && !bt_hci_synchronous_supported(h)) ||
       h->state!=BT_HCI_IDENTIFIED || h->identity.valid!=63 || h->command_tag!=6 ||
       h->acl_owner || h->configured || h->configuration_stage || h->configuration_deadline ||
       h->command!=BT_HCI_IDLE || h->external_command || deadline<=h->transport.now ||
       c->accept_timeout_slots<0x00a0 || c->accept_timeout_slots>0xb540 ||
       c->page_interval_slots<0x0012 || c->page_interval_slots>0x1000 || (c->page_interval_slots&1) ||
       c->page_window_slots<0x0011 || c->page_window_slots>c->page_interval_slots ||
       c->receive_latency_us>=(uint64_t)c->accept_timeout_slots*625-1 ||
       overlaps(c,sizeof(*c),h,sizeof(*h)) ||
       overlaps(c,sizeof(*c),h->transport.binding.uart,sizeof(struct a733_uart)) ||
       h->transport.state!=BT_H4_ACTIVE || h->transport.staged ||
       h->transport.assembling!=h->transport.count || h->transport.tx_offset!=h->transport.tx_length ||
       (h->identity.features[4]&0x20u) || (h->identity.supported_commands[1]&3u)!=3u ||
       !(h->identity.supported_commands[5]&0x40u) || !(h->identity.supported_commands[6]&1u) ||
       (h->identity.supported_commands[7]&0xccu)!=0xccu ||
       (h->identity.supported_commands[8]&3u)!=3u) return -1;
    for(unsigned i=0;i<h->transport.count;++i)
        if(h->transport.records[i].owner!=BT_H4_FREE ||
           overlaps(c,sizeof(*c),h->transport.records[i].storage.bytes,
                    h->transport.records[i].storage.capacity)) return -1;
    struct bt_hci_incoming_config copied=*c;
    if(copied.current(copied.context,&h->transport.binding,copied.service_generation,copied.receive_latency_us)!=1) return -1;
    h->incoming_config=copied;h->configuration_deadline=deadline;
    h->state=BT_HCI_CONFIGURING;return 1;
}
int bt_hci_incoming_bound(struct bt_hci *h,uint64_t response)
{
    if(!h || h->self!=h || h->state!=BT_HCI_IDENTIFIED || !h->configured ||
       h->configuration_stage!=config_count(h) || !response) return 0;
    uint64_t timeout=(uint64_t)h->incoming_config.accept_timeout_slots*625;
    if(h->incoming_config.receive_latency_us>=timeout ||
       response>=timeout-h->incoming_config.receive_latency_us) return 0;
    return config_authority(h);
}
static int request(struct bt_hci *h,uint16_t opcode,const uint8_t *p,size_t n,uint64_t deadline)
{
    if(!h || h->self!=h || !deadline || !p || overlaps(h,sizeof(*h),p,n))return -1;
    if(h->state!=BT_HCI_IDENTIFIED) return -1;
    if(h->command!=BT_HCI_IDLE || !h->credits ||
       h->transport.tx_offset<h->transport.tx_length) return 0;
    if(h->command_tag==UINT64_MAX) return fail(h);
    int accepted=bt_h4_command(&h->transport,opcode,p,n,deadline);
    if(accepted!=1) return accepted==0 ? 0 : fail(h);
    ++h->command_tag;h->opcode=opcode;h->command_deadline=deadline;
    h->command_accepted=0;h->external_command=1;h->command=BT_HCI_SENDING;
    --h->credits;return 1;
}
int bt_hci_request(struct bt_hci *h,uint16_t op,const uint8_t *p,size_t n,uint64_t deadline)
{
    if((op!=0x0405 || n!=13) && (op!=0x0406 || n!=3) && (op!=0x0409 || n!=7) &&
       (op!=0x040a || n!=7))return -1;
    return request(h,op,p,n,deadline);
}
int bt_hci_synchronous_request(struct bt_hci *h,const void *owner,uint16_t op,const uint8_t *p,size_t n,uint64_t deadline)
{
    if(!h || !owner || h->synchronous_owner!=owner || !h->synchronous_ready ||
       ((op!=0x041b || n!=2) && (op!=0x043d || n!=59) && (op!=0x043e || n!=63) &&
        (op!=0x042a || n!=7) && (op!=0x0406 || n!=3)))return -1;
    return request(h,op,p,n,deadline);
}
static int retain(struct bt_hci *h, const struct bt_h4_record *r)
{ return bt_h4_own(&h->transport,r->serial)==1 ? 1 : fail(h); }
static int bad_record(struct bt_hci *h, const struct bt_h4_record *r)
{ (void)retain(h,r);return fail(h); }
static void copy(uint8_t *to, const uint8_t *from, unsigned n)
{ for(unsigned i=0;i<n;++i) to[i]=from[i]; }
static int dispatch(struct bt_hci *h, const struct bt_h4_record *r)
{
    const uint8_t *p=r->storage.bytes;
    if(r->type!=4) return retain(h,r);
    unsigned event=p[0], n=p[1];
    if(event==HCI_EV_HARDWARE_ERROR) return bad_record(h,r);
    if(event==HCI_EV_DISCONN_COMPLETE) {
        if(n!=4 || (le16(p+3)&0xf000u)) return bad_record(h,r);
        return retain(h,r);
    }
    if(event!=HCI_EV_CMD_COMPLETE && event!=HCI_EV_CMD_STATUS) return retain(h,r);
    unsigned cs=event==HCI_EV_CMD_STATUS;
    if((cs && n!=4) || (!cs && n<3)) return bad_record(h,r);
    unsigned credits=p[cs ? 3 : 2];
    uint16_t opcode=le16(p+(cs ? 4 : 3));
    /* Opcode zero is an independent credit update, never a completion. */
    if(!opcode) {
        if((cs && p[2]) || (!cs && n!=3)) return bad_record(h,r);
        h->credits=credits;
        return bt_h4_release(&h->transport,r->serial)==1 ? 1 : fail(h);
    }
    if(h->command!=BT_HCI_WAITING || opcode!=h->opcode || r->tag!=h->command_tag)
        return retain(h,r);
    h->credits=credits;
    if(h->state==BT_HCI_CONFIGURING) {
        unsigned stage=h->configuration_stage;
        if(stage>=config_count(h) || cs || n!=3+config_return(h,stage)) return bad_record(h,r);
        h->last_status=p[5];
        if(p[5]) return bad_record(h,r);
        const struct bt_hci_incoming_config *c=&h->incoming_config;
        unsigned op=config_opcode(h,stage);
        if((op==0x0c15 && le16(p+6)!=c->accept_timeout_slots) ||
           (op==0x0c1b && (le16(p+6)!=c->page_interval_slots || le16(p+8)!=c->page_window_slots)) ||
           (op==0x0c19 && p[6]!=2) || ((op==0x0c2e || op==0x0c5a) && p[6]!=1)) return bad_record(h,r);
        if(bt_h4_release(&h->transport,r->serial)!=1) return fail(h);
        ++h->configuration_stage;h->command=BT_HCI_IDLE;
        if(h->configuration_stage==config_count(h)) {h->configured=1;h->synchronous_ready=c->synchronous_audio;h->state=BT_HCI_IDENTIFIED;}
        return 1;
    }
    if(h->external_command) {
        /* Link commands require Status then their separate terminal event.
         * Unknown-command refusal may instead use Complete with status. */
        if(!cs && (n!=4 || !p[5])) return bad_record(h,r);
        h->last_status=p[cs ? 2 : 5];h->command_accepted=!h->last_status;
        h->external_command=0;h->command=BT_HCI_IDLE;
        return retain(h,r);
    }
    if(cs) {
        h->last_status=p[2];
        if(p[2]) return bad_record(h,r);
        h->command_accepted=1;
        return bt_h4_release(&h->transport,r->serial)==1 ? 1 : fail(h);
    }
    if(n<4) return bad_record(h,r);
    h->last_status=p[5];
    if(p[5] || n!=3+returns[h->stage]) return bad_record(h,r);
    switch(h->stage) {
    case 1:copy(h->identity.version,p+6,8);break;
    case 2:copy(h->identity.supported_commands,p+6,64);break;
    case 3:copy(h->identity.features,p+6,8);break;
    case 4:copy(h->identity.buffer_size,p+6,7);break;
    case 5:copy(h->identity.address,p+6,6);break;
    default:break;
    }
    h->identity.valid|=1u<<h->stage;
    if(bt_h4_release(&h->transport,r->serial)!=1) return fail(h);
    ++h->stage;h->command=BT_HCI_IDLE;
    if(h->stage==6) h->state=BT_HCI_IDENTIFIED;
    return 1;
}
int bt_hci_step(struct bt_hci *h, unsigned budget, uint64_t deadline)
{
    if(!h || h->self!=h || !deadline || !budget || budget>BT_H4_BUDGET || h->state==BT_HCI_CLOSED) return -1;
    if(h->state==BT_HCI_FAULT) return -1;
    if((h->state==BT_HCI_CONFIGURING || h->configured) && config_authority(h)!=1) return -1;
    if(h->state==BT_HCI_CONFIGURING && deadline>h->configuration_deadline)
        deadline=h->configuration_deadline;
    if(h->state==BT_HCI_INITIALIZING && deadline>h->initialization_deadline)
        deadline=h->initialization_deadline;
    if(h->command!=BT_HCI_IDLE && deadline>h->command_deadline) deadline=h->command_deadline;
    uint64_t tag=h->command==BT_HCI_WAITING ? h->command_tag : 0;
    if(bt_h4_step(&h->transport,budget,tag,deadline)!=1) return fail(h);
    if(h->command==BT_HCI_SENDING && h->transport.tx_offset==h->transport.tx_length)
        h->command=BT_HCI_WAITING;
    const struct bt_h4_record *r=bt_h4_next(&h->transport,BT_H4_COMPLETE);
    if(r && dispatch(h,r)!=1) return -1;
    if(h->state==BT_HCI_INITIALIZING && h->command==BT_HCI_IDLE && h->credits &&
       !h->transport.staged && h->transport.assembling==h->transport.count &&
       !bt_h4_next(&h->transport,BT_H4_COMPLETE)) {
        uint64_t now=h->transport.now;
        if(now>UINT64_MAX-h->command_timeout_us) return fail(h);
        h->command_deadline=now+h->command_timeout_us;
        if(h->command_deadline>h->initialization_deadline) h->command_deadline=h->initialization_deadline;
        h->opcode=opcodes[h->stage];h->command_tag=h->stage+1;h->command_accepted=0;
        if(bt_h4_command(&h->transport,h->opcode,0,0,h->command_deadline)!=1) return fail(h);
        --h->credits;h->command=BT_HCI_SENDING;
    }
    if(h->state==BT_HCI_CONFIGURING && h->command==BT_HCI_IDLE && h->credits &&
       !h->transport.staged && h->transport.assembling==h->transport.count &&
       !bt_h4_next(&h->transport,BT_H4_COMPLETE)) {
        const struct bt_hci_incoming_config *c=&h->incoming_config;
        unsigned stage=h->configuration_stage;uint8_t params[8]={0};size_t n=0;
        if(stage>=config_count(h))return fail(h);
        unsigned op=config_opcode(h,stage);
        if(op==0x0c01) {copy(params,config_mask,8);n=8;}
        else if(op==0x0c05) n=1;
        else if(op==0x0c16) {params[0]=(uint8_t)c->accept_timeout_slots;params[1]=(uint8_t)(c->accept_timeout_slots>>8);n=2;}
        else if(op==0x0c1c) {
            params[0]=(uint8_t)c->page_interval_slots;params[1]=(uint8_t)(c->page_interval_slots>>8);
            params[2]=(uint8_t)c->page_window_slots;params[3]=(uint8_t)(c->page_window_slots>>8);n=4;
        } else if(op==0x0c1a) {params[0]=2;n=1;}
        else if(op==0x0c2f || op==0x0c5b) {params[0]=1;n=1;}
        if(h->transport.now>UINT64_MAX-h->command_timeout_us) return fail(h);
        h->command_deadline=h->transport.now+h->command_timeout_us;
        if(h->command_deadline>h->configuration_deadline) h->command_deadline=h->configuration_deadline;
        h->opcode=(uint16_t)op;++h->command_tag;h->command_accepted=0;
        if(bt_h4_command(&h->transport,h->opcode,params,n,h->command_deadline)!=1) return fail(h);
        --h->credits;h->command=BT_HCI_SENDING;
    }
    return 1;
}
const struct bt_h4_record *bt_hci_next(const struct bt_hci *h)
{ return h && h->self==h ? bt_h4_next(&h->transport,BT_H4_OWNED) : 0; }
int bt_hci_release(struct bt_hci *h, uint64_t serial)
{
    if(!h || h->self!=h || h->state==BT_HCI_CLOSED || h->state==BT_HCI_FAULT) return -1;
    /* Only application-owned packets may be released through this interface. */
    for(unsigned i=0;i<h->transport.count;++i)
        if(h->transport.records[i].serial==serial && h->transport.records[i].owner==BT_H4_OWNED)
            return bt_h4_release(&h->transport,serial);
    return -1;
}
