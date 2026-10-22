#include "a733_spi.h"
#include "a733_range.h"

#define GCR 0x04u
#define TCR 0x08u
#define IER 0x10u
#define ISR 0x14u
#define FCR 0x18u
#define FSR 0x1cu
#define WCR 0x20u
#define MBC 0x30u
#define MTC 0x34u
#define BCC 0x38u
#define BAT 0x40u
#define NDMA 0x88u /* Normal DMA handshake control; reset value 0xe5. */
#define TXD 0x200u
#define RXD 0x300u
#define BSR 0x400u
#define XCH 0x80000000u /* TCR: start exchange. */
#define GCR_SRST 0x80000000u /* GCR: soft reset, self-clearing. */
#define RESETS 0x80008000u
#define ERRORS 0x2700u /* RX overflow/underflow, TX overflow, invalid SS */
#define DONE 0x1000u
#define STATUS 0x7f77u
#define FIFO_COUNTS 0x00ff00ffu
#define BUFFER_COUNTS 0x070700ffu
#define FIFO_CONTROL 0x00200001u /* TX trigger 32, RX trigger 1, no DMA. */
#define TX_DRQ 0x01000000u
#define RX_DRQ 0x00000100u
#define DUAL 0x10000000u /* BCC DRM */
#define QUAD 0x20000000u /* BCC Quad_EN */

