#ifndef SUNSTONE_I2C_H
#define SUNSTONE_I2C_H
/* Header I2C class (docs/status/buses-rtc.md). An application declares the
 * controllers it uses in the "i2c" section of its manifest.json:
 *
 *   "i2c": [{"bus": "twi0", "max_rate_hz": 400000}, {"bus": "twi1", "max_rate_hz": 100000}]
 *
 * The OS grants a declared controller when the board description routes it to
 * the header and no other owner holds it (TWI0 to TWI3 and S_TWI2; the camera
 * buses TWI9 and TWI11, the Type-C bus S_TWI1 and the PMIC bus S_TWI0 are not
 * header buses). Controllers the manifest does not declare are never touched.
 * Bus numbers are enum a733_i2c_bus_id in include/a733_i2c_bus.h:
 * 0 TWI0, 1 TWI1, 2 TWI2, 3 TWI3, 4 S_TWI2.
 *
 * Every call returns at once: 1 done, 0 not yet (repeat the identical call
 * after ss_i2c_wait), -1 refused or failed. A transfer is a list of messages
 * ending with one STOP, with a repeated START between messages. The
 * controller interrupt completes it: the OS reads each received byte and
 * writes each sent byte in that interrupt, and wakes the application task at
 * the final one. The OS starts a transfer from its owner task, one hop; there
 * is no polling and no tick wait. A deadline bounds every transfer precisely:
 * at timeout_us after the start the owner requests STOP and reports
 * SS_I2C_TIMEOUT.
 *
 * The caller keeps the message list and its buffers unchanged and valid from
 * ss_i2c_submit until ss_i2c_result returns 1. One transfer per bus is in
 * flight. A bus whose state is uncertain (a transfer the OS could not end, or
 * a release it could not prove) is retained: every later call returns -1. */
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define SS_I2C_READ 1u
struct ss_i2c_message {
    uint16_t address;   /* seven-bit address, not shifted */
    uint16_t flags;     /* SS_I2C_READ or 0 */
    uint8_t *data;
    uint32_t length;    /* a write may be empty (an address probe); a read needs a byte */
};
enum ss_i2c_status {
    SS_I2C_OK = 1,
    SS_I2C_NACK_ADDRESS = 2,
    SS_I2C_NACK_DATA = 3,
    SS_I2C_ARBITRATION = 4,
    SS_I2C_BUS_ERROR = 5,
    SS_I2C_TIMEOUT = 6,
    SS_I2C_UNEXPECTED = 7
};
struct ss_i2c_result {
    int32_t status;             /* enum ss_i2c_status */
    uint32_t hardware_status;   /* the controller's last TWI_STAT code (0x20: address not acknowledged) */
    uint32_t messages;          /* index of the message in progress when the transfer ended */
    uint32_t reserved;
    uint64_t started_ticks;     /* system counter (24 MHz) when the OS requested START */
    uint64_t finished_ticks;    /* system counter in the interrupt that ended the transfer */
};
#define SS_I2C_LINES_UNSAMPLED 0xffffffffu
struct ss_i2c_info {
    uint32_t bus, rate_hz;      /* programmed SCL rate, at most the rate asked for */
    uint32_t input_hz;          /* the controller's parent clock */
    uint32_t clock_word;        /* TWI_CCR as programmed */
    uint32_t interrupts;        /* controller interrupts served */
    uint32_t spurious;          /* interrupts with no flag set, or a flag with no transfer */
    uint32_t strays;            /* interrupts that arrived while the bus was not live */
    uint32_t gic_intid;
    uint32_t idle_starts;       /* transfers that began with the previous STOP complete and both lines high */
    uint32_t busy_starts;       /* transfers that began before the bus read idle */
    uint32_t lines_open;        /* 1 SCL and SDA high, 0 one low, SS_I2C_LINES_UNSAMPLED: sampled once the bus is open */
    uint32_t lines_close;       /* the same, sampled by the owner before it closes the gate */
    uint32_t fault, fault_detail; /* the first failure of the bus, 0 for none */
};

/* Enable the controller at up to rate_hz (not above the manifest's max_rate_hz).
 * The OS takes the clock lease and the pin lease, releases the controller from
 * reset, programs the rate and unmasks the interrupt. -1 if the bus is not
 * granted, the rate is above the declaration, or the controller or its pins
 * are not in the state the OS expects. Repeating it on an open bus returns 1
 * and does nothing. */
int ss_i2c_open(unsigned bus, uint32_t rate_hz);
/* Start a transfer: 1 started, 0 the bus is not open yet or still has a transfer
 * or an uncollected result, -1 refused (not granted, invalid list). */
int ss_i2c_submit(unsigned bus, const struct ss_i2c_message *messages, uint32_t count, uint32_t timeout_us);
/* 1 finished (result is filled and the bus is free for the next submit), 0 still
 * running, -1 no transfer in flight. */
int ss_i2c_result(unsigned bus, struct ss_i2c_result *result);
/* Sleep until a transfer on any bus finishes or wait_us passes. Call the class
 * operation again after waking; wake-ups may coalesce. */
void ss_i2c_wait(uint32_t wait_us);
/* Counters and observations of the bus: valid at any time for a granted bus, and the way to read why an open failed
 * (fault, fault_detail) and the line samples taken at open and at close. 1 filled, -1 not granted. */
int ss_i2c_info(unsigned bus, struct ss_i2c_info *info);
/* Release the controller: interrupt masked, bus idle, gate and reset closed, pins
 * and clock lease returned. 1 closed, 0 in progress, -1 failed (the bus is retained).
 * The OS closes every open bus before it returns to the host; closing first is
 * how an application proves the release itself. */
int ss_i2c_close(unsigned bus);

#ifdef __cplusplus
}
#endif
#endif
