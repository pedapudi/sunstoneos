/* SPDX-License-Identifier: Apache-2.0 */
/* Bundle 7 of docs/designs/board-bundles-next.md: domain 1 ON through the
 * power-domain owner, the first PPU write. Domain 1 stays ON until power
 * removal, so the bundle gives one sample per board lifetime.
 *
 * After provider readiness and supply verification, the client takes a
 * shared DCDC2 token, reads R_CCU 0x1ac and, while the PPU clock reads open,
 * the delay row (+0x170, +0x174, +0xc00, +0xc04, +0xc10), policy and status
 * of domain 1. It then requests ON through the domain-1 row and polls the
 * row once per tick. The owner admits the row, writes and reads back each
 * delay-row word and writes the ON command; the recording layer then reads
 * the status back to back with CNTPCT stamps until it reads ON or 10 ms
 * pass (power.h gives the effective 20 ms bound). Afterwards the client
 * reads the delay row, policy and status again.
 *
 * The summary and the outcome line print first, then the detail lines while
 * they fit, then "TRACE END board bundle 7". Outcomes: status 0 with
 * "COMPLETE board bundle 7: domain 1 ON"; status 0 with "COMPLETE board
 * bundle 7: inconclusive" when the owner refused the row before any PPU
 * write (a gate, the supply record or adoption); status 1 with "FAILED board
 * bundle 7: domain 1", the bundle's controlled outcome. */
#include "power.h"
#include "platform.h"
#include "boot.h"
#include "a733_power_authority.h"

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
        if (!bb_power_read(A733_POWER_PCK_DOMAIN(1) + words[i], &out[i]))
            bb_power_fail("board bundle 7: domain 1 read");
    return 1;
}
static void detail_words(const char *prefix, unsigned read, const uint32_t *v)
{
    struct bb_line l;
    for (unsigned i = 0; read && i < WORDS; ++i) {
        bb_word_line(prefix, words[i], v[i], &l);
        bb_power_detail(&l);
    }
}

static void client(void *unused)
{
    (void)unused;
    uint32_t before[WORDS] = {0}, after[WORDS] = {0};
    struct bb7_summary s = {0};
    platform_puts(BB7_BEGIN);
    bb_power_wait_ready();
    bb_power_acquire(A733_RAIL_BIT(A733_RAIL_DCDC2));
    s.before_read = read_domain(before);
    s.owner_first = bb_power.accessed;
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
    const char *outcome;
    unsigned status = 0;
    if (state == A733_POWER_ROW_FAILED) {
        outcome = BB7_FAILED;
        status = 1;
    } else if (state == A733_POWER_ROW_REFUSED) {
        if (bb_power.writes)
            bb_power_fail("board bundle 7: refused after a PPU write");
        bb_power_park();
        outcome = BB7_REFUSED;
    } else if (state == A733_POWER_ROW_ON) {
        s.after_read = read_domain(after);
        bb_power_park();
        outcome = BB7_ON;
    } else {
        bb_power_fail("board bundle 7: row state");
    }
    s.row_state = state;
    s.domain_state = bb_power.domains.domain[0].state;
    s.state = a733_power_authority_load(&bb_power.rails.authority);
    s.accesses = bb_power.accessed;
    s.writes = bb_power.writes;
    s.status = bb_power.status;
    struct bb_line lines[BB7_SUMMARY_LINES];
    const unsigned n = bb7_summary_lines(&s, lines);
    for (unsigned i = 0; i < n; ++i)
        bb_power_print(&lines[i]);
    platform_puts(outcome);
    detail_words("BEFORE", s.before_read, before);
    bb_power_detail_accesses();
    for (unsigned i = 0; i < s.status.changes && i < BB_STATUS_CHANGES; ++i) {
        bb_status_line(&s.status, i, &lines[0]);
        bb_power_detail(&lines[0]);
    }
    detail_words("AFTER", s.after_read, after);
    bb_power_details_end();
    platform_puts(BB7_TRACE_END);
    platform_exit(status);
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{
    (void)handle;
    platform_panic(name, 0);
}

void kernel_main(void)
{
    platform_init();
    if (!bb_power_init(client, "bundle7"))
        platform_panic("board bundle 7: power initialization", 0);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
