/* Controller-only UFS bring-up. PHY remains reset; no storage requests or DMA. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_i2c.h"

#define CCU 0x02002000UL
#define RTC 0x07090000UL
#define UFS 0x04520000UL
#define BIT(n) (1U << (n))

static StaticTask_t task;
static StackType_t stack[1024];

static void report(const char *name, uint64_t value)
{
    platform_puts(name); platform_hex(value); platform_puts("\n");
}

static void delay_us(unsigned us)
{
    uint64_t end = counter() + COUNTER_HZ / 1000000 * us;
    while (counter() < end) __asm__ volatile("yield");
}

static int wait_bits(uintptr_t address, uint32_t mask, uint32_t value)
{
    uint64_t end = counter() + COUNTER_HZ / 20;
    while ((mmio_read(address) & mask) != value) {
        if (counter() >= end) {
            report("timeout register=", address);
            report("value=", mmio_read(address));
            return 0;
        }
    }
    return 1;
}

static int get_attribute(unsigned attribute)
{
    if (!wait_bits(UFS + 0x30, 8, 8)) return 0;
    mmio_write(UFS + 0x20, BIT(10));
    mmio_write(UFS + 0x94, attribute << 16);
    mmio_write(UFS + 0x98, 0);
    mmio_write(UFS + 0x9c, 0);
    mmio_write(UFS + 0x90, 1); // DME_GET, local link-management attribute.
    if (!wait_bits(UFS + 0x20, BIT(10), BIT(10))) return 0;
    report("local UFS attribute=", attribute);
    report("UIC result=", mmio_read(UFS + 0x98));
    report("value=", mmio_read(UFS + 0x9c));
    return !(mmio_read(UFS + 0x98) & 0xff);
}

static int power_ready(void)
{
    struct a733_i2c bus;
    enum i2c_result result = a733_i2c_open(&bus);
    if (result) return 0;
    const uint8_t registers[] = {0xff, 0x0e, 0x10, 0x19, 0x22, 0x39};
    const uint8_t masks[] = {0xff, 0xff, 0x80, 0x7f, 0x20, 0x1f};
    const uint8_t expected[] = {0, 3, 0x80, 100, 0x20, 20};
    for (unsigned i = 0; i < sizeof(registers); ++i) {
        uint8_t value;
        result = a733_i2c_read8(&bus, 0x36, registers[i], &value);
        if (result || (value & masks[i]) != expected[i]) {
            a733_i2c_close(&bus);
            return 0;
        }
    }
    a733_i2c_close(&bus);
    return 1;
}

static void probe(void *unused)
{
    (void)unused;
    configASSERT(power_ready());
    platform_puts("UFS assigned rails enabled: DCDC8 1.8V, DLDO6 2.5V\n");
    uint32_t pll = mmio_read(CCU + 0xa0);
    report("peripheral PLL0=", pll);
    // Use inherited PLL outputs. Do not reprogram a shared PLL for this probe.
    configASSERT((pll & 0x9e000000U) == 0x9e000000U);
    configASSERT(mmio_read(CCU + 0x5c0) & BIT(24));
    const uintptr_t addresses[] = {CCU + 0xd80, CCU + 0xd84, CCU + 0xd8c,
                                   RTC + 0x15c, RTC + 0x160, RTC + 0x16c};
    uint32_t saved[6];
    for (unsigned i = 0; i < 6; ++i) {
        saved[i] = mmio_read(addresses[i]);
        report("saved register=", addresses[i]); report("value=", saved[i]);
    }
    // This initial bring-up supports the observed 26 MHz internal reference.
    configASSERT(((saved[4] >> 14) & 3) == 2);
    configASSERT(!(saved[0] & BIT(31)));
    // ROM leaves the configuration clock enabled at the desired divisor even
    // with every UFS reset asserted. Preserve that valid inherited setting.
    configASSERT(!(saved[1] & BIT(31)) || saved[1] == (BIT(31) | 24));
    configASSERT((saved[2] & 0xf0001) == 0);
    mmio_write(CCU + 0xd80, BIT(24)); // PLL peripheral 200 MHz, divisor one.
    delay_us(10);
    mmio_write(CCU + 0xd8c, saved[2] | BIT(17));
    mmio_write(CCU + 0xd80, BIT(31) | BIT(24));
    mmio_write(CCU + 0xd8c, saved[2] | BIT(17) | BIT(16));
    mmio_write(CCU + 0xd8c, saved[2] | BIT(17) | BIT(16) | 1);
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t cfg = mmio_read(UFS + 0x200), gates = mmio_read(UFS + 0x204);
    report("UFS initial configuration=", cfg);
    report("UFS initial gates=", gates);
    // SRAM initialization is reserved for the later PHY startup step.
    mmio_write(UFS + 0x200, cfg & ~(BIT(20) | BIT(19)));
    if (!(saved[1] & BIT(31))) {
        mmio_write(CCU + 0xd84, 24); // Peripheral 480 MHz / 25 = 19.2 MHz.
        delay_us(10);
        mmio_write(CCU + 0xd84, BIT(31) | 24);
        delay_us(10);
    }
    mmio_write(RTC + 0x15c, (saved[3] & ~0xffffU) | 0x16aa);
    mmio_write(RTC + 0x16c, saved[5] | 1);
    mmio_write(RTC + 0x160, saved[4] & ~BIT(31));
    uint32_t next = mmio_read(UFS + 0x200);
    next &= ~(0x3f00U | BIT(5) | BIT(4) | 3U);
    next |= 0x1300U | BIT(3) | 1U;
    mmio_write(UFS + 0x200, next);
    mmio_write(UFS + 0x204, (gates & ~(BIT(16) | BIT(17))) | 0x800003ffU);
    mmio_write(CCU + 0xd8c, saved[2] | BIT(19) | BIT(17) | BIT(16) | 1);
    // HCI registers belong to the controller core. The reference implementation
    // reads them only after releasing core reset; an early read may stall AHB.
    report("UFS capability=", mmio_read(UFS));
    report("UFS HCI version=", mmio_read(UFS + 8));
    mmio_write(UFS + 0x24, 0); // Polling; no controller IRQs.
    mmio_write(UFS + 0x34, 1);
    delay_us(1000);
    int passed = wait_bits(UFS + 0x34, 1, 1) && wait_bits(UFS + 0x30, 8, 8);
    report("UFS status=", mmio_read(UFS + 0x30));
    if (passed) passed = get_attribute(0x15a9) && get_attribute(0x1520) && get_attribute(0x1540);
    mmio_write(UFS + 0x34, 0);
    int disabled = wait_bits(UFS + 0x34, 1, 0);
    mmio_write(UFS + 0x200, cfg);
    mmio_write(UFS + 0x204, gates);
    mmio_write(CCU + 0xd8c, saved[2] | BIT(17) | BIT(16) | 1);
    // Restore RTC settings while their write-unlock key is still active.
    mmio_write(RTC + 0x160, saved[4]);
    mmio_write(RTC + 0x16c, saved[5]);
    mmio_write(RTC + 0x15c, saved[3]);
    mmio_write(CCU + 0xd84, saved[1]);
    mmio_write(CCU + 0xd80, saved[0]);
    mmio_write(CCU + 0xd8c, saved[2]);
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i = 0; i < 6; ++i) {
        // The RTC unlock register is a key, not ordinary readable state.
        if (i != 3) configASSERT(mmio_read(addresses[i]) == saved[i]);
    }
    configASSERT(disabled && passed);
    platform_puts("PASS UFS controller and local attributes; PHY and storage untested\n");
    platform_exit(0);
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{
    (void)handle;
    platform_panic(name, 0);
}

void kernel_main(void)
{
    platform_init();
    configASSERT(xTaskCreateStatic(probe, "ufs", 1024, NULL, 3, stack, &task));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
