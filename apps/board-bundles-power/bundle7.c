/* SPDX-License-Identifier: Apache-2.0 */
/* Bundle 7 of docs/designs/board-bundles-next.md: domain 1 ON through the
 * power-domain owner, the first PPU write.
 *
 * After provider readiness and supply verification, the client takes a
 * shared DCDC2 token, reads R_CCU 0x1ac and, while the PPU clock reads open,
 * the delay row (+0x170, +0x174, +0xc00, +0xc04, +0xc10), policy and status
 * of domain 1. It then requests ON through the domain-1 row and polls the
 * row once per tick. The owner admits the row, writes and reads back each
 * delay-row word and writes the ON command; the recording layer then reads
 * the status back to back with CNTPCT stamps until it reads ON or 10 ms
 * pass. Afterwards the client reads the delay row, policy and status again.
 * Domain 1 stays ON until power removal.
 *
 * Outcomes: status 0 with "COMPLETE board bundle 7: domain 1 ON"; status 0
 * with "COMPLETE board bundle 7: inconclusive" when the owner refused the row
 * before any PPU write (a gate, the supply record or adoption); status 1 with
 * "FAILED board bundle 7: domain 1", the owner's record and the status trace
 * ending in "TRACE END board bundle 7", the bundle's controlled outcome. */
#include "power.h"
#include "platform.h"
#include "boot.h"
#include "a733_power_authority.h"

#define DOMAIN1 UINT32_C(0x07061000)
#define ROW_WAIT_US 200000u
static const uint16_t words[] = {0x000, 0x008, 0x170, 0x174, 0xc00, 0xc04, 0xc10};
#define WORDS (sizeof words / sizeof words[0])

static unsigned read_domain(uint32_t *out)
{
    uint32_t ppu;
    if (!bb_power_read(0x070101ac, &ppu))
        bb_power_fail("board bundle 7: PPU clock read");
    if (!(ppu & 1))
        return 0;
    for (unsigned i = 0; i < WORDS; ++i)
        if (!bb_power_read(DOMAIN1 + words[i], &out[i]))
            bb_power_fail("board bundle 7: domain 1 read");
    return 1;
}
static void put_words(const char *name, const uint32_t *v)
{
    for (unsigned i = 0; i < WORDS; ++i) {
        platform_puts(name);
        bb_power_put_hex(" offset=", words[i]);
        bb_power_put_hex("  word=", v[i]);
    }
}
static void report(unsigned before_read, const uint32_t *before)
{
    const struct bb_status_trace *t = &bb_power.status;
    bb_power_put_hex("POWER_STATE_WORD=", a733_power_authority_load(&bb_power.rails.authority));
    bb_power_put("DOMAIN1_OWNER_STATE=", bb_power.domains.domain[0].state);
    if (before_read)
        put_words("BEFORE", before);
    bb_power_report_accesses();
    bb_power_put("STATUS reads=", t->reads);
    bb_power_put("STATUS changes=", t->changes);
    for (unsigned i = 0; i < t->changes && i < BB_STATUS_CHANGES; ++i) {
        bb_power_put_hex("STATUS value=", t->change[i].value);
        bb_power_put("  cntpct=", t->change[i].at);
    }
    bb_power_put("STATUS on=", t->on);
    bb_power_put("STATUS on_cntpct=", t->on_at);
    bb_power_put("STATUS bounded=", t->bounded);
    bb_power_put("STATUS last_cntpct=", t->last_at);
}

static void client(void *unused)
{
    (void)unused;
    uint32_t before[WORDS] = {0}, after[WORDS] = {0};
    platform_puts("BEGIN board bundle 7: domain 1 ON through the owner\n");
    bb_power_wait_ready();
    bb_power_acquire(A733_RAIL_BIT(A733_RAIL_DCDC2));
    const unsigned before_read = read_domain(before);
    const unsigned owner_first = bb_power.accessed;
    bb_power.spin_after_on = 1;
    if (!a733_power_domain_request(&bb_power.row, 1))
        bb_power_fail("board bundle 7: row request");
    const uint64_t until = bb_power_now() + ROW_WAIT_US;
    enum a733_power_row_state state;
    while ((state = a733_power_domain_state(&bb_power.row)) == A733_POWER_ROW_PENDING) {
        if (bb_power_now() >= until)
            bb_power_fail("board bundle 7: row answer deadline");
        vTaskDelay(1);
    }
    bb_power_put("ROW_STATE=", state);
    bb_power_put("OWNER first access=", owner_first);
    if (state == A733_POWER_ROW_FAILED) {
        platform_puts("FAILED board bundle 7: domain 1\n");
        report(before_read, before);
        platform_puts("TRACE END board bundle 7\n");
        platform_exit(1);
    }
    if (state == A733_POWER_ROW_REFUSED) {
        if (bb_power.writes)
            bb_power_fail("board bundle 7: refused after a PPU write");
        bb_power_park();
        report(before_read, before);
        platform_puts("COMPLETE board bundle 7: inconclusive, the owner refused domain 1 before any PPU write\n");
        platform_exit(0);
    }
    if (state != A733_POWER_ROW_ON)
        bb_power_fail("board bundle 7: row state");
    const unsigned after_read = read_domain(after);
    bb_power_park();
    report(before_read, before);
    if (after_read)
        put_words("AFTER", after);
    platform_puts("COMPLETE board bundle 7: domain 1 ON\n");
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
    const uint64_t start = counter() / 24;
    if (!bb_power_init(start + 1800000, client, "bundle7"))
        platform_panic("board bundle 7: power initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
