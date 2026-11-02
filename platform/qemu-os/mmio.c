/* Register model of the virtual OS board.
 *
 * The OS and its driver adapters reach the A733's peripherals through
 * a733_mmio.h, whose plain accessors call qemu_os_mmio_read and
 * qemu_os_mmio_write here. Registers are 32-bit words in a sparse table that
 * starts from the state the board inherits from boot0, and most behave as
 * memory. The registers with behavior drivers depend on are devices:
 *
 *   the thermal sensor block (THS): readings that arrive at scripted times,
 *     update flags that clear by writing one, a calibration the efuse words
 *     describe;
 *   the S_TWI0 controller and the AXP PMIC behind it: START, address, byte
 *     and STOP events that complete after the documented bit times, the
 *     status codes the controller reports, and the PMIC registers the power
 *     provider reads and writes.
 *
 * An access to an address no device or seed names is memory too; a driver
 * that reads a register the model does not describe sees zero. */
#include "qemu_os.h"
#include "platform.h"
#include "a733_pmic.h"
#include "a733_thermal.h"
#include <stdatomic.h>
#include <string.h>

#define CCU UINT32_C(0x02002000)
#define THS UINT32_C(0x02522000)
#define TWI UINT32_C(0x07083000)
#define SID UINT32_C(0x03006244)
#define WORDS 256u

static struct { uintptr_t address; uint32_t value; } words[WORDS];
static unsigned used;
static atomic_flag guard = ATOMIC_FLAG_INIT;
static uint64_t now_us(void) { uint64_t n; __asm__ volatile("isb\n mrs %0, cntpct_el0" : "=r"(n) :: "memory"); return n / 24; }

static uint32_t *word(uintptr_t address, int create)
{
    for (unsigned i = 0; i < used; ++i) if (words[i].address == address) return &words[i].value;
    if (!create) return 0;
    if (used == WORDS) platform_panic("register table full", WORDS);
    words[used].address = address;
    return &words[used++].value;
}
static void seed(uintptr_t address, uint32_t value) { *word(address, 1) = value; }

/* ---- Thermal sensors ---------------------------------------------------- */
/* The code that the sensor reports for a temperature: the inverse of
 * a733_thermal_temperature on its branch above code 1769. */
static uint16_t code_for(int32_t millicelsius)
{
    int32_t code = 2822 - millicelsius / 62;
    return (uint16_t)(code < 1770 ? 1770 : code > 4094 ? 4094 : code);
}
static struct {
    uint32_t flags, data[QEMU_OS_SENSORS];
    int64_t last[QEMU_OS_SENSORS];
} ths = {.last = {-1, -1, -1, -1, -1}};
/* Called under the guard before any THS register is read. A sensor delivers
 * a reading once its first time has passed and one more each period. */
static void ths_advance(void)
{
    const struct qemu_os_scenario *s = qemu_os_scenario();
    uint32_t *enable = word(THS + 4, 0);
    if (!enable) return;
    const uint64_t t = now_us();
    for (unsigned i = 0; i < QEMU_OS_SENSORS; ++i) {
        if (!(*enable >> i & 1) || s->first_us[i] == UINT64_MAX || t < s->first_us[i]) continue;
        const int64_t k = s->period_us ? (int64_t)((t - s->first_us[i]) / s->period_us) : 0;
        if (k == ths.last[i]) continue;
        ths.last[i] = k;
        ths.flags |= 1u << i;
        ths.data[i] = code_for(s->temperature_mc[i] + (int32_t)k * s->slope_mc[i]);
    }
}
static int ths_read(uintptr_t a, uint32_t *v)
{
    const uintptr_t offset = a - THS;
    if (offset == 0x20) { ths_advance(); *v = ths.flags; return 1; }
    if (offset >= 0xc0 && offset < 0xc0 + 4 * QEMU_OS_SENSORS) { ths_advance(); *v = ths.data[(offset - 0xc0) / 4]; return 1; }
    return 0;
}
static int ths_write(uintptr_t a, uint32_t v)
{
    if (a - THS == 0x20) { ths.flags &= ~v; return 1; }
    return 0;
}
/* The efuse words: the factory temperature in tenths of a degree and, from
 * bit 12, the 12-bit code of each sensor at that temperature. */
static void sid_seed(void)
{
    uint32_t w[3] = {250, 0, 0};
    if (qemu_os_scenario()->fault == QEMU_OS_FAULT_THERMAL_CALIBRATION) { w[0] = w[1] = w[2] = 0xffffffffu; }
    else for (unsigned i = 0; i < QEMU_OS_SENSORS; ++i) {
        const unsigned bit = (i + 1) * 12, index = bit / 32, shift = bit % 32;
        const uint32_t code = 2419; /* 24.986 C: factory trim of zero */
        w[index] |= code << shift;
        if (shift > 20) w[index + 1] |= code >> (32 - shift);
    }
    w[2] |= 4u << 8; /* the CPU voltage bin: the dvfs_ori code of bin 4 */
    for (unsigned i = 0; i < 3; ++i) seed(SID + 4 * i, w[i]);
}

