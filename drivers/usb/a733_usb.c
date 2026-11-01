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

static void hold_us(uint32_t microseconds)
{
    /* This legacy target's monitor establishes the 24 MHz counter contract.
     * One extra raw tick covers the fraction before the first sample. */
    const uint64_t ticks=(COUNTER_HZ/1000000u)*(uint64_t)microseconds+1;
    const uint64_t start=counter();
    if(start>UINT64_MAX-ticks)platform_panic("USB hold overflow",__LINE__);
    const uint64_t target=start+ticks;
    if(microseconds>=1000)vTaskDelay(pdMS_TO_TICKS(microseconds/1000));
    uint64_t previous=start;
    for(unsigned reads=0;reads<1000000;++reads){
        uint64_t now=counter();
        if(now<previous)platform_panic("USB counter reversed",__LINE__);
        if(now>=target)return;
        previous=now;
    }
    platform_panic("USB counter stopped",__LINE__);
}

void a733_usb_prepare(void)
{
    // Complete the caller's SoftConn clear before the 150 ms detach hold.
    a733_mmio_dsb_sy();
    hold_us(150000);
    // Boot ROM supplies the parent reference and interconnect clocks. Reset
    // only this device controller, preserving the other USB controllers.
    a733_mmio_write32_dsb_sy_after(CCU + 0x1300, mmio_read(CCU + 0x1300) | (1U << 30));
    uint32_t gate = mmio_read(CCU + 0x1304) | (1U << 8);
    a733_mmio_write32_dsb_sy_after(CCU + 0x1304, gate & ~(1U << 24));
    /* Device reset is asserted: hold at least 1 ms after write completion. */
    hold_us(1000);
    a733_mmio_write32_dsb_sy_after(CCU + 0x1304, gate | (1U << 24));
    /* Reset is released: allow 40 us for device gate/reset propagation. */
    hold_us(40);
    a733_mmio_write32_dsb_sy_after(USB + 0x420, mmio_read(USB + 0x420) | 1);
    a733_mmio_write32_dsb_sy_after(USB + 0x410, (mmio_read(USB + 0x410) & ~(1U << 3)) | (1U << 5));
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
