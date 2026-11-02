/* Ownership, refusal and restoration of the real watchdog driver (drivers/watchdog/a733_watchdog.c), linked
 * unchanged against the strict register model (checks/host/strict_mmio.h). Hardware reset is not modeled.
 *
 * Every modeled behavior cites its source. The model refuses an access to any offset not listed here, any access
 * of another width, a restart write other than the reload word, and a data write without the upper key. The
 * initial register values are scenario inputs (the state another owner or an earlier stage may have left), not
 * claims about the hardware. Dropped writes are fault injection: they exercise the driver's verification. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "a733_watchdog.h"
#include "strict_mmio.h"

#define WDT 0x02050000UL
#define FEL_FLAG 0x07090108UL
#define CITE_MAP "docs/watchdog.md:28-38 (WDT0 register map, write order and readback of the lower words)"
#define CITE_KEY "~/.cache/sunstoneos/sources/board-kernel/sunxi_wdt.c:234 (wdt_filed_magic 0x16AA0000 is written with the data)"
#define CITE_RELOAD "sunxi_wdt.c:31 WDT_CTRL_RELOAD = 1 | (0x0a57 << 1) = 0x14af; docs/watchdog.md:36"

struct wdt_state { int drop; };
static struct wdt_state drop_restart, drop_cfg, drop_mode, drop_out, drop_flag;
static int enabled;

static int write_restart(struct strict_reg *reg, uint64_t value)
{
    if (value != 0x14af) return 1;       /* only the reload word is modeled */
    if (((struct wdt_state *)reg->context)->drop) return 0;
    return 0;
}

static int write_keyed(struct strict_reg *reg, uint64_t value)
{
    if ((value >> 16) != 0x16aa) return 1; /* the effect of a write without the key is not modeled */
    if (((struct wdt_state *)reg->context)->drop) return 0;
    if (reg->address == WDT + 0x14 && (value & 1)) enabled = 1;
    reg->value = value & 0xffff;
    return 0;
}

static int write_flag(struct strict_reg *reg, uint64_t value)
{
    if (!((struct wdt_state *)reg->context)->drop) reg->value = value;
    return 0;
}

static struct strict_reg regs[] = {
    {WDT + 0x0c, 4, "WDT0 restart", CITE_RELOAD, 0, NULL, write_restart, &drop_restart},
    {WDT + 0x10, 4, "WDT0 configuration", CITE_MAP "; " CITE_KEY, 0, NULL, write_keyed, &drop_cfg},
    {WDT + 0x14, 4, "WDT0 mode", CITE_MAP "; " CITE_KEY, 0, NULL, write_keyed, &drop_mode},
    {WDT + 0x18, 4, "WDT0 reset output", CITE_MAP "; " CITE_KEY, 0, NULL, write_keyed, &drop_out},
    {FEL_FLAG, 4, "RTC GP_DATA_REG2 (FEL request flag)", "docs/vendor-firmware/scp-firmware.md:437-439 (general-purpose data register)",
     0, NULL, write_flag, &drop_flag},
};
#define R(address) (regs[(address) == WDT + 0x0c ? 0 : (address) == WDT + 0x10 ? 1 : (address) == WDT + 0x14 ? 2 : (address) == WDT + 0x18 ? 3 : 4])
#define CONFIG R(WDT + 0x10).value
#define MODE R(WDT + 0x14).value
#define OUTPUT R(WDT + 0x18).value
#define FLAG R(FEL_FLAG).value

static unsigned total_writes(void) { unsigned n = 0; for (unsigned i = 0; i < sizeof regs / sizeof *regs; ++i) n += regs[i].writes; return n; }
static unsigned total_reads(void) { unsigned n = 0; for (unsigned i = 0; i < sizeof regs / sizeof *regs; ++i) n += regs[i].reads; return n; }

static void reset_fixture(void)
{
    strict_mmio_define(regs, sizeof regs / sizeof *regs);
    for (unsigned i = 0; i < sizeof regs / sizeof *regs; ++i) regs[i].reads = regs[i].writes = 0;
    drop_restart.drop = drop_cfg.drop = drop_mode.drop = drop_out.drop = drop_flag.drop = 0;
    CONFIG = 0x22; MODE = 0xa2; OUTPUT = 7; FLAG = 0x12345678; R(WDT + 0x0c).value = 0;
    enabled = 0;
}

static char last_message[200];
static void remember(const char *message) { snprintf(last_message, sizeof last_message, "%s", message); }

int main(void)
{
    struct a733_watchdog watchdog = {0};
    reset_fixture();
    assert(!a733_watchdog_arm(&watchdog, 7));
    assert(!total_reads() && !total_writes());
    MODE |= 1;
    assert(!a733_watchdog_arm(&watchdog, 6));
    assert(!total_writes()); /* Preserve another owner's active reset policy. */
    reset_fixture();
    assert(a733_watchdog_arm(&watchdog, 6));
    assert(watchdog.armed && FLAG == 0x5aa5a55a);
    assert((MODE & 0xf1) == 0x61);
    unsigned prior_writes = total_writes();
    assert(!a733_watchdog_arm(&watchdog, 2));
    assert(total_writes() == prior_writes);
    a733_watchdog_feed(&watchdog);
    assert(total_writes() == prior_writes + 1);
    assert(a733_watchdog_disarm(&watchdog));
    assert(MODE == 0xa2 && CONFIG == 0x22 && OUTPUT == 7);
    assert(FLAG == 0x12345678 && !watchdog.armed);
    prior_writes = total_writes();
    a733_watchdog_feed(&watchdog);
    assert(total_writes() == prior_writes);
    reset_fixture();
    drop_flag.drop = 1;
    assert(!a733_watchdog_arm(&watchdog, 6));
    assert(MODE == 0xa2 && !watchdog.armed);
    reset_fixture();
    drop_cfg.drop = 1;
    assert(!a733_watchdog_arm(&watchdog, 6));
    assert(!enabled); /* Do not start a timer with an unverified reset policy. */
    assert(MODE == 0xa2 && OUTPUT == 7 && FLAG == 0x12345678);
    assert(!watchdog.armed);
    reset_fixture();
    drop_out.drop = 1;
    assert(!a733_watchdog_arm(&watchdog, 6));
    assert(!enabled && !watchdog.armed);
    assert(MODE == 0xa2 && CONFIG == 0x22 && FLAG == 0x12345678);
    reset_fixture();
    assert(a733_watchdog_arm(&watchdog, 6));
    drop_mode.drop = 1;
    assert(!a733_watchdog_disarm(&watchdog));
    assert(watchdog.armed && FLAG == 0x5aa5a55a);

    /* The model refuses what it does not define. */
    strict_mmio_set_refusal_handler(remember);
    reset_fixture();
    strict_mmio_set_refusal_handler(remember);
    (void)strict_mmio_read(WDT + 0x1c, 4);
    assert(strict_mmio_refusals() == 1 && last_message[0] == 'r');
    strict_mmio_write(WDT + 0x14, 2, 0x16aa0001);       /* a modeled offset at another width */
    assert(strict_mmio_refusals() == 2);
    strict_mmio_write(WDT + 0x14, 4, 0x00000001);       /* no key */
    assert(strict_mmio_refusals() == 3 && MODE == 0xa2);
    strict_mmio_write(WDT + 0x0c, 4, 0x1234);           /* not the reload word */
    assert(strict_mmio_refusals() == 4);
    strict_mmio_clear();
    puts("PASS watchdog ownership, refusal, register restoration, failed-disarm retention and strict-model refusals; hardware reset untested.");
    return 0;
}