/* ---- S_TWI0 and the AXP PMIC -------------------------------------------- */
enum { TWI_DATA = 8, TWI_CNTR = 0xc, TWI_STAT = 0x10, TWI_LCR = 0x20 };
enum { WANT_NONE, WANT_ADDRESS, WANT_REGISTER, WANT_BYTE, WANT_READ };
static struct {
    uint8_t regs[256], pointer;
    uint32_t cntr, stat, data;
    unsigned want, in_transfer, reading, pending; /* pending: 1 start, 2 stop, 3 byte */
    uint64_t done_at; /* microseconds, when the pending event finishes */
} twi = {.cntr = 0x40, .stat = 0xf8};
#define BIT_NS 9231u /* the 26 MHz oscillator over the divider that boot0 leaves: 108 kHz */
static void axp_seed(void)
{
    uint8_t *r = twi.regs;
    r[0x0e] = 3; /* chip: AXP8191 */
    r[0x10] = 0x9f; r[0x11] = 0x11;   /* DCDC1-5 and 8, DCDC9 and DC1SW2 on */
    r[0x12] = 23;                     /* DCDC1, DC1SW2: 3.3 V */
    r[0x13] = r[0x14] = r[0x15] = r[0x16] = 30; /* DCDC2-5: 800 mV */
    r[0x19] = 30; r[0x1a] = 70;       /* DCDC8: 800 mV, DCDC9: 1.2 V */
    r[0x23] = 8; r[0x3f] = 24;        /* ELDO6: 1.1 V */
}
static void twi_finish(void)
{
    if (!twi.pending || now_us() < twi.done_at) return;
    switch (twi.pending) {
    case 1: twi.stat = twi.in_transfer ? 0x10 : 8; twi.in_transfer = 1; twi.want = WANT_ADDRESS; break;
    case 2: twi.stat = 0xf8; twi.in_transfer = 0; twi.want = WANT_NONE; break;
    default:
        switch (twi.want) {
        case WANT_ADDRESS:
            twi.reading = twi.data & 1;
            if ((twi.data >> 1) != 0x36) { twi.stat = twi.reading ? 0x48 : 0x20; twi.want = WANT_NONE; }
            else { twi.stat = twi.reading ? 0x40 : 0x18; twi.want = twi.reading ? WANT_READ : WANT_REGISTER; }
            break;
        case WANT_REGISTER: twi.pointer = (uint8_t)twi.data; twi.stat = 0x28; twi.want = WANT_BYTE; break;
        case WANT_BYTE: twi.regs[twi.pointer++] = (uint8_t)twi.data; twi.stat = 0x28; break;
        case WANT_READ: twi.data = twi.regs[twi.pointer++]; twi.stat = 0x58; break;
        default: twi.stat = 0x30; break;
        }
    }
    twi.pending = 0;
}
static void trace(const char *what, uintptr_t offset, uint32_t value)
{
    if (!(qemu_os_scenario()->trace & 1)) return;
    qemu_os_puts("TWI "); qemu_os_puts(what); qemu_os_puts(" "); qemu_os_hex(offset);
    qemu_os_puts(" "); qemu_os_hex(value); qemu_os_puts(" t="); qemu_os_dec(now_us()); qemu_os_puts("\n");
}
static int twi_read_value(uintptr_t a, uint32_t *v);
static int twi_read(uintptr_t a, uint32_t *v)
{
    const int handled = twi_read_value(a, v);
    if (handled) trace("r", a - TWI, *v);
    return handled;
}
static int twi_read_value(uintptr_t a, uint32_t *v)
{
    const uintptr_t offset = a - TWI;
    twi_finish();
    switch (offset) {
    case TWI_DATA: *v = twi.data; return 1;
    case TWI_STAT: *v = twi.stat; return 1;
    case TWI_LCR: /* SCL and SDA high, controller idle */
        *v = qemu_os_scenario()->fault == QEMU_OS_FAULT_PMIC_BUS ? 0x1a : 0x3a;
        return 1;
    case TWI_CNTR:
        /* Bus enable stays set; M_STP shows until the STOP finishes; the
         * interrupt flag shows when an event has finished. */
        *v = 0x40 | (twi.pending == 2 ? 0x10 : 0) | (!twi.pending && twi.stat != 0xf8 ? 8 : 0);
        return 1;
    default: return 0;
    }
}
static int twi_write(uintptr_t a, uint32_t v)
{
    const uintptr_t offset = a - TWI;
    trace("w", offset, v);
    twi_finish();
    if (offset == TWI_DATA) { twi.data = v & 0xff; return 1; }
    if (offset != TWI_CNTR) return 0;
    if (twi.pending) return 1; /* the controller ignores a command in flight */
    if (v & 0x20) { twi.pending = 1; twi.done_at = now_us() + 2 * BIT_NS / 1000; }
    else if (v & 0x10) { twi.pending = 2; twi.done_at = now_us() + 2 * BIT_NS / 1000; }
    else if (twi.in_transfer) { twi.pending = 3; twi.done_at = now_us() + 9 * BIT_NS / 1000; }
    return 1;
}

