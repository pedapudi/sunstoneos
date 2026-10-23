/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register definitions and configuration sequence derived from:
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Reuuimlla Technology Co., Ltd. <www.reuuimllatech.com>
 * Emma <liujuan1@reuuimllatech.com>
 * 2013.6.6 Mintow <duanmintao@allwinnertech.com>
 * sunxi-uart-ng.h and sunxi-uart-ng-core.c, Linux BSP. */
#include "a733_uart.h"
#include "a733_range.h"

#define RBR 0x00u
#define THR 0x00u
#define DLL 0x00u
#define DLH 0x04u
#define IER 0x04u
#define IIR 0x08u
#define FCR 0x08u
#define LCR 0x0cu
#define MCR 0x10u
#define LSR 0x14u
#define MSR 0x18u
#define USR 0x7cu
#define HSK 0x88u        /* UART_HSK: 0xe5 selects DMA handshake mode. */
#define DMA_REQ_EN 0x8cu /* bit0 RX, bit1 TX DMA request enable. */
#define HALT 0xa4u
#define HALT_DMA 0xc2u   /* HALT DMA_PTE_RX(7), PTE(6), CHCFG_AT_BUSY(1). */
#define RS485_CTL 0xc0u  /* UART_485_CTL */
/* sunxi-uart-ng hardware 9-bit configuration: half duplex (DUPLEX, bit 4)
 * and RXBFA (bit 3, reserved in the manual); RCM 0 selects 9-bit receive. */
#define RS485_HALF_9BIT 0x18u
/* Readback compares the defined control bits and leaves out bits 6 and 5
 * (write-one-to-clear address-detect status) and bit 3, whose reserved
 * definition in the manual may read back zero. */
#define RS485_CHECKED 0x97u
#define ERRORS 0x9eu

static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return a733_extents_intersect((uintptr_t)a, an, (uintptr_t)b, bn);
}
int a733_uart_plan(const struct a733_uart_format *f, struct a733_uart_plan *out)
{
    struct a733_uart_plan p;
    uint64_t target, div, generated, delta;
    if (!f || !out || overlap(f, sizeof(*f), out, sizeof(*out)) ||
        f->port > 8 || f->data_bits < 5 || f->data_bits > 8 ||
        f->parity > 2 || f->auto_flow > 1 || !f->clock_hz || !f->baud ||
        f->max_error_ppm > 30000 ||
        (f->auto_flow && (!f->port || f->port > 6)) ||
        (f->stop_half_bits != 2 &&
         !(f->stop_half_bits == 3 && f->data_bits == 5) &&
         !(f->stop_half_bits == 4 && f->data_bits != 5))) return -1;
    target = (uint64_t)f->baud * 16;
    div = ((uint64_t)f->clock_hz + target / 2) / target;
    if (!div || div > 65535) return -1;
    generated = target * div;
    delta = generated > f->clock_hz ? generated - f->clock_hz : f->clock_hz - generated;
    if (delta * 1000000 > generated * f->max_error_ppm) return -1;
    p.base = f->port < 7 ? 0x02500000u + f->port * 0x1000u :
                          0x07080000u + (f->port - 7) * 0x1000u;
    p.fifo_bytes = f->port && f->port < 7 ? 256 : 64;
    p.lcr = f->data_bits - 5u;
    if (f->stop_half_bits != 2) p.lcr |= 4;
    if (f->parity) p.lcr |= 8u | (f->parity == 2 ? 16u : 0);
    p.mcr = f->auto_flow ? 0x22u : 0;
    p.fcr = a733_uart_fcr(f->port);
    p.divisor = (uint32_t)div;
    p.actual_baud = f->clock_hz / (16u * p.divisor);
    *out = p;
    return 1;
}
static int fault(struct a733_uart *u) { u->state = A733_UART_FAULT; return 0; }
static int clock_ok(struct a733_uart *u)
{
    uint64_t now;
    if (u->ops.now_us(u->context, &now) != 1 || now < u->last_now ||
        now >= u->deadline) return fault(u);
    u->last_now = now;
    return 1;
}
static int read_reg(struct a733_uart *u, unsigned off, uint32_t *v)
{
    if (!clock_ok(u)) return 0;
    if (u->ops.read32(u->context, u->plan.base + off, v) != 1) return fault(u);
    return clock_ok(u);
}
static int write_reg(struct a733_uart *u, unsigned off, uint32_t v)
{
    if (!clock_ok(u)) return 0;
    if (u->ops.write32(u->context, u->plan.base + off, v) != 1) return fault(u);
    return clock_ok(u);
}
/* FIFO batches check the deadline before and after the batch instead of
 * around every access. Each access still faults on an uncertain result. */
