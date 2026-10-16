/* Bounded, RAM-only eMMC identification. No media programming commands. */
#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"

#define CCU 0x02002000UL
#define PIOC 0x02000180UL
#define MMC 0x04022000UL
#define START (1U << 31)
#define ERRORS 0xbfc2U

static StaticTask_t task;
static StackType_t stack[1024];

static void report(const char *name, uint64_t value)
{
    platform_puts(name); platform_hex(value); platform_puts("\n");
}

static int clear_before(uintptr_t address, uint32_t mask, unsigned ms)
{
    uint64_t end = counter() + COUNTER_HZ * ms / 1000;
    while (mmio_read(address) & mask)
        if (counter() >= end) return 0;
    return 1;
}

static void delay_ms(unsigned ms)
{
    uint64_t end = counter() + COUNTER_HZ * ms / 1000;
    while (counter() < end) __asm__ volatile("yield");
}

static int command(unsigned index, uint32_t argument, uint32_t flags)
{
    mmio_write(MMC + 0x38, UINT32_MAX);
    mmio_write(MMC + 0x1c, argument);
    mmio_write(MMC + 0x18, START | flags | index);
    uint64_t end = counter() + COUNTER_HZ / 10;
    uint32_t status;
    do {
        status = mmio_read(MMC + 0x38);
        if ((status & ERRORS) || counter() >= end) {
            report("command failed index=", index);
            report("raw interrupt=", status);
            report("command register=", mmio_read(MMC + 0x18));
            report("controller status=", mmio_read(MMC + 0x3c));
            return 0;
        }
    } while (!(status & 4));
    return 1;
}

static int update_clock(void)
{
    mmio_write(MMC + 0x18, START | (1U << 21) | (1U << 13));
    return clear_before(MMC + 0x18, START, 20);
}

