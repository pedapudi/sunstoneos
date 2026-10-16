#ifndef SUNSTONE_A733_I2C_H
#define SUNSTONE_A733_I2C_H
#include <stdint.h>

/* Early boot, single-owner access to the inherited always-on I2C bus.
 * Clock and pin setup must already be valid. No bus reset or recovery pulses.
 */
enum i2c_result {
    I2C_OK, I2C_UNAVAILABLE, I2C_BUSY, I2C_TIMEOUT, I2C_STATUS, I2C_STOP_FAILED
};
struct a733_i2c {
    uint32_t control;
    unsigned last_status, phase;
    int opened;
};
enum i2c_result a733_i2c_open(struct a733_i2c *bus);
enum i2c_result a733_i2c_read8(struct a733_i2c *bus, uint8_t address,
                             uint8_t reg, uint8_t *value);
void a733_i2c_close(struct a733_i2c *bus);
#endif
