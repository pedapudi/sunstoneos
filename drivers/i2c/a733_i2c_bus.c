/* Interrupt-driven master engine for the header two-wire controllers.
 * Protocol and status codes: A733 User Manual V1.00 section 18.11.6 (TWI
 * engine operation) and 18.11.9 (registers). The sequence for each byte is the
 * manual's: the interrupt reports a status, software writes the next byte or
 * control word, and writing INT_FLAG (bit 3) releases the clock stretch. */
#include "a733_i2c_bus.h"

static const struct a733_i2c_bus_data buses[A733_I2C_BUS_COUNT] = {
    /* Main controllers: SPI 11 to 14, so INTIDs 43 to 46; gate 0 and reset 16 of
     * CCU+0xe80 to 0xe8c; source APB1 (CCU+0x518). */
    [A733_I2C_TWI0] = {"TWI0", 0x02510000, 0x02002e80, 1u, 1u << 16, 0x02002518, A733_I2C_PARENT_APB1, 43},
    [A733_I2C_TWI1] = {"TWI1", 0x02511000, 0x02002e84, 1u, 1u << 16, 0x02002518, A733_I2C_PARENT_APB1, 44},
    [A733_I2C_TWI2] = {"TWI2", 0x02512000, 0x02002e88, 1u, 1u << 16, 0x02002518, A733_I2C_PARENT_APB1, 45},
    [A733_I2C_TWI3] = {"TWI3", 0x02513000, 0x02002e8c, 1u, 1u << 16, 0x02002518, A733_I2C_PARENT_APB1, 46},
    /* S_TWI2: SPI 205, so INTID 237; R_CCU+0x19c gate 2 and reset 18; source APBS1
     * (R_CCU+0x10). The word is shared with S_TWI0 and S_TWI1. */
    [A733_I2C_S_TWI2] = {"S_TWI2", 0x07085000, 0x0701019c, 1u << 2, 1u << 18, 0x07010010, A733_I2C_PARENT_APBS1, 237},
};
const struct a733_i2c_bus_data *a733_i2c_bus_data(unsigned bus)
{ return bus < A733_I2C_BUS_COUNT ? &buses[bus] : NULL; }

int a733_i2c_clock_word(uint32_t input_hz, uint32_t target_hz, uint32_t *word, uint32_t *rate_hz)
{
    if (!input_hz || !target_hz || !word || !rate_hz) return 0;
    uint64_t best = 0;
    uint32_t best_word = 0;
    for (unsigned n = 0; n < 8; ++n)
        for (unsigned m = 0; m < 16; ++m) {
            uint64_t d = ((uint64_t)1 << n) * (m + 1u) * 10u;
            /* Not above the target; the smallest divisor gives the highest rate. */
            if ((uint64_t)input_hz > (uint64_t)target_hz * d) continue;
            if (!best || d < best) { best = d; best_word = (m << 3) | n; }
        }
    if (!best || input_hz / best == 0) return 0;
    *word = best_word | (target_hz > 100000u ? 1u << 8 : 0u);
    *rate_hz = (uint32_t)(input_hz / best);
    return 1;
}

static uint32_t rd(struct a733_i2c_engine *e, uint32_t o) { return e->io.read(e->io.context, o); }
static void wr(struct a733_i2c_engine *e, uint32_t o, uint32_t v) { e->io.write(e->io.context, o, v); }
#define CTRL (A733_I2C_CNTR_BUS_EN | A733_I2C_CNTR_INT_EN)

