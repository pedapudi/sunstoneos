/* Host model of one A733 two-wire controller on a bus with attached devices,
 * driving the interrupt-driven engine in drivers/i2c/a733_i2c_bus.c.
 *
 * The model follows manual 18.11.6 and 18.11.9: START or a byte write to DATA
 * followed by writing INT_FLAG makes the controller run the bus phase and set
 * INT_FLAG with the next status; M_STP ends the transaction with STAT 0xf8 and
 * no flag. Each case counts the START, STOP and flag-release writes, so a
 * missing STOP after a NACK or an extra STOP after lost arbitration fails. */
#include "a733_i2c_bus.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum phase { IDLE, STARTED, TX, RX, STUCK };
struct device { int present; uint8_t memory[16]; unsigned written, nack_after; };
struct model {
    uint32_t cntr, stat, data, ccr, efr, srst, lcr_low;
    unsigned flag, phase, addr, rw, rx_index, starts, stops, writes, advances;
    unsigned arbitrate_at, error_at, bogus_at, ack_log, ack_count, irq_line_after_stop;
    struct device dev[128];
};
static void reset_model(struct model *m)
{
    memset(m, 0, sizeof(*m));
    m->stat = 0xf8;
}
static uint32_t mread(void *c, uint32_t o)
{
    struct model *m = c;
    switch (o) {
    case A733_I2C_DATA: return m->data;
    case A733_I2C_CNTR: return (m->cntr & 0xc4) | (m->flag ? 8 : 0);
    case A733_I2C_STAT: return m->stat;
    case A733_I2C_CCR: return m->ccr;
    case A733_I2C_SRST: return m->srst;
    case A733_I2C_EFR: return m->efr;
    case A733_I2C_LCR: return m->lcr_low ? 0x20 : 0x30;
    default: assert(!"unmodelled read");
    }
    return 0;
}
static void status(struct model *m, uint32_t s)
{
    ++m->advances;
    if (m->arbitrate_at && m->advances == m->arbitrate_at) s = 0x38;
    if (m->error_at && m->advances == m->error_at) s = 0x00;
    if (m->bogus_at && m->advances == m->bogus_at) s = 0xe0;
    m->stat = s; m->flag = 1;
    if (s == 0x38 || s == 0x00 || s == 0xe0) m->phase = STUCK;
}
static void advance(struct model *m)
{
    struct device *d = &m->dev[m->addr];
    switch (m->phase) {
    case STARTED:
        m->addr = m->data >> 1; m->rw = m->data & 1; d = &m->dev[m->addr];
        m->rx_index = 0;
        m->phase = !d->present ? STUCK : m->rw ? RX : TX;
        status(m, !m->rw ? (d->present ? 0x18 : 0x20) : (d->present ? 0x40 : 0x48));
        break;
    case TX:
        if (d->nack_after && d->written >= d->nack_after) { m->phase = STUCK; status(m, 0x30); break; }
        if (d->written < sizeof(d->memory)) d->memory[d->written] = (uint8_t)m->data;
        ++d->written; status(m, 0x28);
        break;
    case RX:
        /* The ACK bit in the write that released the flag is what the master sends for this byte. */
        m->data = d->memory[m->rx_index++ % sizeof(d->memory)];
        m->ack_log |= ((m->cntr & A733_I2C_CNTR_ACK) ? 1u : 0u) << m->ack_count++;
        status(m, (m->cntr & A733_I2C_CNTR_ACK) ? 0x50 : 0x58);
        break;
    default: break;
    }
}
static void mwrite(void *c, uint32_t o, uint32_t v)
{
    struct model *m = c;
    ++m->writes;
    switch (o) {
    case A733_I2C_DATA: m->data = v & 255; return;
    case A733_I2C_CCR: m->ccr = v; return;
    case A733_I2C_EFR: m->efr = v; return;
    case A733_I2C_SRST: m->srst = 0; return; /* completes immediately */
    case A733_I2C_CNTR: break;
    default: assert(!"unmodelled write");
    }
    unsigned had = m->flag;
    m->cntr = v;
    if (v & A733_I2C_CNTR_FLAG) m->flag = 0;
    if (v & A733_I2C_CNTR_STOP) {
        ++m->stops; m->phase = IDLE; m->stat = 0xf8; m->flag = 0; m->cntr &= ~A733_I2C_CNTR_STOP; return;
    }
    if (v & A733_I2C_CNTR_START) {
        ++m->starts; m->stat = m->phase == IDLE ? 0x08 : 0x10; m->phase = STARTED; m->flag = 1;
        m->cntr &= ~A733_I2C_CNTR_START; return;
    }
    if (had && (v & A733_I2C_CNTR_FLAG)) advance(m);
}
/* Deliver interrupts until the line is quiet, as the GIC would for a level source. */
static unsigned run(struct a733_i2c_engine *e, struct model *m)
{
    unsigned delivered = 0;
    while ((m->cntr & A733_I2C_CNTR_INT_EN) && m->flag) {
        assert(delivered < 200);
        ++delivered;
        a733_i2c_engine_irq(e);
    }
    return delivered;
}
static void setup(struct a733_i2c_engine *e, struct model *m)
{
    reset_model(m);
    struct a733_i2c_io io = {mread, mwrite, m};
    a733_i2c_engine_init(e, &io);
    a733_i2c_engine_reset(e);
    assert(a733_i2c_engine_reset_done(e));
    a733_i2c_engine_enable(e, 0x1b);
    m->dev[0x50].present = 1; m->dev[0x68].present = 1;
    for (unsigned i = 0; i < 16; ++i) m->dev[0x50].memory[i] = (uint8_t)(0xa0 + i);
}
static void clock_words(void)
{
    uint32_t word, rate;
    /* 24 MHz input: 100 kHz needs a divisor of 240, which is exact (N=1, M=11 per the manual's example). */
    assert(a733_i2c_clock_word(24000000, 100000, &word, &rate) && rate == 100000 && !(word & 0x180));
    assert((24000000u / (10u << (word & 7)) / (((word >> 3) & 15) + 1)) == 100000);
    assert(a733_i2c_clock_word(24000000, 400000, &word, &rate) && rate == 400000 && (word & 0x100));
    /* The manual's example (N=1, M=2) gives the same rate; the engine takes the finest sampling clock. */
    assert((24000000u / ((1u << (word & 7)) * (((word >> 3) & 15) + 1u) * 10u)) == 400000);
    assert(a733_i2c_clock_word(100000000, 400000, &word, &rate) && rate <= 400000 && rate > 300000);
    /* Brute force: no divisor gives a higher rate at or below the target, for several inputs. */
    static const uint32_t inputs[] = {24000000, 50000000, 100000000, 75000000, 19200000, 26000000, 3000000, 1000000};
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i)
        for (unsigned t = 0; t < 2; ++t) {
            uint32_t target = t ? 400000 : 100000, w, r;
            assert(a733_i2c_clock_word(inputs[i], target, &w, &r));
            assert(r <= target);
            uint32_t decoded = inputs[i] / ((1u << (w & 7)) * (((w >> 3) & 15) + 1u) * 10u);
            assert(decoded == r);
            for (unsigned n = 0; n < 8; ++n)
                for (unsigned mm = 0; mm < 16; ++mm) {
                    uint64_t d = (1ull << n) * (mm + 1) * 10;
                    if (inputs[i] <= (uint64_t)target * d) assert(inputs[i] / d <= r);
                }
        }
    assert(!a733_i2c_clock_word(5, 100000, &word, &rate));   /* below 1 Hz at the lowest rate */
    assert(!a733_i2c_clock_word(0, 100000, &word, &rate) && !a733_i2c_clock_word(24000000, 0, &word, &rate));
}
static void table(void)
{
    uintptr_t seen[A733_I2C_BUS_COUNT]; unsigned ints[A733_I2C_BUS_COUNT];
    for (unsigned i = 0; i < A733_I2C_BUS_COUNT; ++i) {
        const struct a733_i2c_bus_data *d = a733_i2c_bus_data(i);
        assert(d && d->name && !(d->base & 0xfff) && d->gate && d->reset);
        for (unsigned j = 0; j < i; ++j) assert(seen[j] != d->base && ints[j] != d->intid);
        seen[i] = d->base; ints[i] = d->intid;
    }
    assert(!a733_i2c_bus_data(A733_I2C_BUS_COUNT));
    /* Manual 4.1.6.147 to 4.1.6.150, and the device tree's SPI 11 to 14 and 205. */
    assert(a733_i2c_bus_data(A733_I2C_TWI0)->base == 0x02510000 && a733_i2c_bus_data(A733_I2C_TWI3)->bgr == 0x02002e8c);
    assert(a733_i2c_bus_data(A733_I2C_TWI0)->intid == 43 && a733_i2c_bus_data(A733_I2C_TWI3)->intid == 46);
    assert(a733_i2c_bus_data(A733_I2C_S_TWI2)->intid == 237 && a733_i2c_bus_data(A733_I2C_S_TWI2)->gate == 4 &&
           a733_i2c_bus_data(A733_I2C_S_TWI2)->reset == 0x40000 && a733_i2c_bus_data(A733_I2C_S_TWI2)->parent == A733_I2C_PARENT_APBS1);
}
int main(void)
{
    clock_words();
    table();
    struct a733_i2c_engine e; struct model m;
    uint8_t buffer[8];

    /* Zero-length probe of an absent address: NACK, one STOP, two interrupts (START, address). */
    setup(&e, &m);
    struct a733_i2c_message probe = {0x30, 0, NULL, 0};
    assert(a733_i2c_engine_begin(&e, &probe, 1));
    assert(e.active && !a733_i2c_engine_begin(&e, &probe, 1));   /* a second begin is refused and changes nothing */
    assert(run(&e, &m) == 2 && !e.active);
    assert(e.result == A733_I2C_NACK_ADDRESS && e.last_status == 0x20 && m.starts == 1 && m.stops == 1);
    assert(a733_i2c_engine_idle(&e) && e.spurious == 0);
    /* The same engine probes a present address and succeeds. */
    probe.address = 0x50;
    assert(a733_i2c_engine_begin(&e, &probe, 1) && run(&e, &m) == 2 && e.result == A733_I2C_OK && m.stops == 2);

    /* Write then repeated-start read: register pointer, then four bytes with the last one not acknowledged. */
    setup(&e, &m);
    uint8_t pointer[1] = {0};
    struct a733_i2c_message pair[2] = {{0x50, 0, pointer, 1}, {0x50, A733_I2C_READ, buffer, 4}};
    assert(a733_i2c_engine_begin(&e, pair, 2) && run(&e, &m) > 0);
    assert(e.result == A733_I2C_OK && m.starts == 2 && m.stops == 1);
    assert(m.dev[0x50].written == 1 && m.dev[0x50].memory[0] == 0);   /* the register byte went to the device */
    for (unsigned i = 0; i < 4; ++i) assert(buffer[i] == (i ? 0xa0 + i : 0));
    assert(m.ack_count == 4 && m.ack_log == 0x7);                    /* ACK, ACK, ACK, NACK */

    /* A one-byte read is not acknowledged at all. */
    setup(&e, &m);
    struct a733_i2c_message one = {0x50, A733_I2C_READ, buffer, 1};
    assert(a733_i2c_engine_begin(&e, &one, 1) && run(&e, &m) > 0 && e.result == A733_I2C_OK);
    assert(m.ack_count == 1 && m.ack_log == 0 && buffer[0] == 0xa0);

    /* Data NACK after two bytes: STOP, NACK_DATA, only the accepted bytes written. */
    setup(&e, &m);
    m.dev[0x50].nack_after = 2;
    uint8_t out[4] = {1, 2, 3, 4};
    struct a733_i2c_message w = {0x50, 0, out, 4};
    assert(a733_i2c_engine_begin(&e, &w, 1) && run(&e, &m) > 0);
    assert(e.result == A733_I2C_NACK_DATA && m.stops == 1 && m.dev[0x50].written == 2 && e.last_status == 0x30);

    /* Read NACKed at the address phase (status 0x48). */
    setup(&e, &m);
    struct a733_i2c_message absent_read = {0x31, A733_I2C_READ, buffer, 2};
    assert(a733_i2c_engine_begin(&e, &absent_read, 1) && run(&e, &m) > 0);
    assert(e.result == A733_I2C_NACK_ADDRESS && e.last_status == 0x48 && m.stops == 1);

    /* Lost arbitration: the flag is released and no STOP is sent. */
    setup(&e, &m);
    m.arbitrate_at = 1;
    assert(a733_i2c_engine_begin(&e, &probe, 1) && run(&e, &m) > 0);
    assert(e.result == A733_I2C_ARBITRATION && m.stops == 0 && !m.flag && !e.active);

    /* Bus error and an impossible status each end with a STOP. */
    setup(&e, &m);
    m.error_at = 1;
    assert(a733_i2c_engine_begin(&e, &probe, 1) && run(&e, &m) > 0);
    assert(e.result == A733_I2C_BUS_ERROR && m.stops == 1);
    setup(&e, &m);
    m.bogus_at = 1;
    assert(a733_i2c_engine_begin(&e, &probe, 1) && run(&e, &m) > 0);
    assert(e.result == A733_I2C_UNEXPECTED && e.last_status == 0xe0 && m.stops == 1);
    /* A status that is legal only for the other direction is also refused. */
    setup(&e, &m);
    assert(a733_i2c_engine_begin(&e, &one, 1));
    m.stat = 0x18; m.flag = 1;   /* a write acknowledgement during a read */
    assert(run(&e, &m) > 0 && e.result == A733_I2C_UNEXPECTED);

    /* Deadline abort: STOP is requested, the transfer ends as TIMEOUT, and a late interrupt is spurious. */
    setup(&e, &m);
    assert(a733_i2c_engine_begin(&e, &probe, 1));
    assert(a733_i2c_engine_abort(&e) && e.result == A733_I2C_TIMEOUT && m.stops == 1 && !e.active);
    assert(!a733_i2c_engine_abort(&e));
    unsigned writes = m.writes;
    assert(!a733_i2c_engine_irq(&e) && m.writes == writes && e.spurious == 1);
    assert(a733_i2c_engine_idle(&e));
    m.lcr_low = 1;   /* a line held low is not an idle bus */
    assert(!a733_i2c_engine_idle(&e) && !a733_i2c_engine_lines_high(&e));
    m.lcr_low = 0;

    /* A flag with no transfer active is released with a STOP and counted. */
    setup(&e, &m);
    m.flag = 1; m.stat = 0x08;
    assert(!a733_i2c_engine_irq(&e) && m.stops == 1 && !m.flag && e.spurious == 1);

    /* Invalid requests change nothing on the bus. */
    setup(&e, &m);
    const unsigned setup_writes = m.writes;
    struct a733_i2c_message bad[4] = {{0x80, 0, NULL, 0}, {0x10, A733_I2C_READ, buffer, 0}, {0x10, 0, NULL, 2}, {0x10, 2, NULL, 0}};
    for (unsigned i = 0; i < 4; ++i) { assert(!a733_i2c_engine_begin(&e, &bad[i], 1) && e.result == A733_I2C_REFUSED); }
    assert(!a733_i2c_engine_begin(&e, NULL, 1) && !a733_i2c_engine_begin(&e, bad, 0));
    assert(m.starts == 0 && m.writes == setup_writes && !e.active);

    /* Full-range scan of an empty bus: NACK on every address, STOP each time, bus released. */
    setup(&e, &m);
    memset(m.dev, 0, sizeof(m.dev));
    unsigned irqs = 0;
    for (unsigned a = 0; a < 128; ++a) {
        struct a733_i2c_message p = {(uint16_t)a, 0, NULL, 0};
        assert(a733_i2c_engine_begin(&e, &p, 1));
        irqs += run(&e, &m);
        assert(e.result == A733_I2C_NACK_ADDRESS && e.last_status == 0x20);
    }
    assert(m.starts == 128 && m.stops == 128 && irqs == 256 && a733_i2c_engine_idle(&e));
    /* The same scan finds the two attached devices. */
    m.dev[0x50].present = m.dev[0x68].present = 1;
    unsigned found = 0;
    for (unsigned a = 0; a < 128; ++a) {
        struct a733_i2c_message p = {(uint16_t)a, 0, NULL, 0};
        assert(a733_i2c_engine_begin(&e, &p, 1)); run(&e, &m);
        found += e.result == A733_I2C_OK;
    }
    assert(found == 2);
    puts("PASS i2c engine: probe, write, combined read, NACK, arbitration, error, abort, spurious, scan, clock words, table");
    return 0;
}
