/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Register definitions and configuration sequence derived from:
 * Copyright(c) 2020 - 2023 Allwinner Technology Co.,Ltd. All rights reserved.
 * Reuuimlla Technology Co., Ltd. <www.reuuimllatech.com>
 * Emma <liujuan1@reuuimllatech.com>
 * 2013.6.6 Mintow <duanmintao@allwinnertech.com>
 * sunxi-uart-ng.h and sunxi-uart-ng-core.c, Linux BSP. */
#include "a733_uart.h"

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
#define HALT 0xa4u
#define ERRORS 0x9eu

static int span(const void *p, size_t n)
{
    return !n || (p && (uintptr_t)p <= UINTPTR_MAX - (n - 1));
}
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return an && bn && ((uintptr_t)a <= (uintptr_t)b ?
           (uintptr_t)b - (uintptr_t)a < an : (uintptr_t)a - (uintptr_t)b < bn);
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
    u->state = A733_UART_CONFIGURING;
    /* A fresh reset is mandatory. Refuse unexpected DLAB or active hardware. */
    if (!read_reg(u, LCR, &v)) return u->state;
    if (v || !read_reg(u, USR, &v)) { fault(u); return u->state; }
    if (v & 9u) { fault(u); return u->state; }
    if (!write_reg(u, IER, 0) || !write_reg(u, FCR, 1) ||
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
        !write_reg(u, FCR, 7) || !write_reg(u, FCR, 1) ||
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
    return r && span(rx, rn) && span(tx, tn) &&
        !overlap(u, sizeof(*u), rx, rn) && !overlap(u, sizeof(*u), tx, tn) &&
        !overlap(u, sizeof(*u), r, sizeof(*r)) && !overlap(rx, rn, tx, tn) &&
        !overlap(rx, rn, r, sizeof(*r)) && !overlap(tx, tn, r, sizeof(*r));
}
static int service(struct a733_uart *u, struct a733_uart_byte *rx, size_t rn,
                   const uint8_t *tx, size_t tn, unsigned budget, uint64_t deadline,
                   struct a733_uart_result *r, int irq)
{
    uint32_t v, data, ier;
    unsigned dorx = 1, dotx = 1;
    if (!u || !budget || budget > 256 || !deadline ||
        !buffers_ok(u, rx, rn, tx, tn, r)) return -1;
    if (u->state == A733_UART_FAULT) return u->state;
    if (u->state != A733_UART_READY) return -1;
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
    }
    if (dorx) {
        /* Do not clear the next byte's line flags without a destination. */
        for (unsigned n = 0; n < budget && r->received < rn; ++n) {
            if (!read_reg(u, LSR, &v)) return u->state;
            r->errors |= v & ERRORS; u->errors |= v & ERRORS;
            if (!(v & 1u)) break;
            if (!read_reg(u, RBR, &data)) return u->state;
            rx[r->received++] = (struct a733_uart_byte){(uint8_t)data, (uint8_t)(v & ERRORS)};
        }
    }
    if (dotx) {
        for (unsigned n = 0; n < budget && r->transmitted < tn; ++n) {
            if (!read_reg(u, USR, &v)) return u->state;
            if (!(v & 2u)) break;
            if (!write_reg(u, THR, tx[r->transmitted])) return u->state;
            ++r->transmitted;
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
    if (u->state!=A733_UART_READY) return -1;
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
    if (u->state != A733_UART_READY) return -1;
    u->deadline = deadline; u->state = A733_UART_DRAINING;
    if (!write_reg(u, IER, 0)) return u->state;
    u->ier = 0;
    return u->state;
}