void a733_i2c_engine_init(struct a733_i2c_engine *e, const struct a733_i2c_io *io)
{
    *e = (struct a733_i2c_engine){.io = *io};
}
static int valid(const struct a733_i2c_message *m, uint32_t count)
{
    if (!m || !count) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (m[i].address > 0x7f || (m[i].flags & ~A733_I2C_READ)) return 0;
        if ((m[i].flags & A733_I2C_READ) && !m[i].length) return 0;
        if (m[i].length && !m[i].data) return 0;
    }
    return 1;
}
int a733_i2c_engine_begin(struct a733_i2c_engine *e, const struct a733_i2c_message *m, uint32_t count)
{
    if (e->active) return 0;
    if (!valid(m, count)) { e->result = A733_I2C_REFUSED; return 0; }
    e->messages = m; e->count = count; e->index = 0; e->cursor = 0;
    e->last_status = 0; e->result = A733_I2C_PENDING; e->active = 1;
    wr(e, A733_I2C_CNTR, CTRL | A733_I2C_CNTR_START);
    return 1;
}
static int finish(struct a733_i2c_engine *e, enum a733_i2c_result r, uint32_t control)
{
    wr(e, A733_I2C_CNTR, control);
    e->result = r; e->active = 0;
    return 1;
}
/* The message list is exhausted or the next message begins. */
static int next(struct a733_i2c_engine *e)
{
    if (e->index + 1 < e->count) {
        ++e->index; e->cursor = 0;
        wr(e, A733_I2C_CNTR, CTRL | A733_I2C_CNTR_FLAG | A733_I2C_CNTR_START);
        return 0;
    }
    return finish(e, A733_I2C_OK, CTRL | A733_I2C_CNTR_FLAG | A733_I2C_CNTR_STOP);
}
int a733_i2c_engine_irq(struct a733_i2c_engine *e)
{
    uint32_t control = rd(e, A733_I2C_CNTR);
    if (!(control & A733_I2C_CNTR_FLAG)) { ++e->spurious; return 0; }
    ++e->interrupts;
    uint32_t status = rd(e, A733_I2C_STAT);
    e->last_status = status;
    if (!e->active) {
        /* A flag with no transfer: release the controller, never leave a level
         * interrupt asserted. */
        wr(e, A733_I2C_CNTR, CTRL | A733_I2C_CNTR_FLAG | A733_I2C_CNTR_STOP);
        ++e->spurious;
        return 0;
    }
    const struct a733_i2c_message *m = &e->messages[e->index];
    const uint32_t reading = m->flags & A733_I2C_READ;
    switch (status) {
    case 0x08: case 0x10: /* START or repeated START sent: the address byte */
        wr(e, A733_I2C_DATA, ((uint32_t)m->address << 1) | (reading ? 1u : 0u));
        wr(e, A733_I2C_CNTR, CTRL | A733_I2C_CNTR_FLAG);
        return 0;
    case 0x18: /* address and write acknowledged */
    case 0x28: /* data byte acknowledged */
        if (reading) break;
        if (e->cursor < m->length) {
            wr(e, A733_I2C_DATA, m->data[e->cursor++]);
            wr(e, A733_I2C_CNTR, CTRL | A733_I2C_CNTR_FLAG);
            return 0;
        }
        return next(e);
    case 0x40: /* address and read acknowledged: acknowledge every byte but the last */
        if (!reading) break;
        wr(e, A733_I2C_CNTR, CTRL | A733_I2C_CNTR_FLAG | (m->length > 1 ? A733_I2C_CNTR_ACK : 0u));
        return 0;
    case 0x50: /* byte received and acknowledged */
        if (!reading) break;
        if (e->cursor < m->length) m->data[e->cursor++] = (uint8_t)rd(e, A733_I2C_DATA);
        wr(e, A733_I2C_CNTR, CTRL | A733_I2C_CNTR_FLAG | (e->cursor + 1 < m->length ? A733_I2C_CNTR_ACK : 0u));
        return 0;
    case 0x58: /* last byte received, not acknowledged */
        if (!reading) break;
        if (e->cursor < m->length) m->data[e->cursor++] = (uint8_t)rd(e, A733_I2C_DATA);
        return next(e);
    case 0x20: case 0x48:
        if (reading != (status == 0x48)) break;
        return finish(e, A733_I2C_NACK_ADDRESS, CTRL | A733_I2C_CNTR_FLAG | A733_I2C_CNTR_STOP);
    case 0x38: /* lost arbitration: release without STOP */
        return finish(e, A733_I2C_ARBITRATION, CTRL | A733_I2C_CNTR_FLAG);
    case 0x00:
        return finish(e, A733_I2C_BUS_ERROR, CTRL | A733_I2C_CNTR_FLAG | A733_I2C_CNTR_STOP);
    case 0x30:
        if (reading) break;
        return finish(e, A733_I2C_NACK_DATA, CTRL | A733_I2C_CNTR_FLAG | A733_I2C_CNTR_STOP);
    default:
        break;
    }
    return finish(e, A733_I2C_UNEXPECTED, CTRL | A733_I2C_CNTR_FLAG | A733_I2C_CNTR_STOP);
}
int a733_i2c_engine_abort(struct a733_i2c_engine *e)
{
    if (!e->active) return 0;
    return finish(e, A733_I2C_TIMEOUT, CTRL | A733_I2C_CNTR_FLAG | A733_I2C_CNTR_STOP);
}
int a733_i2c_engine_lines_high(struct a733_i2c_engine *e)
{
    return (rd(e, A733_I2C_LCR) & (A733_I2C_LCR_SCL | A733_I2C_LCR_SDA)) == (A733_I2C_LCR_SCL | A733_I2C_LCR_SDA);
}
int a733_i2c_engine_idle(struct a733_i2c_engine *e)
{
    uint32_t control = rd(e, A733_I2C_CNTR);
    return !e->active && !(control & (A733_I2C_CNTR_START | A733_I2C_CNTR_STOP | A733_I2C_CNTR_FLAG)) &&
        rd(e, A733_I2C_STAT) == A733_I2C_STAT_IDLE && a733_i2c_engine_lines_high(e);
}
void a733_i2c_engine_reset(struct a733_i2c_engine *e)
{
    wr(e, A733_I2C_EFR, 0);
    wr(e, A733_I2C_SRST, 1);
}
int a733_i2c_engine_reset_done(struct a733_i2c_engine *e)
{ return !(rd(e, A733_I2C_SRST) & 1u); }
void a733_i2c_engine_enable(struct a733_i2c_engine *e, uint32_t clock_word)
{
    wr(e, A733_I2C_CCR, clock_word);
    wr(e, A733_I2C_EFR, 0);
    wr(e, A733_I2C_CNTR, CTRL);
}
void a733_i2c_engine_disable(struct a733_i2c_engine *e)
{
    wr(e, A733_I2C_CNTR, 0);
}
