#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_usb.h"
#include "a733_mmio.h"
#include "device/dcd.h"

#define USB 0x04100000UL
#define CCU 0x02002000UL
static bool poll_enabled;

uint32_t tusb_time_millis_api(void)
{
    return (uint32_t)(counter() / (COUNTER_HZ / 1000));
}

void tusb_time_delay_ms_api(uint32_t milliseconds)
{
    vTaskDelay(pdMS_TO_TICKS(milliseconds));
}

void a733_usb_prepare(void)
{
    // The caller has removed soft-connect. Let the host observe the detach.
    vTaskDelay(pdMS_TO_TICKS(150));
    // Boot ROM supplies the parent reference and interconnect clocks. Reset
    // only this device controller, preserving the other USB controllers.
    mmio_write(CCU + 0x1300, mmio_read(CCU + 0x1300) | (1U << 30));
    uint32_t gate = mmio_read(CCU + 0x1304) | (1U << 8);
    mmio_write(CCU + 0x1304, gate & ~(1U << 24));
    a733_mmio_dsb_sy();
    vTaskDelay(pdMS_TO_TICKS(1));
    mmio_write(CCU + 0x1304, gate | (1U << 24));
    mmio_write(USB + 0x420, mmio_read(USB + 0x420) | 1);
    mmio_write(USB + 0x410, (mmio_read(USB + 0x410) & ~(1U << 3)) | (1U << 5));
    a733_mmio_dsb_sy();
    platform_puts("USB controller prepared\n");
}

void a733_usb_poll_enable(bool enable)
{
    poll_enabled = enable;
}

void a733_usb_poll(void)
{
    if (poll_enabled) {
        volatile uint32_t *stats = (volatile uint32_t *)(RESULT_BASE + 128);
        uint8_t pending = a733_mmio_read8(USB + 0x4c);
        ++stats[0];
        if (pending & 4) ++stats[1];
        stats[2] = pending;
        stats[3] = a733_mmio_read16(USB + 0x82);
        dcd_int_handler(0);
    }
}

void a733_usb_trace(uint32_t event, uint32_t first, uint32_t second)
{
    volatile uint32_t *count = (volatile uint32_t *)(RESULT_BASE + 144);
    if (*count >= 256) return;
    volatile uint32_t *entry = (volatile uint32_t *)(RESULT_BASE + 16384 + *count * 16);
    entry[0] = tusb_time_millis_api();
    entry[1] = event;
    entry[2] = first;
    entry[3] = second;
    ++*count;
}

void a733_usb_assert(void)
{
    uintptr_t address = (uintptr_t)__builtin_return_address(0);
    platform_panic("TinyUSB assertion address", (unsigned)address);
}
