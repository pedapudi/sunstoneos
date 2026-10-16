#include "a733_i2c.h"
#include "boot.h"

#define TWI 0x07083000UL
#define CONTROL (TWI + 0x0c)
#define STATUS (TWI + 0x10)
#define ENABLE 0x40U
#define FLAG 0x08U

enum i2c_result a733_i2c_open(struct a733_i2c *bus)
{
    bus->opened = 0;
    bus->last_status = bus->phase = 0;
    if ((mmio_read(0x0701019c) & 0x10001) != 0x10001 ||
        (mmio_read(0x07025000) & 0xff) != 0x22)
        return I2C_UNAVAILABLE;
    uint32_t ctl = mmio_read(CONTROL);
    bus->last_status = mmio_read(STATUS);
    if (!(ctl & ENABLE) || mmio_read(TWI + 0x1c) != 0)
        return I2C_UNAVAILABLE;
    if ((ctl & 0xb0) || bus->last_status != 0xf8 ||
        (mmio_read(TWI + 0x20) & 0x35) != 0x30)
        return I2C_BUSY;
    bus->control = ctl;
    bus->opened = 1;
    return I2C_OK;
}

static enum i2c_result step(struct a733_i2c *bus, unsigned control,
                            unsigned status, uint64_t deadline)
{
    ++bus->phase;
    mmio_write(CONTROL, ENABLE | FLAG | control);
    while (!(mmio_read(CONTROL) & FLAG)) {
        if (counter() >= deadline) {
            bus->last_status = mmio_read(STATUS);
            return I2C_TIMEOUT;
        }
    }
    bus->last_status = mmio_read(STATUS);
    return bus->last_status == status ? I2C_OK : I2C_STATUS;
}

enum i2c_result a733_i2c_read8(struct a733_i2c *bus, uint8_t address,
                             uint8_t reg, uint8_t *value)
{
    if (!bus->opened || address > 0x7f || !value) return I2C_UNAVAILABLE;
    bus->last_status = mmio_read(STATUS);
    if (bus->last_status != 0xf8) return I2C_BUSY;
    bus->phase = 0;
    uint64_t deadline = counter() + COUNTER_HZ / 100;
    enum i2c_result result = step(bus, 0x20, 0x08, deadline);
    if (result) goto finish;
    mmio_write(TWI + 8, address << 1);
    result = step(bus, 0, 0x18, deadline);
    if (result) goto finish;
    mmio_write(TWI + 8, reg);
    result = step(bus, 0, 0x28, deadline);
    if (result) goto finish;
    result = step(bus, 0x20, 0x10, deadline);
    if (result) goto finish;
    mmio_write(TWI + 8, (address << 1) | 1);
    result = step(bus, 0, 0x40, deadline);
    if (result) goto finish;
    result = step(bus, 0, 0x58, deadline); // Final byte is NACKed.
    uint8_t received = (uint8_t)mmio_read(TWI + 8);
finish:
    // Lost arbitration means another master owns the bus; do not issue STOP.
    if (bus->last_status == 0x38) {
        mmio_write(CONTROL, ENABLE | FLAG);
        bus->opened = 0;
        return result;
    }
    mmio_write(CONTROL, ENABLE | FLAG | 0x10);
    deadline = counter() + COUNTER_HZ / 100;
    while ((mmio_read(CONTROL) & 0x10) || mmio_read(STATUS) != 0xf8) {
        if (counter() >= deadline) {
            bus->opened = 0;
            return I2C_STOP_FAILED;
        }
    }
    if (result == I2C_OK) *value = received;
    return result;
}

void a733_i2c_close(struct a733_i2c *bus)
{
    if (bus->opened) {
        mmio_write(CONTROL, bus->control & ~(FLAG | 0x30));
        bus->opened = 0;
    }
}
