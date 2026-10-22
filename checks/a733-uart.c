#include "a733_uart.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct model {
    uint32_t regs[64], dll, dlh, cause, lsr_error;
    unsigned accesses, writes, reads, clocks, fail_access, rx, tx_space, tx_count, update_pending;
    unsigned trace_offset[64], trace_value[64], trace_count;
    uint64_t now;
    int backwards, late, shift_busy;
    unsigned ninth, last_tx, stuck_offset, reserved_zero;
    uintptr_t base; /* Zero selects UART1. */
};
static int now(void *p, uint64_t *t)
{ struct model *m=p; ++m->clocks; *t=m->backwards ? m->now-1 : m->now; return 1; }
static int read32(void *p, uintptr_t addr, uint32_t *v)
{
    struct model *m=p; unsigned o=(unsigned)(addr-(m->base ? m->base : 0x02501000u));
    assert(!(o&3) && (o<=0xa4 || o==0xc0));
    if (++m->accesses == m->fail_access) return 0;
    ++m->reads;
    if (o==0) { assert(!(m->regs[3]&128) && m->rx); *v=(0x80u + --m->rx) | (m->ninth ? 0x100u : 0); }
    else if (o==8) *v=m->cause;
    else if (o==0x14) { *v=(m->shift_busy ? 0x20u : 0x60u) | (m->rx ? 1u : 0u) | m->lsr_error; m->lsr_error=0; }
    else if (o==0x7c) *v=(m->tx_space ? 2u : 0u)|4u|(m->rx ? 9u : 0u);
    else if (o==0xa4) *v=m->update_pending ? 7 : 3;
    else *v=m->regs[o/4];
    if (m->late) m->now=1000;
    return 1;
}
static int write32(void *p, uintptr_t addr, uint32_t v)
{
    struct model *m=p; unsigned o=(unsigned)(addr-(m->base ? m->base : 0x02501000u));
    assert(!(o&3) && (o<=0x10 || o==0xa4 || o==0xc0));
    if (++m->accesses == m->fail_access) return -1;
    if (m->stuck_offset && o==m->stuck_offset) return 1;
    ++m->writes;
    if (m->trace_count<64) {
        m->trace_offset[m->trace_count]=o; m->trace_value[m->trace_count++]=v;
    }
    if (o==0 && (m->regs[3]&128)) m->dll=v;
    else if (o==4 && (m->regs[3]&128)) m->dlh=v;
    else if (o==0) { assert(m->tx_space); --m->tx_space; ++m->tx_count; m->last_tx=v; }
    else if (o==0xc0 && m->reserved_zero) m->regs[o/4]=v&~8u;
    else m->regs[o/4]=v;
    return 1;
}
static const struct a733_uart_ops ops={read32,write32,now};
static struct a733_uart_format format(void)
{ return (struct a733_uart_format){1,8,2,0,1,24000000,1500000,0}; }
static void ready(struct a733_uart *u, struct model *m)
{
    struct a733_uart_format f=format();
    memset(u,0,sizeof(*u)); memset(m,0,sizeof(*m));m->now=1;m->tx_space=256;m->cause=1;
    assert(a733_uart_begin(u,&ops,m,&f,1000)==A733_UART_CONFIGURING);
    m->update_pending=1;
    unsigned writes=m->writes;
    assert(a733_uart_poll(u)==A733_UART_CONFIGURING && m->writes==writes);
    m->update_pending=0;
    assert(a733_uart_poll(u)==A733_UART_READY);
    assert(m->dll==1 && m->dlh==0 && m->regs[3]==3 && m->regs[4]==0x22);
}
static void plan_cases(void)
{
    struct a733_uart_plan p, saved;
    struct a733_uart_format f=format();
    for (unsigned port=0;port<9;++port) {
        f.port=port;f.auto_flow=0; assert(a733_uart_plan(&f,&p)==1);
        assert(p.base==(port<7 ? 0x02500000u+port*4096 : 0x07080000u+(port-7)*4096));
        assert(p.fifo_bytes==(port && port<7 ? 256u : 64u));
        assert(p.fcr==(port && port<7 ? 0x81u : 1u) && p.fcr==a733_uart_fcr(port));
        f.auto_flow=1;
        assert(a733_uart_plan(&f,&p)==(port && port<7 ? 1 : -1));
    }
    f=format();f.baud=115200;f.max_error_ppm=2000;
    assert(a733_uart_plan(&f,&p)==1 && p.divisor==13);
    saved=p;f.max_error_ppm=0;
    assert(a733_uart_plan(&f,&p)==-1 && !memcmp(&p,&saved,sizeof(p)));
    f=format();f.data_bits=5;f.stop_half_bits=3;f.parity=2;
    assert(a733_uart_plan(&f,&p)==1 && p.lcr==28);
    f.stop_half_bits=4;assert(a733_uart_plan(&f,&p)==-1);
    f=format();f.baud=1;assert(a733_uart_plan(&f,&p)==-1);
}
/* Line modes: each case checks the MCR/RS485 control writes, the ninth-bit
 * data path and the refusals that leave hardware untouched. */
