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
    h->identity=(struct bt_hci_identity){0};h->initialization_deadline=deadline;
    h->command_timeout_us=timeout;h->command_deadline=0;h->command_tag=0;
    h->stage=0;h->external_command=0;h->credits=1;h->opcode=0;h->last_status=0;h->command_accepted=0;
    h->command=BT_HCI_IDLE;h->state=BT_HCI_INITIALIZING;return 1;
}
int bt_hci_request(struct bt_hci *h, uint16_t opcode, const uint8_t *p, size_t n,
                   uint64_t deadline)
{
    if(!h || !deadline || !p || ((opcode!=0x0405 || n!=13) &&
       (opcode!=0x0406 || n!=3)) || overlaps(h,sizeof(*h),p,n)) return -1;
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
    if(!h || !deadline || !budget || budget>BT_H4_BUDGET || h->state==BT_HCI_CLOSED) return -1;
    if(h->state==BT_HCI_FAULT) return -1;
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
    return 1;
}
const struct bt_h4_record *bt_hci_next(const struct bt_hci *h)
{ return h ? bt_h4_next(&h->transport,BT_H4_OWNED) : 0; }
int bt_hci_release(struct bt_hci *h, uint64_t serial)
{
    if(!h || h->state==BT_HCI_CLOSED || h->state==BT_HCI_FAULT) return -1;
    /* Only application-owned packets may be released through this interface. */
    for(unsigned i=0;i<h->transport.count;++i)
        if(h->transport.records[i].serial==serial && h->transport.records[i].owner==BT_H4_OWNED)
            return bt_h4_release(&h->transport,serial);
    return -1;
}