static int fifo_read(struct a733_uart *u, unsigned off, uint32_t *v)
{ return u->ops.read32(u->context, u->plan.base + off, v) == 1 || fault(u); }
static int fifo_write(struct a733_uart *u, unsigned off, uint32_t v)
{ return u->ops.write32(u->context, u->plan.base + off, v) == 1 || fault(u); }
int a733_uart_begin(struct a733_uart *u, const struct a733_uart_ops *ops, void *context,
                    const struct a733_uart_format *format, uint64_t deadline)
{
    struct a733_uart_plan plan;
    uint32_t v;
    if (!u || !ops || !ops->read32 || !ops->write32 || !ops->now_us ||
        u->state != A733_UART_CLOSED || !deadline ||
        overlap(u, sizeof(*u), ops, sizeof(*ops)) ||
        overlap(u, sizeof(*u), format, sizeof(*format)) ||
        a733_uart_plan(format, &plan) != 1) return -1;
    u->plan = plan; u->ops = *ops; u->context = context;
    u->deadline = deadline; u->last_now = 0; u->ier = 0; u->errors = 0;
    u->mode = A733_UART_MODE_UART;
    u->state = A733_UART_CONFIGURING;
    /* A fresh reset is mandatory. Refuse unexpected DLAB or active hardware. */
    if (!read_reg(u, LCR, &v)) return u->state;
    if (v || !read_reg(u, USR, &v)) { fault(u); return u->state; }
    if (v & 9u) { fault(u); return u->state; }
    if (!write_reg(u, IER, 0) || !write_reg(u, FCR, plan.fcr) ||
        !write_reg(u, HALT, 3) || !write_reg(u, LCR, plan.lcr | 0x80u) ||
        !write_reg(u, DLL, plan.divisor & 255u) ||
        !write_reg(u, DLH, plan.divisor >> 8) || !write_reg(u, HALT, 7)) return u->state;
    return u->state;
}
int a733_uart_poll(struct a733_uart *u)
{
    uint32_t v, lsr;
    if (!u) return -1;
    if (u->state == A733_UART_FAULT || u->state == A733_UART_DRAINED) return u->state;
    if (u->state == A733_UART_DRAINING) {
        if (!read_reg(u, LSR, &lsr)) return u->state;
        u->errors |= lsr & ERRORS;
        if (!read_reg(u, USR, &v)) return u->state;
        if ((lsr & 0x41u) == 0x40u && !(v & 9u) && (v & 4u))
            u->state = A733_UART_DRAINED;
        return u->state;
    }
    if (u->state != A733_UART_CONFIGURING) return -1;
    if (!read_reg(u, HALT, &v) || (v & 4u)) return u->state;
    if (!write_reg(u, LCR, u->plan.lcr) || !write_reg(u, HALT, 2) ||
        !write_reg(u, FCR, u->plan.fcr | 6u) || !write_reg(u, FCR, u->plan.fcr) ||
        !write_reg(u, MCR, u->plan.mcr)) return u->state;
    if (!read_reg(u, LCR, &v)) return u->state;
    if ((v & 255u) != u->plan.lcr) { fault(u); return u->state; }
    if (!read_reg(u, MCR, &v)) return u->state;
    if ((v & 255u) != u->plan.mcr) { fault(u); return u->state; }
    u->ier = 5u | (u->plan.mcr ? 8u : 0u);
    if (!write_reg(u, IER, u->ier)) return u->state;
    u->state = A733_UART_READY;
    return u->state;
}
static int buffers_ok(struct a733_uart *u, struct a733_uart_byte *rx, size_t rn,
                      const uint8_t *tx, size_t tn, struct a733_uart_result *r)
{
    if (rn > SIZE_MAX / sizeof(*rx)) return 0;
    rn *= sizeof(*rx);
    return r && a733_span_incl_or_empty(rx, rn) && a733_span_incl_or_empty(tx, tn) &&
        !overlap(u, sizeof(*u), rx, rn) && !overlap(u, sizeof(*u), tx, tn) &&
        !overlap(u, sizeof(*u), r, sizeof(*r)) && !overlap(rx, rn, tx, tn) &&
        !overlap(rx, rn, r, sizeof(*r)) && !overlap(tx, tn, r, sizeof(*r));
}
static int service(struct a733_uart *u, struct a733_uart_byte *rx, size_t rn,
                   const uint8_t *tx, size_t tn, unsigned budget, uint64_t deadline,
                   struct a733_uart_result *r, int irq)
{
    uint32_t v, data, ier;
    unsigned dorx = 1, dotx = 1, timed = 0;
    if (!u || !budget || budget > 256 || !deadline ||
        !buffers_ok(u, rx, rn, tx, tn, r)) return -1;
    if (u->state == A733_UART_FAULT) return u->state;
    if (u->state != A733_UART_READY || u->dma_owned || u->dma_released) return -1;
    *r = (struct a733_uart_result){0}; r->cause = 1;
    u->deadline = deadline;
    if (irq) {
        if (!read_reg(u, IIR, &v)) return u->state;
        r->cause = v & 15u;
        if (r->cause == 1) return u->state;
        dorx = r->cause == 4 || r->cause == 6 || r->cause == 12;
        dotx = r->cause == 2;
        if (r->cause == 7) {
            /* USR acknowledges BUSY; unexpected configuration races retain fault. */
            if (read_reg(u, USR, &v)) fault(u);
            return u->state;
        }
        if (!dorx && !dotx && r->cause != 0) { fault(u); return u->state; }
        if (r->cause == 0) {
            if (!read_reg(u, MSR, &v)) return u->state;
            r->modem = v & 255u;
        }
        timed = 1; /* The IIR/MSR read just checked the deadline. */
    }
    if (dorx && rn) {
        if (!timed && !clock_ok(u)) return u->state;
        /* Do not clear the next byte's line flags without a destination. */
        for (unsigned n = 0; n < budget && r->received < rn; ++n) {
            if (!fifo_read(u, LSR, &v)) return u->state;
            r->errors |= v & ERRORS; u->errors |= v & ERRORS;
            if (!(v & 1u)) break;
            if (!fifo_read(u, RBR, &data)) return u->state;
            /* RS485 9-bit receive returns the address bit in RBR bit 8. */
            uint8_t status = (uint8_t)(v & ERRORS);
            if (u->mode == A733_UART_MODE_RS485) status |= (uint8_t)((data >> 8) & A733_UART_NINTH_BIT);
            rx[r->received++] = (struct a733_uart_byte){(uint8_t)data, status};
        }
        if (!clock_ok(u)) return u->state;
        timed = 1;
    }
    if (dotx) {
        if (tn) {
            if (!timed && !clock_ok(u)) return u->state;
            for (unsigned n = 0; n < budget && r->transmitted < tn; ++n) {
                if (!fifo_read(u, USR, &v)) return u->state;
                if (!(v & 2u)) break;
                /* As in sunxi-uart-ng, the 9-bit transmitter takes the
                 * ninth bit in THR bit 8 from the even-parity select. */
                uint32_t ninth = u->mode == A733_UART_MODE_RS485 && (u->plan.lcr & 0x10u) ? 0x100u : 0;
                if (!fifo_write(u, THR, tx[r->transmitted] | ninth)) return u->state;
                ++r->transmitted;
            }
            if (!clock_ok(u)) return u->state;
        }
        ier = (u->ier & ~2u) | (r->transmitted < tn ? 2u : 0u);
        if (ier != u->ier) {
            if (!write_reg(u, IER, ier)) return u->state;
            u->ier = ier;
        }
    }
    return u->state;
}
int a733_uart_exchange(struct a733_uart *u, struct a733_uart_byte *rx, size_t rn,
                       const uint8_t *tx, size_t tn, unsigned budget, uint64_t deadline,
                       struct a733_uart_result *r)
{ return service(u, rx, rn, tx, tn, budget, deadline, r, 0); }
int a733_uart_tx_irq(struct a733_uart *u,unsigned enabled,uint64_t deadline)
{
    if (!u || enabled>1 || !deadline) return -1;
    if (u->state==A733_UART_FAULT) return u->state;
    if (u->state!=A733_UART_READY || u->dma_owned || u->dma_released) return -1;
    u->deadline=deadline;
    uint32_t ier=(u->ier&~2u)|(enabled?2u:0u);
    if (!write_reg(u,IER,ier)) return u->state;
    u->ier=ier;return u->state;
}
int a733_uart_interrupt(struct a733_uart *u, struct a733_uart_byte *rx, size_t rn,
                        const uint8_t *tx, size_t tn, unsigned budget, uint64_t deadline,
                        struct a733_uart_result *r)
{ return service(u, rx, rn, tx, tn, budget, deadline, r, 1); }
int a733_uart_drain(struct a733_uart *u, uint64_t deadline)
{
    if (!u || !deadline) return -1;
    if (u->state == A733_UART_FAULT) return u->state;
    if (u->state != A733_UART_READY || u->dma_owned) return -1;
    u->deadline = deadline; u->state = A733_UART_DRAINING;
    if (!write_reg(u, IER, 0)) return u->state;
    u->ier = 0;
    return u->state;
}
int a733_uart_mode(struct a733_uart *u, enum a733_uart_mode mode, uint64_t deadline)
{
    uint32_t lsr, usr, v, mcr, ctl;
    if (!u || !deadline || (unsigned)mode > A733_UART_MODE_LOOPBACK) return -1;
    if (u->state == A733_UART_FAULT) return u->state;
    if (u->state != A733_UART_READY || u->dma_owned || u->dma_released || (u->ier & 2u)) return -1;
    switch (mode) {
    case A733_UART_MODE_UART: mcr = u->plan.mcr; break;
    case A733_UART_MODE_LOOPBACK: mcr = u->plan.mcr | 0x10u; break;
    case A733_UART_MODE_IRDA:
        /* IrDA 1.0 SIR tops out at 115200 baud, within its 0.87% rate
         * tolerance (115384 baud from 24 MHz), and has no RTS/CTS. */
        if (u->plan.mcr || u->plan.actual_baud > 116202) return -1;
        mcr = 0x40u; break;
    default:
        /* Automatic RTS drives the transceiver enable, so the plan must route
         * RTS/CTS (UART1..6). 9-bit mode needs eight data bits and parity. */
        if (u->plan.mcr != 0x22u || (u->plan.lcr & 0x0bu) != 0x0bu) return -1;
        mcr = 0x80u | u->plan.mcr; break;
    }
    u->deadline = deadline;
    if (!read_reg(u, LSR, &lsr)) return u->state;
    u->errors |= lsr & ERRORS;
    if (!read_reg(u, USR, &usr)) return u->state;
    if ((lsr & 0x41u) != 0x40u || (usr & 9u) || !(usr & 4u)) return u->state;
    ctl = mode == A733_UART_MODE_RS485 ? RS485_HALF_9BIT : 0;
    /* Leave RS-485 before changing MCR; enter it after MCR selects it. */
    if (u->mode == A733_UART_MODE_RS485 && !ctl &&
        (!write_reg(u, RS485_CTL, 0) || !read_reg(u, RS485_CTL, &v))) return u->state;
    if (u->mode == A733_UART_MODE_RS485 && !ctl && (v & RS485_CHECKED)) { fault(u); return u->state; }
    if (!write_reg(u, MCR, mcr) || !read_reg(u, MCR, &v)) return u->state;
    if ((v & 255u) != mcr) { fault(u); return u->state; }
    if (ctl && (!write_reg(u, RS485_CTL, ctl) || !read_reg(u, RS485_CTL, &v))) return u->state;
    if (ctl && (v & RS485_CHECKED) != (ctl & RS485_CHECKED)) { fault(u); return u->state; }
    u->mode = mode;
    return u->state;
}

