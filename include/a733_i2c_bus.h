#ifndef SUNSTONE_A733_I2C_BUS_H
#define SUNSTONE_A733_I2C_BUS_H
/* Interrupt-driven master engine for the A733 two-wire controllers that the
 * 40-pin header exposes: TWI0 to TWI3 and S_TWI2. All five share the register
 * map of manual section 18.11.9 (TWI_CNTR, TWI_STAT, TWI_CCR, TWI_SRST,
 * TWI_EFR, TWI_LCR); they differ in base address, gate and reset word,
 * parent clock and interrupt. That per-part data is the table in
 * a733_i2c_bus_data(). Board routing (pins and header positions) is in the
 * board description (include/sunstone_board.h), not here.
 *
 * The engine performs no ownership, clock, pin or interrupt-controller
 * operation: the platform adapter that owns the controller does those, and
 * calls a733_i2c_engine_irq() from the controller interrupt. A transfer is a
 * list of messages that ends with one STOP; a repeated START separates
 * messages. Completion is reported by the interrupt that finishes the last
 * step; nothing polls. The engine sends STOP after any address or data NACK
 * and after a bus error, never after lost arbitration (another master owns
 * the bus). */
#include <stddef.h>
#include <stdint.h>

enum a733_i2c_bus_id {
    A733_I2C_TWI0, A733_I2C_TWI1, A733_I2C_TWI2, A733_I2C_TWI3, A733_I2C_S_TWI2,
    A733_I2C_BUS_COUNT
};
enum a733_i2c_parent { A733_I2C_PARENT_APB1, A733_I2C_PARENT_APBS1 };
struct a733_i2c_bus_data {
    const char *name;
    uintptr_t base;
    uintptr_t bgr;                  /* gate and reset word */
    uint32_t gate, reset;           /* bit masks within bgr */
    uintptr_t parent_register;      /* APB1 (CCU+0x518) or APBS1 (R_CCU+0x10) */
    enum a733_i2c_parent parent;
    uint16_t intid;                 /* GIC interrupt ID (SPI number + 32) */
};
const struct a733_i2c_bus_data *a733_i2c_bus_data(unsigned bus);

/* Register offsets and bits (manual 18.11.9). */
#define A733_I2C_DATA 0x08u
#define A733_I2C_CNTR 0x0cu
#define A733_I2C_STAT 0x10u
#define A733_I2C_CCR 0x14u
#define A733_I2C_SRST 0x18u
#define A733_I2C_EFR 0x1cu
#define A733_I2C_LCR 0x20u
#define A733_I2C_DRV_CTRL 0x200u
#define A733_I2C_DRV_INT_CTRL 0x210u
#define A733_I2C_CNTR_INT_EN 0x80u
#define A733_I2C_CNTR_BUS_EN 0x40u
#define A733_I2C_CNTR_START 0x20u
#define A733_I2C_CNTR_STOP 0x10u
#define A733_I2C_CNTR_FLAG 0x08u
#define A733_I2C_CNTR_ACK 0x04u
#define A733_I2C_STAT_IDLE 0xf8u
#define A733_I2C_LCR_SCL 0x20u
#define A733_I2C_LCR_SDA 0x10u

/* Clock register word for the target rate: Fscl = Fin / (2^N * (M+1) * 10),
 * N in bits 2:0 and M in bits 6:3. Rates above 100 kHz select the 30% duty
 * (bit 8), the vendor's fast-mode setting, so the low period meets 1.3 us at
 * 400 kHz; at or below 100 kHz the duty is 50% (bits 8:7 clear).
 * Chooses the highest rate not above target_hz. Returns 0 if input_hz cannot
 * produce a rate of at least 1 Hz at or below the target (input too low).
 * *rate_hz receives the programmed rate. */
int a733_i2c_clock_word(uint32_t input_hz, uint32_t target_hz, uint32_t *word, uint32_t *rate_hz);

enum a733_i2c_result {
    A733_I2C_PENDING,       /* not finished */
    A733_I2C_OK,
    A733_I2C_NACK_ADDRESS,  /* no device acknowledged the address: STOP sent */
    A733_I2C_NACK_DATA,     /* the device refused a data byte: STOP sent */
    A733_I2C_ARBITRATION,   /* another master won the bus: no STOP */
    A733_I2C_BUS_ERROR,     /* illegal START or STOP: STOP sent */
    A733_I2C_TIMEOUT,       /* aborted by the owner at its deadline */
    A733_I2C_UNEXPECTED,    /* a status code the protocol does not allow here: STOP sent */
    A733_I2C_REFUSED        /* invalid request: nothing happened */
};
#define A733_I2C_READ 1u
struct a733_i2c_message {
    uint16_t address;   /* seven-bit address, not shifted */
    uint16_t flags;     /* A733_I2C_READ or 0 */
    uint8_t *data;      /* read: destination; write: source (const in effect) */
    uint32_t length;    /* a write may have length 0 (address probe); a read needs at least 1 */
};
struct a733_i2c_io {
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);
    void *context;
};
struct a733_i2c_engine {
    struct a733_i2c_io io;
    const struct a733_i2c_message *messages;
    uint32_t count, index, cursor;
    uint32_t last_status, interrupts, spurious;
    enum a733_i2c_result result;
    uint8_t active;
};
void a733_i2c_engine_init(struct a733_i2c_engine *, const struct a733_i2c_io *);
/* Starts a transfer: validates the list, then requests START. 1 started, 0 refused
 * (a transfer is active, or the list is invalid; result becomes
 * A733_I2C_REFUSED for an invalid list). The messages stay unchanged and valid
 * until the transfer finishes. */
int a733_i2c_engine_begin(struct a733_i2c_engine *, const struct a733_i2c_message *, uint32_t count);
/* Controller interrupt: services one INT_FLAG. Returns 1 when this call
 * finished the transfer (result is final), 0 otherwise. A call without a flag
 * set counts as spurious and changes nothing. */
int a733_i2c_engine_irq(struct a733_i2c_engine *);
/* The owner's deadline expired with the transfer active: request STOP, mark
 * A733_I2C_TIMEOUT and finish. Returns 1 if a transfer was aborted. The bus is
 * not proven idle: check a733_i2c_engine_idle() before the next transfer. */
int a733_i2c_engine_abort(struct a733_i2c_engine *);
/* No transfer active, STAT is idle (0xf8), STOP and START requests are clear
 * and both lines are high. This is the observation after a STOP completes. */
int a733_i2c_engine_idle(struct a733_i2c_engine *);
int a733_i2c_engine_lines_high(struct a733_i2c_engine *);
/* Controller setup after clock and reset are released: soft reset (waited by
 * the caller, which owns the deadline), then clock word, enhance register and
 * bus enable with the interrupt enabled. a733_i2c_engine_reset_done() reports
 * SOFT_RST cleared. */
void a733_i2c_engine_reset(struct a733_i2c_engine *);
int a733_i2c_engine_reset_done(struct a733_i2c_engine *);
void a733_i2c_engine_enable(struct a733_i2c_engine *, uint32_t clock_word);
/* Mask the controller interrupt and disable the bus (before the gate closes). */
void a733_i2c_engine_disable(struct a733_i2c_engine *);
#endif
