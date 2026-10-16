/* Host register model: refusal and restoration checks, not reset qualification. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SUNSTONE_A733_BOOT_H
static uint32_t registers[7], rtc_flag;
static uintptr_t ignore_write;
static unsigned writes, reads;
static int enabled;

static uint32_t mmio_read(uintptr_t address)
{
    ++reads;
    if (address == 0x07090108) return rtc_flag;
    assert(address >= 0x02050000 && address <= 0x02050018 && !(address & 3));
    return registers[(address - 0x02050000) / 4];
}

static void mmio_write(uintptr_t address, uint32_t value)
{
    ++writes;
    if (address == ignore_write) return;
    if (address == 0x07090108) { rtc_flag = value; return; }
    assert(address >= 0x0205000c && address <= 0x02050018 && !(address & 3));
    if (address == 0x0205000c) {
        assert(value == 0x14af);
        return;
    }
    assert((value >> 16) == 0x16aa);
    if (address == 0x02050014 && (value & 1)) enabled = 1;
    registers[(address - 0x02050000) / 4] = value & 0xffff;
}

#include "../drivers/watchdog/a733_watchdog.c"

static void reset_fixture(void)
{
    memset(registers, 0, sizeof(registers));
    registers[5] = 0xa2;
    registers[4] = 0x22;
    registers[6] = 7;
    rtc_flag = 0x12345678;
    writes = reads = 0;
    enabled = 0;
    ignore_write = 0;
}

int main(void)
{
    struct a733_watchdog watchdog = {0};
    reset_fixture();
    assert(!a733_watchdog_arm(&watchdog, 7));
    assert(!reads && !writes);
    registers[5] |= 1;
    assert(!a733_watchdog_arm(&watchdog, 6));
    assert(!writes); // Preserve another owner's active reset policy.
    reset_fixture();
    assert(a733_watchdog_arm(&watchdog, 6));
    assert(watchdog.armed && rtc_flag == 0x5aa5a55a);
    assert((registers[5] & 0xf1) == 0x61);
    unsigned prior_writes = writes;
    assert(!a733_watchdog_arm(&watchdog, 2));
    assert(writes == prior_writes);
    a733_watchdog_feed(&watchdog);
    assert(writes == prior_writes + 1);
    assert(a733_watchdog_disarm(&watchdog));
    assert(registers[5] == 0xa2 && registers[4] == 0x22 && registers[6] == 7);
    assert(rtc_flag == 0x12345678 && !watchdog.armed);
    prior_writes = writes;
    a733_watchdog_feed(&watchdog);
    assert(writes == prior_writes);
    reset_fixture();
    ignore_write = FEL_FLAG;
    assert(!a733_watchdog_arm(&watchdog, 6));
    assert(registers[5] == 0xa2 && !watchdog.armed);
    reset_fixture();
    ignore_write = WDT + 0x10;
    assert(!a733_watchdog_arm(&watchdog, 6));
    assert(!enabled); // Do not start a timer with an unverified reset policy.
    assert(registers[5] == 0xa2 && registers[6] == 7 && rtc_flag == 0x12345678);
    assert(!watchdog.armed);
    reset_fixture();
    ignore_write = WDT + 0x18;
    assert(!a733_watchdog_arm(&watchdog, 6));
    assert(!enabled && !watchdog.armed);
    assert(registers[5] == 0xa2 && registers[4] == 0x22 && rtc_flag == 0x12345678);
    reset_fixture();
    assert(a733_watchdog_arm(&watchdog, 6));
    ignore_write = WDT + 0x14;
    assert(!a733_watchdog_disarm(&watchdog));
    assert(watchdog.armed && rtc_flag == 0x5aa5a55a);
    puts("PASS watchdog ownership, refusal, register restoration, and failed-disarm retention; hardware reset untested.");
}
