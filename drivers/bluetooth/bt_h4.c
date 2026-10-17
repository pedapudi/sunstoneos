/* SPDX-License-Identifier: Apache-2.0 */
/* Original implementation of Bluetooth Core H4/HCI wire formats.
 * Protocol values and field lengths are checked against separately retained
 * Linux reference structures; no Linux implementation code is included. */
#include "bt_h4.h"
#define H4_ACL 2u
#define H4_SCO 3u
#define H4_EVENT 4u
#define H4_ISO 5u
static int overlaps(const void *a, size_t an, const void *b, size_t bn)
{
    return an && bn && ((uintptr_t)a <= (uintptr_t)b ?
        (uintptr_t)b-(uintptr_t)a < an : (uintptr_t)a-(uintptr_t)b < bn);
}
static int valid_span(const void *p, size_t n)
{ return !n || (p && (uintptr_t)p <= UINTPTR_MAX-(n-1)); }
void bt_h4_fail(struct bt_h4 *q) { if (q) q->state=BT_H4_FAULT; }
static int valid(struct bt_h4 *q, uint64_t deadline)
{
    uint64_t now;
    struct a733_uart *u=q->binding.uart;
    if (q->state!=BT_H4_ACTIVE) return 0;
    if (u->state!=A733_UART_READY || u->errors ||
        u->plan.base!=q->uart_plan.base || u->plan.lcr!=q->uart_plan.lcr ||
        u->plan.mcr!=q->uart_plan.mcr || u->plan.divisor!=q->uart_plan.divisor ||
        u->plan.actual_baud!=q->uart_plan.actual_baud ||
        q->binding.current(q->binding.context,q->binding.firmware_generation,
                           q->binding.uart_generation)!=1 ||
        u->ops.now_us(u->context,&now)!=1 || now<q->now || now<u->last_now ||
        now>=deadline || (q->partial_deadline && now>=q->partial_deadline) ||
        (q->tx_offset<q->tx_length && now>=q->tx_deadline)) {
        bt_h4_fail(q); return 0;
    }
    q->now=now;return 1;
}
int bt_h4_init(struct bt_h4 *q, const struct bt_h4_binding *b,
               const struct bt_h4_storage *s, unsigned count, uint64_t deadline)
{
    if (!q || !b || !s || !count || count>BT_H4_SLOTS || !deadline ||
        q->state!=BT_H4_CLOSED || !b->uart || !b->current || !b->baud ||
        !b->firmware_generation || !b->uart_generation || !b->frame_timeout_us ||
        overlaps(q,sizeof(*q),b,sizeof(*b)) || overlaps(q,sizeof(*q),s,count*sizeof(*s)) ||
        overlaps(q,sizeof(*q),b->uart,sizeof(*b->uart)) ||
        b->uart->state!=A733_UART_READY || !b->uart->ops.now_us ||
        b->uart->plan.base!=0x02501000u || b->uart->plan.lcr!=3 ||
        b->uart->plan.mcr!=0x22 || b->uart->plan.actual_baud!=b->baud) return -1;
    for(unsigned i=0;i<count;++i) {
        if(s[i].capacity<257 || s[i].capacity>BT_H4_MAX_PACKET ||
           !valid_span(s[i].bytes,s[i].capacity) ||
           overlaps(s[i].bytes,s[i].capacity,q,sizeof(*q)) ||
           overlaps(s[i].bytes,s[i].capacity,b->uart,sizeof(*b->uart)) ||
           overlaps(s[i].bytes,s[i].capacity,b,sizeof(*b)) ||
           overlaps(s[i].bytes,s[i].capacity,s,count*sizeof(*s))) return -1;
        for(unsigned j=0;j<i;++j)
            if(overlaps(s[i].bytes,s[i].capacity,s[j].bytes,s[j].capacity)) return -1;
    }
    *q=(struct bt_h4){0};q->binding=*b;q->uart_plan=b->uart->plan;
    q->count=count;q->assembling=count;q->next_serial=1;q->state=BT_H4_ACTIVE;
    for(unsigned i=0;i<count;++i) q->records[i].storage=s[i];
    return valid(q,deadline) ? 1 : -1;
}
int bt_h4_command(struct bt_h4 *q, uint16_t opcode, const uint8_t *p, size_t n,
                  uint64_t deadline)
{
    if(!q || !opcode || n>255 || !valid_span(p,n) || !deadline ||
       overlaps(p,n,q,sizeof(*q))) return -1;
    if(q->state!=BT_H4_ACTIVE) return -1;
    if(q->tx_offset<q->tx_length) return 0;
    if(!valid(q,deadline)) return -1;
    q->tx[0]=1;q->tx[1]=(uint8_t)opcode;q->tx[2]=(uint8_t)(opcode>>8);q->tx[3]=(uint8_t)n;
    for(size_t i=0;i<n;++i) q->tx[4+i]=p[i];
    q->tx_payload=0;q->tx_header=n+4;
    q->tx_length=n+4;q->tx_offset=0;q->tx_deadline=deadline;return 1;
}
int bt_h4_acl(struct bt_h4 *q, uint16_t handle_flags, const uint8_t *p, size_t n,
              uint64_t deadline)
{
    unsigned pb=(handle_flags>>12)&3u;
    if(!q || !n || n>65535 || !valid_span(p,n) || !deadline ||
       (handle_flags&0xc000u) || (handle_flags&0xfffu)>0xeffu || (pb!=1 && pb!=2) ||
       overlaps(p,n,q,sizeof(*q))) return -1;
    if(q->state!=BT_H4_ACTIVE) return -1;
    if(overlaps(p,n,q->binding.uart,sizeof(*q->binding.uart))) return -1;
    for(unsigned i=0;i<q->count;++i)
        if(overlaps(p,n,q->records[i].storage.bytes,q->records[i].storage.capacity)) return -1;
    if(q->tx_offset<q->tx_length) return 0;
    if(!valid(q,deadline)) return -1;
    q->tx[0]=2;q->tx[1]=(uint8_t)handle_flags;q->tx[2]=(uint8_t)(handle_flags>>8);
    q->tx[3]=(uint8_t)n;q->tx[4]=(uint8_t)(n>>8);
    q->tx_payload=p;q->tx_header=5;q->tx_length=5+n;q->tx_offset=0;q->tx_deadline=deadline;
    return 1;
}
int bt_h4_sco(struct bt_h4 *q,uint16_t handle,const uint8_t *p,size_t n,uint64_t deadline)
{
    if(!q || !n || n>255 || handle>0xeff || !valid_span(p,n) || !deadline ||
       overlaps(p,n,q,sizeof(*q)))return -1;
    if(q->state!=BT_H4_ACTIVE)return -1;
    if(overlaps(p,n,q->binding.uart,sizeof(*q->binding.uart)))return -1;
    for(unsigned i=0;i<q->count;++i)if(overlaps(p,n,q->records[i].storage.bytes,q->records[i].storage.capacity))return -1;
    if(q->tx_offset<q->tx_length)return 0;
    if(!valid(q,deadline))return -1;
    q->tx[0]=3;q->tx[1]=(uint8_t)handle;q->tx[2]=(uint8_t)(handle>>8);q->tx[3]=(uint8_t)n;
    q->tx_payload=p;q->tx_header=4;q->tx_length=4+n;q->tx_offset=0;q->tx_deadline=deadline;return 1;
}
static unsigned free_slot(const struct bt_h4 *q)
{
    for(unsigned i=0;i<q->count;++i) if(q->records[i].owner==BT_H4_FREE) return i;
    return q->count;
}
static int parse(struct bt_h4 *q, unsigned budget)
{
    for(unsigned n=0;n<budget && q->stage_offset<q->staged;++n) {
        struct a733_uart_byte byte=q->staging[q->stage_offset];
        if(byte.status) {bt_h4_fail(q);return 0;}
        if(q->assembling==q->count) {
            unsigned i=free_slot(q), h;
            if(i==q->count) break;
            h=byte.value==H4_EVENT ? 2 : byte.value==H4_SCO ? 3 :
              (byte.value==H4_ACL || byte.value==H4_ISO) ? 4 : 0;
            if(!h || q->next_serial==UINT64_MAX ||
               q->now>UINT64_MAX-q->binding.frame_timeout_us) {bt_h4_fail(q);return 0;}
            struct bt_h4_record *r=&q->records[i];
            r->owner=BT_H4_PARTIAL;r->length=0;r->expected=h;r->header=h;
            r->type=byte.value;r->serial=q->next_serial++;r->tag=q->stage_tag;
            q->assembling=i;q->partial_deadline=q->now+q->binding.frame_timeout_us;
            ++q->stage_offset;continue;
        }
        struct bt_h4_record *r=&q->records[q->assembling];
        r->storage.bytes[r->length++]=byte.value;++q->stage_offset;
        if(r->length==r->header) {
            const uint8_t *p=r->storage.bytes;
            size_t len=r->type==H4_EVENT ? p[1] : r->type==H4_SCO ? p[2] :
                       (size_t)p[2]|((size_t)p[3]<<8);
            if(r->type==H4_ISO) len &= 0x3fffu;
            if(len>r->storage.capacity-r->header) {bt_h4_fail(q);return 0;}
            r->expected=r->header+len;
        }
        if(r->length==r->expected) {
            r->owner=BT_H4_COMPLETE;q->assembling=q->count;q->partial_deadline=0;
        }
    }
    if(q->stage_offset==q->staged) q->stage_offset=q->staged=0;
    return 1;
}
int bt_h4_step(struct bt_h4 *q, unsigned budget, uint64_t tag, uint64_t deadline)
{
    struct a733_uart_result result;
    if(!q || !budget || budget>BT_H4_BUDGET || !deadline) return -1;
    if(!valid(q,deadline)) return -1;
    size_t room=!q->staged && (q->assembling<q->count || free_slot(q)<q->count) ? budget : 0;
    unsigned rx_empty=0;
    if(room) {
        int state=a733_uart_exchange(q->binding.uart,q->staging,room,
            0,0,budget,deadline,&result);
        q->staged=result.received;q->stage_offset=0;q->stage_tag=tag;
        if(state!=A733_UART_READY || result.errors) {bt_h4_fail(q);return -1;}
        /* A short receive proves that the UART core observed LSR.DR clear.
         * Exhausting the budget says nothing about the remaining FIFO tail. */
        rx_empty=result.received<room;
    }
    if(!valid(q,deadline) || !parse(q,budget) || !valid(q,deadline)) return -1;
    if(q->tx_offset<q->tx_length && rx_empty && !q->staged &&
       q->assembling==q->count && !bt_h4_next(q,BT_H4_COMPLETE)) {
        const uint8_t *bytes;
        size_t remaining;
        if(q->tx_offset<q->tx_header) {
            bytes=q->tx+q->tx_offset;remaining=q->tx_header-q->tx_offset;
        } else {
            bytes=q->tx_payload+(q->tx_offset-q->tx_header);
            remaining=q->tx_length-q->tx_offset;
        }
        int state=a733_uart_exchange(q->binding.uart,0,0,
            bytes,remaining,budget,deadline,&result);
        q->tx_offset+=result.transmitted;
        if(state!=A733_UART_READY || result.errors) {bt_h4_fail(q);return -1;}
        if(!valid(q,deadline)) return -1;
    }
    return 1;
}
const struct bt_h4_record *bt_h4_next(const struct bt_h4 *q, enum bt_h4_owner owner)
{
    if(!q || q->state==BT_H4_CLOSED || (owner!=BT_H4_COMPLETE && owner!=BT_H4_OWNED)) return 0;
    const struct bt_h4_record *r=0;
    for(unsigned i=0;i<q->count;++i) if(q->records[i].owner==owner &&
        (!r || q->records[i].serial<r->serial)) r=&q->records[i];
    return r;
}
int bt_h4_own(struct bt_h4 *q, uint64_t serial)
{
    if(!q || q->state!=BT_H4_ACTIVE || !serial) return -1;
    for(unsigned i=0;i<q->count;++i) if(q->records[i].serial==serial && q->records[i].owner==BT_H4_COMPLETE) {
        q->records[i].owner=BT_H4_OWNED;return 1;
    }
    return -1;
}
int bt_h4_release(struct bt_h4 *q, uint64_t serial)
{
    if(!q || q->state!=BT_H4_ACTIVE || !serial) return -1;
    for(unsigned i=0;i<q->count;++i) if(q->records[i].serial==serial &&
        (q->records[i].owner==BT_H4_COMPLETE || q->records[i].owner==BT_H4_OWNED)) {
        q->records[i].owner=BT_H4_FREE;return 1;
    }
    return -1;
}
