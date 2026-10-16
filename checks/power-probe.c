#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_i2c.h"

static StaticTask_t task;
static StackType_t stack[1024];

static void report(const char *name, uint64_t value)
{
    platform_puts(name); platform_hex(value); platform_puts("\n");
}

static void probe(void *unused)
{
    (void)unused;
    report("R TWI gate=", mmio_read(0x0701019c));
    report("PL configuration=", mmio_read(0x07025000));
    struct a733_i2c bus;
    enum i2c_result result = a733_i2c_open(&bus);
    report("I2C open result=", result);
    report("I2C status=", bus.last_status);
    if (result) goto done;
    report("I2C clock divisor=", mmio_read(0x07083014));
    report("I2C inherited control=", bus.control);
    // Read only the normal register page and documented identity/rail settings.
    // Writing a register address before repeated START does not program it.
    const uint8_t registers[] = {0xff, 0x0e, 0x0f, 0x03, 0x10, 0x11,
        0x12, 0x19, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x2f, 0x31, 0x33, 0x39};
    for (unsigned i = 0; i < sizeof(registers); ++i) {
        uint8_t value = 0;
        result = a733_i2c_read8(&bus, 0x36, registers[i], &value);
        report("PMIC register=", registers[i]);
        if (result) {
            report("I2C transfer result=", result);
            report("I2C phase=", bus.phase);
            report("I2C status=", bus.last_status);
            break;
        }
        report("value=", value);
        if ((i == 0 && value != 0) || (i == 1 && value != 3)) {
            platform_puts("INCOMPLETE unexpected PMIC page or identity\n");
            result = I2C_UNAVAILABLE;
            break;
        }
    }
    a733_i2c_close(&bus);
    if (!result) platform_puts("PASS PMIC identity and rail reads\n");
done:
    platform_puts("COMPLETE power probe\n");
    platform_exit(result == I2C_OK ? 0 : 1);
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{
    (void)handle;
    platform_panic(name, 0);
}

void kernel_main(void)
{
    platform_init();
    configASSERT(xTaskCreateStatic(probe, "power", 1024, NULL, 3, stack, &task));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