/* A733 manual 18.10.7: mode1, byte-wide handshake, both DMA requests gated.
 * FCR is write-only (the same read address is IIR), so do not read it back. */
int a733_uart_dma_prepare(struct a733_uart *u,uint64_t deadline)
{
    uint32_t v;
    if(!u || !deadline || u->state!=A733_UART_READY || u->dma_owned || u->dma_released)return -1;
    u->deadline=deadline;u->dma_owned=1;
    if(!write_reg(u,DMA_REQ_EN,0) || !read_reg(u,DMA_REQ_EN,&v))return u->state;
    if(v){fault(u);return u->state;}
    if(!write_reg(u,IER,0) || !write_reg(u,HSK,0xe5) ||
       !write_reg(u,FCR,9) || !write_reg(u,HALT,HALT_DMA))return u->state;
    u->ier=0;u->dma_requests=0;
    if(!read_reg(u,HSK,&v))return u->state;
    if(v!=0xe5){fault(u);return u->state;}
    if(!read_reg(u,HALT,&v))return u->state;
    if(v!=HALT_DMA)fault(u);
    return u->state;
}
int a733_uart_dma_requests(struct a733_uart *u,unsigned enabled,uint64_t deadline)
{
    uint32_t v;
    if(!u || !deadline || enabled>1 || !u->dma_owned || u->state!=A733_UART_READY)return -1;
    u->deadline=deadline;
    if(!write_reg(u,DMA_REQ_EN,enabled?3:0) || !read_reg(u,DMA_REQ_EN,&v))return u->state;
    if(v!=(enabled?3u:0u)){fault(u);return u->state;}
    u->dma_requests=enabled;return u->state;
}
int a733_uart_dma_status(struct a733_uart *u,uint64_t deadline)
{
    uint32_t v;
    if(!u || !deadline || !u->dma_owned || u->state!=A733_UART_READY)return -1;
    u->deadline=deadline;
    if(read_reg(u,LSR,&v))u->errors|=v&ERRORS;
    return u->state;
}
int a733_uart_dma_release(struct a733_uart *u,unsigned quiet,uint64_t deadline)
{
    uint32_t lsr,v;
    if(!u || !deadline || quiet!=1 || !u->dma_owned || u->dma_requests ||
       u->state!=A733_UART_READY)return -1;
    u->deadline=deadline;
    if(!read_reg(u,DMA_REQ_EN,&v))return u->state;
    if(v){fault(u);return u->state;}
    if(!read_reg(u,LSR,&lsr))return u->state;
    u->errors|=lsr&ERRORS;
    if(!read_reg(u,USR,&v))return u->state;
    if((lsr&0x41)!=0x40 || (v&9) || !(v&4))return 0;
    /* Keep handshake registers until reset: requests remain disabled. IER
     * stays 0 and FCR in DMA mode, so release is terminal: only drain and
     * close follow. */
    u->dma_owned=0;u->dma_released=1;return u->state;
}