int a733_spi_plan(unsigned port, uint32_t hz, struct a733_spi_plan *p)
{
    static const uint16_t clocks[6] = {0xf00,0xf08,0xf10,0xf20,0xf28,0x150};
    unsigned best=1025, bm=0, bn=0;
    if (!p || port>5 || hz<24194 || hz>24000000) return -1;
    for (unsigned m=1;m<=32;m++) for (unsigned n=1;n<=32;n++) {
        unsigned d=m*n;
        if (24000000ull<=(uint64_t)hz*d && d<best) { best=d;bm=m;bn=n; }
    }
    if (best>1024 || 24000000u/best<24000u) return -1;
    p->base=port==5?0x07092000u:0x02540000u+port*0x1000u;
    p->clock=(port==5?0x07010000u:0x02002000u)+clocks[port];
    p->reset=p->clock+(port==5?12u:4u);
    p->clock_word=0x80000000u | (bm-1) | ((bn-1)<<8);
    p->divisor=best;p->actual_hz=24000000u/best;p->intid=port==5?242u:56u+port;
    return 0;
}
unsigned a733_spi_dma_request(unsigned port)
{
    static const uint8_t requests[6]={23,24,25,26,0,53};
    return port<6?requests[port]:0;
}
static int fault(struct a733_spi *s) { s->state=A733_SPI_FAULT;return 0; }
static int timely(struct a733_spi *s)
{
    uint64_t t;
    if (s->ops.now_us(s->context,&t)!=1 || !a733_deadline_admit(t,&s->last_now,s->deadline)) return fault(s);
    return 1;
}
static int rd(struct a733_spi *s,unsigned o,uint32_t *v)
{
    if (!timely(s) || s->ops.read32(s->context,s->plan.base+o,v)!=1) return fault(s);
    return timely(s);
}
static int wr(struct a733_spi *s,unsigned o,uint32_t v)
{
    if (!timely(s) || s->ops.write32(s->context,s->plan.base+o,v)!=1) return fault(s);
    return timely(s);
}
static int admission(struct a733_spi *s)
{
    if (!timely(s) || s->ops.admit(s->context,&s->format,&s->plan)!=1) return fault(s);
    return timely(s);
}
static int idle(struct a733_spi *s)
{
    uint32_t t,f,b;
    if (!rd(s,TCR,&t)||!rd(s,FSR,&f)||!rd(s,BSR,&b)) return 0;
    if ((t&XCH)||(f&FIFO_COUNTS)||(b&BUFFER_COUNTS)) return fault(s);
    return 1;
}
static int errors(struct a733_spi *s, uint32_t *v)
{
    if (!rd(s,ISR,v)) return 0;
    s->errors|=*v&ERRORS;
    if (s->errors) return fault(s);
    return 1;
}
int a733_spi_begin(struct a733_spi *s,const struct a733_spi_ops *ops,void *ctx,
                   const struct a733_spi_format *f,uint64_t deadline)
{
    struct a733_spi_plan p;
    uint32_t g;
    if (!s||!ops||!f||s->state!=A733_SPI_CLOSED || !deadline ||
        !ops->admit||!ops->read32||!ops->write32||!ops->read8||!ops->write8||!ops->now_us ||
        f->cs>3||f->mode>3||f->lsb_first>1||f->active_high>1 ||
        f->lanes>4||f->lanes==3||f->dma>1||(f->dma&&!a733_spi_dma_request(f->port)) ||
        a733_spi_plan(f->port,f->max_hz,&p)) return -1;
    s->plan=p;s->format=*f;s->ops=*ops;s->context=ctx;s->deadline=deadline;
    s->last_now=0;s->errors=0;s->sent=0;s->received=0;s->dma_requests=0;
    s->state=A733_SPI_RESETTING;
    if (!admission(s)||!rd(s,GCR,&g)) return s->state;
    if ((g&(GCR_SRST|1u)) || !idle(s)) { fault(s);return s->state; }
    /* Reset only an exclusively owned disabled controller. */
    wr(s,GCR,GCR_SRST);
    return s->state;
}
static int overlap(const void *a,size_t an,const void *b,size_t bn)
{ return a733_extents_intersect((uintptr_t)a,an,(uintptr_t)b,bn); }
static int requests_off(struct a733_spi *s)
{
    uint32_t v;
    if (!wr(s,FCR,FIFO_CONTROL)||!rd(s,FCR,&v)) return 0;
    if (v!=FIFO_CONTROL) return fault(s);
    s->dma_requests=0;return 1;
}
int a733_spi_submit(struct a733_spi *s,const struct a733_spi_transfer *t,uint64_t deadline)
{
    uint32_t n=0,st,v,fcr;
    if (!s||!t||s->state!=A733_SPI_READY||!deadline) return -1;
    /* DMA names bus memory only; CPU transfers borrow disjoint buffers. */
    if (t->dma ? t->dma!=1||!s->format.dma||t->tx||t->rx :
        !a733_span_incl_or_null_empty(t->tx,t->tx_bytes)||!a733_span_incl_or_null_empty(t->rx,t->rx_bytes) ||
        overlap(t->tx,t->tx_bytes,t->rx,t->rx_bytes)||
        overlap(s,sizeof(*s),t->tx,t->tx_bytes)||overlap(s,sizeof(*s),t->rx,t->rx_bytes)) return -1;
    unsigned lanes=t->lanes?t->lanes:1;
    if ((lanes!=1&&lanes!=2&&lanes!=4)||lanes>(s->format.lanes?s->format.lanes:1u)||
        (lanes==1?t->single_bytes!=0:t->kind==A733_SPI_FULL_DUPLEX||t->single_bytes>t->tx_bytes)) return -1;
    switch(t->kind) {
    case A733_SPI_FULL_DUPLEX: if (!t->tx_bytes||t->tx_bytes!=t->rx_bytes) return -1;n=t->tx_bytes;break;
    case A733_SPI_WRITE: if (!t->tx_bytes||t->rx_bytes) return -1;n=t->tx_bytes;break;
    case A733_SPI_READ: if (t->tx_bytes||!t->rx_bytes) return -1;n=t->rx_bytes;break;
    case A733_SPI_WRITE_READ:
        if (!t->tx_bytes||!t->rx_bytes||t->tx_bytes>0xffffffu||t->rx_bytes>0xffffffu-t->tx_bytes) return -1;
        n=t->tx_bytes+t->rx_bytes;break;
    default:return -1;
    }
    if (n>0xffffffu) return -1;
    s->deadline=deadline;
    if (!admission(s)||!idle(s)||!errors(s,&st)) return s->state;
    if (t->dma) {
        if (!rd(s,NDMA,&v)||!rd(s,FCR,&fcr)) return s->state;
        if (v!=0xe5u||fcr!=FIFO_CONTROL) { fault(s);return s->state; }
    }
    s->transfer=*t;s->sent=0;s->received=0;
    uint32_t control=s->tcr | (t->kind==A733_SPI_FULL_DUPLEX?0:0x100u);
    /* STC counts the leading single-lane TX bytes; the rest use DRM/Quad_EN. */
    uint32_t bcc=lanes==1?t->tx_bytes:t->single_bytes|(lanes==2?DUAL:QUAD);
    if (!wr(s,ISR,STATUS)||!wr(s,MBC,n)||!wr(s,MTC,t->tx_bytes)||
        !wr(s,BCC,bcc)||!wr(s,TCR,control)) return s->state;
    /* Software CS remains asserted over the entire transaction. */
    control=(control&~0x80u)|(s->format.active_high?0x80u:0);
    if (!wr(s,TCR,control)) return s->state;
    if (t->dma) {
        /* The caller's channels are already active; record the requests
         * before the write so an uncertain write is never assumed off. */
        s->dma_requests=(t->tx_bytes?TX_DRQ:0)|(t->rx_bytes?RX_DRQ:0);
        if (!wr(s,FCR,FIFO_CONTROL|s->dma_requests)||!rd(s,FCR,&v)) return s->state;
        if (v!=(FIFO_CONTROL|s->dma_requests)) { fault(s);return s->state; }
    }
    s->wait_since=s->last_now;s->state=A733_SPI_SETUP;
    return s->state;
}
static int move(struct a733_spi *s,unsigned budget)
{
    uint32_t f;
    if (!rd(s,FSR,&f)) return 0;
    unsigned rx=f&255u,tx=(f>>16)&255u;
    if (rx>192||tx>64||rx>s->transfer.rx_bytes-s->received) return fault(s);
    /* FSR bounds each FIFO batch; the deadline is checked around each batch
     * rather than around every byte. */
    unsigned count=rx<budget?rx:budget;
    for(unsigned i=0;i<count;i++) {
        uint8_t byte;
        if (s->ops.read8(s->context,s->plan.base+RXD,&byte)!=1) return fault(s);
        s->transfer.rx[s->received++]=byte;
    }
    if (count&&!timely(s)) return 0;
    count=64u-tx;if(count>budget)count=budget;
    if(count>s->transfer.tx_bytes-s->sent)count=s->transfer.tx_bytes-s->sent;
    for(unsigned i=0;i<count;i++) {
        if (s->ops.write8(s->context,s->plan.base+TXD,s->transfer.tx[s->sent])!=1) return fault(s);
        s->sent++;
    }
    return !count||timely(s);
}
int a733_spi_poll(struct a733_spi *s,unsigned budget)
{
    uint32_t v,t,f,b;
    if(!s||!budget||budget>64)return -1;
    if(s->state==A733_SPI_CLOSED||s->state==A733_SPI_READY||
       s->state==A733_SPI_COMPLETE||s->state==A733_SPI_FAULT)return s->state;
    if(!admission(s))return s->state;
    switch(s->state) {
    case A733_SPI_RESETTING:
        if(!rd(s,GCR,&v)||v&GCR_SRST)break;
        s->tcr=0x2040u|s->format.mode|(s->format.cs<<4)|
            (s->format.lsb_first?0x1000u:0)|(s->format.active_high?0:0x84u);
        if(!wr(s,GCR,0x82)||!wr(s,TCR,s->tcr)||!wr(s,IER,0)||
           !wr(s,BAT,0)||!wr(s,WCR,0)||!wr(s,FCR,RESETS|0x00200001u))break;
        s->state=A733_SPI_FIFO_RESET;break;
    case A733_SPI_FIFO_RESET:
        if(!rd(s,FCR,&v)||v&RESETS)break;
        if(!idle(s)||!wr(s,ISR,STATUS)||!wr(s,GCR,0x83))break;
        s->state=A733_SPI_READY;break;
    case A733_SPI_SETUP:
        if(s->last_now-s->wait_since<s->format.setup_us)break;
        if((!s->transfer.dma&&!move(s,budget))||!rd(s,TCR,&t)||!wr(s,TCR,t|XCH))break;
        s->state=A733_SPI_ACTIVE;break;
    case A733_SPI_ACTIVE:
        if(!errors(s,&v)||(!s->transfer.dma&&(!move(s,budget)||!errors(s,&v))))break;
        if(!(v&DONE))break;
        if(!rd(s,TCR,&t)||!rd(s,FSR,&f)||!rd(s,BSR,&b))break;
        if(t&XCH)break;
        if(!s->transfer.dma&&s->sent!=s->transfer.tx_bytes) {fault(s);break;}
        /* DMA completion waits for the RX channel to empty the FIFO. */
        if((!s->transfer.dma&&s->received!=s->transfer.rx_bytes)||
           (f&FIFO_COUNTS)||(b&BUFFER_COUNTS))break;
        s->wait_since=s->last_now;s->state=A733_SPI_HOLD;break;
    case A733_SPI_HOLD:
        if(s->last_now-s->wait_since<s->format.hold_us)break;
        /* Gate DMA requests before the final error and idle checks, so a
         * channel longer than the transfer cannot refill the TX FIFO. */
        if((s->transfer.dma&&!requests_off(s))||!errors(s,&v)||!idle(s)||
           !wr(s,TCR,s->tcr)||!wr(s,ISR,DONE))break;
        s->state=A733_SPI_COMPLETE;break;
    default:fault(s);break;
    }
    return s->state;
}
int a733_spi_dma_stop(struct a733_spi *s,uint64_t deadline)
{
    if(!s||!deadline||!s->transfer.dma||(s->state!=A733_SPI_SETUP&&
       s->state!=A733_SPI_ACTIVE&&s->state!=A733_SPI_HOLD))return -1;
    /* A fresh bounded cleanup deadline permits only request gating. The
     * transaction is abandoned: CS, buffers and leases stay retained. */
    s->deadline=deadline;
    if(admission(s)&&requests_off(s))fault(s);
    return s->state;
}
int a733_spi_reap(struct a733_spi *s)
{
    if(!s||s->state!=A733_SPI_COMPLETE)return -1;
    s->transfer=(struct a733_spi_transfer){0};s->state=A733_SPI_READY;
    return s->state;
}
int a733_spi_close(struct a733_spi *s,uint64_t deadline)
{
    if(!s||s->state!=A733_SPI_READY||!deadline)return -1;
    s->deadline=deadline;
    uint32_t v;
    if(!admission(s)||!idle(s)||!rd(s,FCR,&v))return s->state;
    if(v&(TX_DRQ|RX_DRQ)){fault(s);return s->state;}
    if(!wr(s,IER,0)||!wr(s,GCR,0x82))return s->state;
    if(rd(s,GCR,&v)) {if(v!=0x82)fault(s);else s->state=A733_SPI_CLOSED;}
    return s->state;
}