/* ---- Access ------------------------------------------------------------- */
static int device_read(uintptr_t a, uint32_t *v)
{
    if (a >= THS && a < THS + 0x100) return ths_read(a, v);
    if (a >= TWI && a < TWI + 0x400) return twi_read(a, v);
    return 0;
}
static int device_write(uintptr_t a, uint32_t v)
{
    if (a >= THS && a < THS + 0x100) return ths_write(a, v);
    if (a >= TWI && a < TWI + 0x400) return twi_write(a, v);
    return 0;
}
static void trace_all(const char *what, uintptr_t a, uint32_t v)
{
    if (!(qemu_os_scenario()->trace & 2)) return;
    qemu_os_puts("MMIO "); qemu_os_puts(what); qemu_os_puts(" "); qemu_os_hex(a);
    qemu_os_puts(" "); qemu_os_hex(v); qemu_os_puts("\n");
}
static uint32_t read_word(uintptr_t a)
{
    uint32_t v = 0;
    if (device_read(a, &v)) { trace_all("r", a, v); return v; }
    uint32_t *w = word(a, 0);
    v = w ? *w : 0;
    trace_all("r", a, v);
    return v;
}
static void write_word(uintptr_t a, uint32_t v)
{
    trace_all("w", a, v);
    if (device_write(a, v)) return;
    *word(a, 1) = v;
}
static void lock(void) { while (atomic_flag_test_and_set_explicit(&guard, memory_order_acquire)) __asm__ volatile("yield"); }
static void unlock(void) { atomic_flag_clear_explicit(&guard, memory_order_release); }
uint64_t qemu_os_mmio_read(uintptr_t address, unsigned bytes)
{
    lock();
    uint64_t v = 0;
    for (unsigned i = 0; i < bytes; i += 4) {
        const uintptr_t a = (address + i) & ~(uintptr_t)3;
        uint64_t w = read_word(a);
        if (bytes < 4) v = (w >> ((address & 3) * 8)) & (bytes == 1 ? 0xffu : 0xffffu);
        else v |= w << (8 * i);
    }
    unlock();
    return v;
}
void qemu_os_mmio_write(uintptr_t address, unsigned bytes, uint64_t value)
{
    lock();
    if (bytes >= 4) for (unsigned i = 0; i < bytes; i += 4) write_word(address + i, (uint32_t)(value >> (8 * i)));
    else {
        const unsigned shift = (address & 3) * 8;
        const uint32_t mask = (bytes == 1 ? 0xffu : 0xffffu) << shift;
        write_word(address & ~(uintptr_t)3, (read_word(address & ~(uintptr_t)3) & ~mask) | ((uint32_t)value << shift & mask));
    }
    unlock();
}

/* The state boot0 leaves, before the first access. */
void qemu_os_registers_init(void)
{
    const struct qemu_os_scenario *s = qemu_os_scenario();
    /* The oscillator is 26 MHz (the A733 boards' DCXO) and PLL_REF turns it
     * into 24 MHz: 26 * 96 / 104. */
    seed(0x07090160, 0x10000002 | 2u << 14);
    seed(CCU, 0xf8000000u | 103u << 16 | 95u << 8);
    seed(CCU + 0x518, 0);                               /* APB1 from the 24 MHz oscillator, divider 1 */
    seed(CCU + 0xfc0, s->fault == QEMU_OS_FAULT_THERMAL_CLOCK ? 0 : 0x80000000u); /* sensor ADC clock */
    sid_seed();
    seed(0x0701019c, 0x10001);                          /* S_TWI0 gate and reset open */
    seed(0x07025000, 0x22);                             /* PL0 and PL1 in function 2 */
    seed(0x07025014, QEMU_OS_PL_DRIVE | QEMU_OS_PL_DRIVE << 4); /* 4 bits per pin */
    seed(0x07025024, QEMU_OS_PL_PULL | QEMU_OS_PL_PULL << 2);  /* 2 bits per pin */
    seed(TWI + 0x14, 0x13);                             /* divider: 26 MHz / 240 */
    /* CPU-L, CPU-B and DSU run from their PLLs, as boot0 leaves them:
     * 1014 MHz, 1014 MHz and 780 MHz from the 26 MHz oscillator. */
    seed(0x08871000, 0xf8000000u | 39u << 8); seed(0x0887101c, 3u << 24);
    seed(0x08872000, 0xf8000000u | 39u << 8); seed(0x0887201c, 3u << 24);
    seed(0x08873000, 0xf8000000u | 30u << 8); seed(0x0887301c, 3u << 24);
    seed(TWI + 0x210, 0xc0);
    axp_seed();
}