static void probe(void *unused)
{
    (void)unused;
    const unsigned offsets[] = {0x59c, 0x5c0, 0x5e0, 0xd00, 0xd0c,
                                0xd20, 0xd2c, 0xd30, 0xd3c, 0xd80};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        report("CCU offset=", offsets[i]);
        report("value=", mmio_read(CCU + offsets[i]));
    }
    report("PIO version=", mmio_read(0x02000078));
    report("PIO voltage mode=", mmio_read(0x02000040));
    report("PIO voltage value=", mmio_read(0x02000048));
    report("PF data (card detect bit 6)=", mmio_read(0x02000390));
    uint32_t cfg[3], pull[2];
    for (unsigned i = 0; i < 3; ++i) {
        cfg[i] = mmio_read(PIOC + 4 * i);
        report("PC configuration=", cfg[i]);
    }
    for (unsigned i = 0; i < 2; ++i) {
        pull[i] = mmio_read(PIOC + 0x30 + 4 * i);
        report("PC pull=", pull[i]);
    }
    // Shared bus gates are inherited. Do not alter their keyed registers until
    // their current state and dependencies have been established on this board.
    if (!(mmio_read(CCU + 0x5c0) & (1U << 24))) {
        platform_puts("INCOMPLETE storage AHB gate is disabled\n");
        platform_puts("PASS storage probe completed\n");
        platform_exit(0);
    }
    uint32_t saved_clock = mmio_read(CCU + 0xd20);
    uint32_t saved_bus = mmio_read(CCU + 0xd2c);
    // sys24M / (32 * 2); the controller further divides by two in SDR mode.
    // This remains below the identification limit with either 24 or 26 MHz.
    mmio_write(CCU + 0xd20, START | (1U << 8) | 31);
    mmio_write(CCU + 0xd2c, saved_bus | 0x10001);
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t version = mmio_read(MMC + 0x300);
    report("MMC version=", version);
    mmio_write(MMC, 7);
    if (!clear_before(MMC, 7, 20)) {
        platform_puts("INCOMPLETE controller reset timeout\n");
        goto restore;
    }
    const unsigned pins[] = {0, 1, 5, 6, 8, 9, 10, 11, 13, 14, 15, 16};
    for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        unsigned pin = pins[i], shift = (pin % 8) * 4;
        uintptr_t address = PIOC + (pin / 8) * 4;
        mmio_write(address, (mmio_read(address) & ~(15U << shift)) | (3U << shift));
        shift = (pin % 16) * 2;
        address = PIOC + 0x30 + (pin / 16) * 4;
        mmio_write(address, (mmio_read(address) & ~(3U << shift)) |
                   ((pin == 0 ? 2U : 1U) << shift));
    }
    mmio_write(MMC + 0x30, 0); // Polling only.
    mmio_write(MMC + 0x78, 1);
    mmio_write(MMC + 0x78, 0);
    delay_ms(1);
    mmio_write(MMC + 0x78, 1);
    delay_ms(1);
    mmio_write(MMC + 0x08, UINT32_MAX);
    mmio_write(MMC + 0x0c, 0); // Default one-bit bus.
    // Recent controllers reset with a different clock mode selected. Match
    // the timing-mode-4 initialization for controller versions >= 5.3.0.
    report("MMC initial timing=", mmio_read(MMC + 0x5c));
    if ((version & 0xffffff) >= 0x50300)
        mmio_write(MMC + 0x5c, mmio_read(MMC + 0x5c) & ~START);
    mmio_write(MMC + 0x104, mmio_read(MMC + 0x104) & ~1U);
    mmio_write(MMC + 0x140, (mmio_read(MMC + 0x140) & ~(3U << 16)) | (3U << 16));
    mmio_write(MMC + 0x144, 1U << 7); // Zero input delay, delay control enabled.
    mmio_write(MMC + 0x04, 1U << 16);
    if (!update_clock()) {
        platform_puts("INCOMPLETE clock update timeout\n");
        goto restore;
    }
    delay_ms(1);
    if (!command(0, 0, 1U << 15)) goto restore;
    delay_ms(1);
    uint64_t deadline = counter() + COUNTER_HZ * 3 / 4;
    uint32_t ocr = 0;
    do {
        if (!command(1, 0x40ff8000, 1U << 6)) goto restore;
        ocr = mmio_read(MMC + 0x20);
        if (ocr & START) break;
        delay_ms(1);
    } while (counter() < deadline);
    report("eMMC OCR=", ocr);
    if (!(ocr & START)) goto restore;
    if (!command(2, 0, (1U << 6) | (1U << 7) | (1U << 8))) goto restore;
    for (unsigned i = 0; i < 4; ++i)
        report("eMMC CID response word=", mmio_read(MMC + 0x20 + i * 4));
    platform_puts("IDENTIFIED eMMC responded to CMD1 and CMD2\n");
restore:
    mmio_write(MMC + 0x04, 0);
    (void)update_clock();
    mmio_write(MMC, 7);
    (void)clear_before(MMC, 7, 20);
    for (unsigned i = 0; i < 3; ++i) mmio_write(PIOC + i * 4, cfg[i]);
    for (unsigned i = 0; i < 2; ++i) mmio_write(PIOC + 0x30 + i * 4, pull[i]);
    mmio_write(CCU + 0xd20, saved_clock);
    mmio_write(CCU + 0xd2c, saved_bus);
    __asm__ volatile("dsb sy" ::: "memory");
    configASSERT(mmio_read(CCU + 0xd20) == saved_clock);
    configASSERT(mmio_read(CCU + 0xd2c) == saved_bus);
    for (unsigned i = 0; i < 3; ++i)
        configASSERT(mmio_read(PIOC + i * 4) == cfg[i]);
    for (unsigned i = 0; i < 2; ++i)
        configASSERT(mmio_read(PIOC + 0x30 + i * 4) == pull[i]);
    platform_puts("PASS storage probe completed\n");
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
    configASSERT(xTaskCreateStatic(probe, "storage", 1024, NULL, 3, stack, &task));
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