static void configured(struct a733_uart *u, struct model *m, const struct a733_uart_format *f)
{
    memset(u,0,sizeof(*u)); memset(m,0,sizeof(*m));m->now=1;m->tx_space=256;m->cause=1;
    m->base=0x02500000u+f->port*0x1000u;
    assert(a733_uart_begin(u,&ops,m,f,1000)==A733_UART_CONFIGURING);
    assert(a733_uart_poll(u)==A733_UART_READY);
}
static void modes(void)
{
    struct a733_uart u; struct model m; struct a733_uart_result r;
    struct a733_uart_byte rx[2]; const uint8_t tx[]={0x5a,0xa5};
    assert(a733_uart_dma_request(0)==14 && a733_uart_dma_request(1)==15 && a733_uart_dma_request(6)==20 &&
           a733_uart_dma_request(7)==50 && a733_uart_dma_request(8)==51 && !a733_uart_dma_request(9));
    struct a733_uart_format f=format();f.parity=2;
    configured(&u,&m,&f);assert(m.regs[3]==0x1b);
    assert(a733_uart_mode(&u,A733_UART_MODE_RS485,1000)==A733_UART_READY);
    assert(u.mode==A733_UART_MODE_RS485 && m.regs[4]==0xa2 && m.regs[0xc0/4]==0x18);
    /* Even parity select transmits the ninth bit set; RBR bit 8 is returned. */
    assert(a733_uart_exchange(&u,0,0,tx,1,1,1000,&r)==A733_UART_READY && m.last_tx==0x15a);
    m.rx=1;m.ninth=1;
    assert(a733_uart_exchange(&u,rx,2,0,0,2,1000,&r)==A733_UART_READY && r.received==1);
    assert(rx[0].value==0x80 && rx[0].status==A733_UART_NINTH_BIT);
    assert(a733_uart_mode(&u,A733_UART_MODE_UART,1000)==A733_UART_READY);
    assert(u.mode==A733_UART_MODE_UART && m.regs[4]==0x22 && !m.regs[0xc0/4]);
    m.rx=1;assert(a733_uart_exchange(&u,rx,2,tx+1,1,2,1000,&r)==A733_UART_READY);
    assert(!rx[0].status && m.last_tx==0xa5);
    /* Odd parity select transmits a clear ninth bit. */
    f.parity=1;configured(&u,&m,&f);
    assert(a733_uart_mode(&u,A733_UART_MODE_RS485,1000)==A733_UART_READY);
    assert(a733_uart_exchange(&u,0,0,tx,1,1,1000,&r)==A733_UART_READY && m.last_tx==0x5a);
    /* Loopback keeps the planned flow control. */
    f=format();configured(&u,&m,&f);
    assert(a733_uart_mode(&u,A733_UART_MODE_LOOPBACK,1000)==A733_UART_READY && m.regs[4]==0x32);
    /* Refusals: no parity, no RTS route, IrDA with flow control, too fast
     * for SIR, armed THRE, invalid mode and an active DMA lease. */
    unsigned n=m.accesses;
    assert(a733_uart_mode(&u,A733_UART_MODE_RS485,1000)==-1);
    assert(a733_uart_mode(&u,A733_UART_MODE_IRDA,1000)==-1);
    assert(a733_uart_mode(&u,(enum a733_uart_mode)4,1000)==-1 && m.accesses==n);
    assert(a733_uart_tx_irq(&u,1,1000)==A733_UART_READY);n=m.accesses;
    assert(a733_uart_mode(&u,A733_UART_MODE_UART,1000)==-1 && m.accesses==n);
    f=format();f.port=0;f.auto_flow=0;f.parity=2;configured(&u,&m,&f);n=m.accesses;
    assert(a733_uart_mode(&u,A733_UART_MODE_RS485,1000)==-1);
    assert(a733_uart_mode(&u,A733_UART_MODE_IRDA,1000)==-1 && m.accesses==n);
    f.baud=115200;f.max_error_ppm=2000;configured(&u,&m,&f);
    assert(a733_uart_mode(&u,A733_UART_MODE_IRDA,1000)==A733_UART_READY && m.regs[4]==0x40 && !m.regs[0xc0/4]);
    u.dma_owned=1;n=m.accesses;
    assert(a733_uart_mode(&u,A733_UART_MODE_UART,1000)==-1 && m.accesses==n);
    /* A busy line leaves the mode unchanged; readback mismatches fault. */
    f=format();f.parity=2;configured(&u,&m,&f);m.shift_busy=1;
    assert(a733_uart_mode(&u,A733_UART_MODE_RS485,1000)==A733_UART_READY && !u.mode && m.regs[4]==0x22);
    m.shift_busy=0;m.stuck_offset=0xc0;
    assert(a733_uart_mode(&u,A733_UART_MODE_RS485,1000)==A733_UART_FAULT);
    /* Reserved RS-485 control bit 3 may read back zero in either direction. */
    configured(&u,&m,&f);m.reserved_zero=1;
    assert(a733_uart_mode(&u,A733_UART_MODE_RS485,1000)==A733_UART_READY && m.regs[0xc0/4]==0x10);
    assert(a733_uart_mode(&u,A733_UART_MODE_UART,1000)==A733_UART_READY && !u.mode);
    configured(&u,&m,&f);
    assert(a733_uart_mode(&u,A733_UART_MODE_RS485,1000)==A733_UART_READY);
    m.regs[0xc0/4]=0x8;m.stuck_offset=0xc0;
    assert(a733_uart_mode(&u,A733_UART_MODE_UART,1000)==A733_UART_READY && !u.mode);
    configured(&u,&m,&f);
    assert(a733_uart_mode(&u,A733_UART_MODE_RS485,1000)==A733_UART_READY);
    m.regs[0xc0/4]=0x10;m.stuck_offset=0xc0;
    assert(a733_uart_mode(&u,A733_UART_MODE_UART,1000)==A733_UART_FAULT);
    configured(&u,&m,&f);m.stuck_offset=0x10;
    assert(a733_uart_mode(&u,A733_UART_MODE_LOOPBACK,1000)==A733_UART_FAULT);
}
int main(int argc, char **argv)
{
    struct a733_uart u; struct model m; struct a733_uart_result r;
    struct a733_uart_byte rx[4]; const uint8_t tx[]={1,2,3,4};
    if (argc>1 && !strcmp(argv[1], "--capacity")) {
        ready(&u,&m);m.rx=2;m.lsr_error=4;m.cause=6;
        assert(a733_uart_interrupt(&u,0,0,0,0,4,1000,&r)==A733_UART_READY);
        assert(m.lsr_error==4 && m.rx==2 && !r.received);
        assert(a733_uart_interrupt(&u,rx,1,0,0,4,1000,&r)==A733_UART_READY);
        assert(rx[0].status==4 && m.rx==1 && r.received==1);
        m.lsr_error=8;
        assert(a733_uart_interrupt(&u,rx,1,0,0,4,1000,&r)==A733_UART_READY);
        assert(rx[0].status==8 && !m.rx && r.received==1);
        puts("UART receive capacity preserves unread byte line status.");
        return 0;
    }
    if (argc>1 && !strcmp(argv[1], "--modes")) {
        modes();
        puts("UART RS-485 9-bit, IrDA SIR and loopback modes passed.");
        return 0;
    }
    if (argc>1 && !strcmp(argv[1], "--drain")) {
        ready(&u,&m);m.shift_busy=1;
        assert(a733_uart_drain(&u,1000)==A733_UART_DRAINING);
        assert(a733_uart_poll(&u)==A733_UART_DRAINING);
        m.shift_busy=0;
        assert(a733_uart_poll(&u)==A733_UART_DRAINED);
        puts("UART drain waits for the shift register after FIFO empty.");
        return 0;
    }
    plan_cases();ready(&u,&m);
    if (argc>1) {
        for(unsigned i=0;i<m.trace_count;++i) printf("%x %x\n",m.trace_offset[i],m.trace_value[i]);
        return 0;
    }
    /* RX line flags are captured before the byte read. Both loops stop at the
     * budget even when a peer continually supplies bytes or TX space. */
    m.rx=3;m.lsr_error=0x1e;m.tx_space=1;
    assert(a733_uart_exchange(&u,rx,4,tx,4,2,1000,&r)==A733_UART_READY);
    assert(r.received==2 && r.transmitted==1 && rx[0].status==0x1e && r.errors==0x1e);
    assert(u.errors==0x1e && (u.ier&2) && m.rx==1);
    unsigned n=m.accesses;
    assert(a733_uart_exchange(&u,rx,4,(uint8_t *)&u,1,1,1000,&r)==-1 && m.accesses==n);
    /* A receive timeout consumes only available bytes and never writes TX. */
    m.cause=12;
    assert(a733_uart_interrupt(&u,rx,4,tx+1,3,2,1000,&r)==A733_UART_READY);
    assert(r.received==1 && !r.transmitted && r.cause==12);
    m.cause=2;m.tx_space=256;
    assert(a733_uart_interrupt(&u,rx,4,tx+1,3,2,1000,&r)==A733_UART_READY);
    assert(r.transmitted==2 && (u.ier&2));
    assert(a733_uart_interrupt(&u,rx,4,tx+3,1,2,1000,&r)==A733_UART_READY);
    assert(r.transmitted==1 && !(u.ier&2));
    m.cause=0;m.regs[6]=0x11;
    assert(a733_uart_interrupt(&u,rx,4,0,0,1,1000,&r)==A733_UART_READY && r.modem==0x11);
    m.cause=1;n=m.accesses;
    assert(a733_uart_interrupt(&u,rx,4,0,0,1,1000,&r)==A733_UART_READY && m.accesses==n+1);
    /* A clean 64-byte burst keeps per-byte line status but checks the
     * deadline only around the batch. */
    {
        struct a733_uart_byte burst[64];
        m.rx=64;m.reads=0;m.clocks=0;
        assert(a733_uart_exchange(&u,burst,64,0,0,64,1000,&r)==A733_UART_READY);
        assert(r.received==64 && !m.rx && m.reads==128 && m.clocks<=4);
        for (unsigned i=0;i<64;++i) assert(burst[i].value==0x80u+63u-i && !burst[i].status);
        m.rx=2;m.reads=0;m.clocks=0;
        assert(a733_uart_exchange(&u,burst,64,0,0,64,1000,&r)==A733_UART_READY);
        assert(r.received==2 && m.reads==5 && m.clocks<=4);
    }
    assert(a733_uart_drain(&u,1000)==A733_UART_DRAINING);
    assert(a733_uart_poll(&u)==A733_UART_DRAINED);
    ready(&u,&m);m.rx=1;
    assert(a733_uart_drain(&u,1000)==A733_UART_DRAINING);
    assert(a733_uart_poll(&u)==A733_UART_DRAINING && m.rx==1);
    m.now=1000;assert(a733_uart_poll(&u)==A733_UART_FAULT);n=m.accesses;
    assert(a733_uart_poll(&u)==A733_UART_FAULT && m.accesses==n);
    ready(&u,&m);m.fail_access=m.accesses+2;
    assert(a733_uart_exchange(&u,0,0,tx,4,1,1000,&r)==A733_UART_FAULT);
    n=m.accesses;assert(a733_uart_exchange(&u,0,0,tx,4,1,1000,&r)==A733_UART_FAULT && m.accesses==n);
    ready(&u,&m);m.late=1;
    assert(a733_uart_interrupt(&u,rx,4,0,0,1,1000,&r)==A733_UART_FAULT);
    ready(&u,&m);m.backwards=1;
    assert(a733_uart_interrupt(&u,rx,4,0,0,1,1000,&r)==A733_UART_FAULT);
    ready(&u,&m);m.cause=7;
    assert(a733_uart_interrupt(&u,rx,4,0,0,1,1000,&r)==A733_UART_FAULT);
    /* An update timeout never reaches FIFO traffic or READY. */
    memset(&u,0,sizeof(u));memset(&m,0,sizeof(m));m.now=1;
    struct a733_uart_format f=format();
    assert(a733_uart_begin(&u,&ops,&m,&f,10)==A733_UART_CONFIGURING);
    m.now=10;assert(a733_uart_poll(&u)==A733_UART_FAULT);
    puts("UART configuration, FIFO, interrupt, drain and retained-fault model passed.");
    return 0;
}
