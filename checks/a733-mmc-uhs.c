/* UHS-I profile through the actual storage platform, MMC core and shared PMIC,
 * rail, pin and clock providers. Card, MMIO and time are modeled; the earlier
 * fixture's own test main is absent. */
#include "mmc-platform-fixture.h"
#include "sunxi-mmc-uhs-card.h"
#include "sd_note.h"
#define PERI400 ((1u << 11) | (1u << 2) | (1u << 1))
#define PERI300 ((1u << 11) | (1u << 9) | (1u << 5) | (1u << 4))
#include <sys/mman.h>
#ifndef MAP_32BIT
#define MAP_32BIT 0x40
#endif
static struct uhs u;
static unsigned pf_flip, cfg_nowindow, cfg_dma;
/* The probe and the OS owner configure descriptor DMA (a 64-byte aligned area below 4 GiB, outside the result page) even for the identification and the
 * restart that never use it: the platform's IDMAC value rule and the adapter's register list then apply to the restart's writes. */
static volatile struct sunxi_mmc_dma_descriptor *dma_area;
static int uhs_dma_sync(void *v, uint8_t *b, size_t n, unsigned phase) { (void)v; (void)b; (void)n; (void)phase; return 1; }
static unsigned pf_settle, pf_writes, module_writes, restore_settle, restore_writes, cfg_sdr25, cfg_start18, commands_at_mismatch;
static uint32_t last_module;
static void live(struct storage_model *s)
{
    assert(s->admitted && s->power.register_held && s->platform.pin.id && !s->power.pins.failed);
    assert(a733_rail_status(&s->power.rails, &s->platform.rail) == A733_RAIL_READY);
    assert((*pmic_model_word(&s->power, s->platform.module) & (1u << 31)) &&
           (*pmic_model_word(&s->power, s->platform.bus) & 0x10001) == 0x10001);
}
static int uhs_platform_read(void *v, uintptr_t a, uint32_t *out)
{
    struct storage_model *s = v; ++s->accesses;
    assert(s->power.register_held && a733_mmc_platform_native_address_selected(SUNXI_MMC_SD, 0, a, 0));
    if (a >= s->platform.base && a <= s->platform.base + 0x300) {
        live(s); u.base.now = s->power.now; return uhs_read(&u, (uint32_t)(a - s->platform.base), out);
    }
    /* The bank reports 1.8 V a few status reads after the selector write, and 3.3 V a few after the restore write. */
    if (a == 0x02000048 && u.bank == 1 && pf_settle && !--pf_settle) {
        uint32_t *w = pmic_model_word(&s->power, a); *w = (*w & ~(3u << 10)) | (3u << 10);
        u.bank = 2; u.restarted = s->power.now;
    }
    /* How the board's bank may answer after the write back to 3.3 V: 0 two reads of 1.8 V then 3.3 V; 1 3.3 V at once; 2 3.3 V once,
     * then 1.8 V again; 3 3.3 V but the selector reads back as written 1.8 V; 4 3.3 V but the withstand mode changes to manual. */
    if (a == 0x02000048 && u.bank == 3 && restore_settle) {
        unsigned scenario = getenv("PF_RESTORE") ? (unsigned)atoi(getenv("PF_RESTORE")) : 0;
        uint32_t *w = pmic_model_word(&s->power, a);
        if (scenario == 1 || !--restore_settle) { *w = (*w & ~(3u << 10)) | (2u << 10); u.bank = 0; restore_settle = 0;
            if (scenario == 4) *pmic_model_word(&s->power, 0x02000040) = (*pmic_model_word(&s->power, 0x02000040) & ~(3u << 10)) | (2u << 10);
            if (scenario == 3) *pmic_model_word(&s->power, 0x02000070) = 0;
            if (scenario == 2) pf_flip = 1; }
    }
    if (a == 0x02000048 && pf_flip == 2) { uint32_t *w = pmic_model_word(&s->power, a); *w = (*w & ~(3u << 10)) | (3u << 10); }
    if (a == 0x02000048 && pf_flip == 1) pf_flip = 2;
    *out = *pmic_model_word(&s->power, a); return 1;
}
static int uhs_platform_write(void *v, uintptr_t a, uint32_t value)
{
    struct storage_model *s = v; ++s->accesses; ++s->writes;
    assert(s->power.register_held && s->admitted);
    if (a == 0x02000070 && value == 1) {
        /* The close: back to 3.3 V once, after a 1.8 V lifetime, with the card clock off and nothing in flight. */
        assert(u.bank == 2 && !restore_writes && !(u.base.reg[4/4] & (3u << 16)) && u.phase == 3);
        ++restore_writes; u.bank = 3; restore_settle = 2; *pmic_model_word(&s->power, a) = 1; return 1;
    }
    if (a == 0x02000070) {
        /* One write, 1.8 V, with the card clock stopped after CMD11 (or, for a re-entry, before the core opens). */
        assert(value == 0 && (u.phase == 2 || cfg_start18) && !pf_writes && !u.base.reg[4/4]);
        ++pf_writes; u.bank = 1; *pmic_model_word(&s->power, a) = 0; return 1;
    }
    assert(a733_mmc_platform_native_address_selected(SUNXI_MMC_SD, 0, a, 1));
    if (a >= s->platform.base && a <= s->platform.base + 0x300) {
        live(s); u.base.now = s->power.now;
        /* A command with the bank at 1.8 V (selector written) and a card still signalling 3.3 V would drive 3.3 V into the bank. */
        if (a == s->platform.base + 0x18 && (value & (1u << 31)) && !(value & (1u << 21)) && u.bank >= 1 && u.bank <= 2 && u.phase == 0)
            ++commands_at_mismatch;
        if (a == s->platform.base + 0x30 || a == s->platform.base + 0x38) *pmic_model_word(&s->power, 0x03400218) &= ~2u;     /* the controller's reset and the clear of its status deassert the line */
        return uhs_write(&u, (uint32_t)(a - s->platform.base), value);
    }
    if (a == 0x03400198) { assert(value == 2u); *pmic_model_word(&s->power, 0x03400118) &= ~value; return 1; }
    assert(a == s->platform.module || a == s->platform.bus);
    if (a == s->platform.module) {
        uint32_t setting = value & 0x07001f1fu;
        assert(setting == 29 || (setting & ~31u) == 0x01000000 || setting == 0x02000000);   /* identification, 400 MHz / 2 to 32, 300 MHz / 1 */
        if (value & (1u << 31)) {
            if (!s->module_at) s->module_at = s->power.now;
            if (setting != 29) { ++module_writes; last_module = value; }
        }
    }
    *pmic_model_word(&s->power, a) = value; return 1;
}
static void begin(unsigned pf_mode, unsigned fast, unsigned caps)
{
    if (cfg_dma && !dma_area) {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | 0x20 /* MAP_ANONYMOUS */ | MAP_32BIT, -1, 0);
        assert(p != (void *)-1 && (uintptr_t)p < (UINT64_C(1) << 32) - 4096); dma_area = p;
    }
    memset(&m, 0, sizeof(m)); memset(&u, 0, sizeof(u));
    pf_flip = 0; pf_settle = 3; pf_writes = module_writes = restore_writes = restore_settle = commands_at_mismatch = 0; last_module = 0;
    pmic_model_setup(&m.power); seed(&u.base, &m.platform.core, SUNXI_MMC_SD, 1);
    field(u.base.csd, 84, 12, 5 | (1u << 10)); field(u.base.csd, 96, 8, 0x32); u.scr[0] = 2; u.scr[1] = 5;
    u.caps = caps; u.s18a = 1; u.sdr104 = 1; u.tune_kinds = 8; u.fatal_delay = 99; u.window[0][0] = 20; u.window[0][1] = cfg_nowindow ? 0 : 24;
    if (cfg_start18) u.phase = 3;                      /* the card still signals 1.8 V from the earlier lifetime */
    m.power.pmic[0x11] = 1u << 4; m.power.pmic[0x33] = 13;
    *pmic_model_word(&m.power, 0x02002000) = 0xf8000000u | (99u << 16) | (99u << 8);
    *pmic_model_word(&m.power, 0x02002580) = 1u << 31;
    *pmic_model_word(&m.power, 0x02002588) = 1u << 31;
    *pmic_model_word(&m.power, 0x020025c0) = 1u << 24;
    *pmic_model_word(&m.power, 0x020025e0) = 1u << 30;
    *pmic_model_word(&m.power, 0x0200259c) = 0x30001;
    /* PERI0 at 2.4 GHz, P0 = 1: 400M = 2400/6 and 300M = 2400/8. */
    *pmic_model_word(&m.power, 0x020020a0) = 0xfe000000u | (99u << 8) | (1u << 20);
    uint32_t gates = PERI400 | (fast ? PERI300 : 0);
    *pmic_model_word(&m.power, 0x02003908) = gates | (gates << 16);
    *pmic_model_word(&m.power, 0x02003988) = gates << 16;
    *pmic_model_word(&m.power, 0x02000040) = (pf_mode << 10) | (3u << 4);
    *pmic_model_word(&m.power, 0x02000048) = 2u << 10;
    *pmic_model_word(&m.power, 0x02000070) = 1;
    struct a733_mmc_platform_config c = {.owner = 42, .handoff = 200, .topology = 201, .kind = SUNXI_MMC_SD,
        .deadline_us = 8000000, .lifetime_deadline_us = 10000000, .voltage_mask = 0x00300000,
        .sd_profile = SUNXI_MMC_SD_UHS_SDR104, .sdr25_only = (uint8_t)cfg_sdr25, .start_1v8 = (uint8_t)cfg_start18, .dma = cfg_dma ? dma_area : NULL,
        .fabric = {0, 1u << 31, 1u << 31},
        .fabric_hz = {24000000, 24000000, 24000000}};
    for (unsigned i = 0; i < 7; ++i) { c.desired[i].function = i < 6 ? 2 : 0; c.desired[i].pull = 1; }
    const struct a733_mmc_platform_ops o = {storage_now, storage_current, storage_admit, storage_enter,
        storage_leave, storage_retain, uhs_platform_read, uhs_platform_write, storage_barrier, storage_outside,
        storage_read_fifo, storage_write_fifo, cfg_dma ? uhs_dma_sync : NULL};
    assert(a733_mmc_platform_init(&m.platform, &c, &o, &m, &m.power.clocks, &m.power.pins, &m.power.rails,
                                  &m.power.provider));
}
static enum a733_mmc_platform_result uhs_step(void)
{
    m.power.now += 100; u.base.fifo_this_poll = 0;
    assert(pmic_model_step(&m.power) != A733_PMIC_RETAINED);
    return a733_mmc_platform_poll(&m.platform);
}
static enum a733_mmc_platform_result settle(void)
{
    enum a733_mmc_platform_result r = A733_MMC_PENDING;
    for (unsigned i = 0; i < 40000 && r != A733_MMC_READY && r != A733_MMC_RETAINED; ++i) r = uhs_step();
    return r;
}
static void read_ok(uint8_t *buffer)
{
    struct sunxi_mmc_result result;
    assert(a733_mmc_platform_read(&m.platform, 8, 8, buffer, 4096, 9000000) == A733_MMC_PENDING);
    enum a733_mmc_platform_result r = A733_MMC_PENDING;
    for (unsigned i = 0; i < 400 && r != A733_MMC_COMPLETE; ++i) { r = uhs_step(); assert(r != A733_MMC_RETAINED); }
    assert(r == A733_MMC_COMPLETE && a733_mmc_platform_take(&m.platform, &result, 9000000) == A733_MMC_READY);
    assert(result.received == 4096 && !result.write_started);
}
static void lost(uint8_t *buffer, uintptr_t address, uint32_t clear, uint32_t set)
{
    begin(1, 1, 3); assert(settle() == A733_MMC_READY);
    assert(a733_mmc_platform_read(&m.platform, 8, 8, buffer, 4096, 9000000) == A733_MMC_PENDING);
    unsigned commands = u.base.command_count;
    uint32_t *w = pmic_model_word(&m.power, address); *w = (*w & ~clear) | set;
    assert(uhs_step() == A733_MMC_RETAINED && m.platform.core.borrowed == buffer && u.base.command_count == commands);
    retained();
}
/* Runtime recovery through the platform: a failed read is A733_MMC_ERROR, the recovery policy identifies again at the same ceiling (a new tuning) and then steps
 * down one mode, the card at 1.8 V is re-entered at 1.8 V with no new PF write or CMD11, and every step is in the card's record. */
static enum a733_mmc_platform_result failed_read(uint8_t *buffer)
{
    assert(a733_mmc_platform_read(&m.platform, 8, 8, buffer, 4096, 9000000) == A733_MMC_PENDING);
    enum a733_mmc_platform_result r = A733_MMC_PENDING;
    for (unsigned i = 0; i < 400 && r != A733_MMC_ERROR; ++i) { r = uhs_step(); assert(r != A733_MMC_RETAINED); }
    assert(r == A733_MMC_ERROR && m.platform.stage == 15 /* READY */ && !m.platform.fault);
    return r;
}
static void recover_to_ready(void)
{
    assert(a733_mmc_platform_recover(&m.platform, 9000000) == A733_MMC_PENDING);
    enum a733_mmc_platform_result r = settle();
    if (r != A733_MMC_READY) fprintf(stderr, "recover: result %d platform stage %u fault line %u refused %u core state %d error %d protocol %u setup %u restart %u step %u\n", (int)r, m.platform.stage, m.platform.fault_line, m.platform.refused, (int)m.platform.core.state, (int)m.platform.core.result.error, m.platform.core.protocol, m.platform.core.setup, m.platform.core.restart, m.platform.core.command_step);
    assert(r == A733_MMC_READY);
}
/* A write that faulted: the core is in FAULT with a write running, the way the platform meets a CRC status error or a busy timeout of a bounded write. The platform
 * reports it as recoverable, with the failure record's fields intact until a restart; a restart abandons the piece and the owner writes it again. */
static void write_fault(uint8_t *buffer, enum sunxi_mmc_error error)
{
    struct sunxi_mmc *c = &m.platform.core;
    c->state = SUNXI_MMC_FAULT; c->result.error = error; c->writing = 1; c->dma_write = 1; c->borrowed = buffer; c->borrowed_bytes = 512;
    c->protocol = 27; c->result.interrupts = 0x8080u; c->result.tbc0 = 96u * 512u; c->result.tbc1 = 112u * 512u; c->result.desc_index = 12; c->result.where_valid = 1;
    c->result.dma_status = (1u << 2) | (1u << 9); c->write_request.blocks = 256;                 /* an IDMAC fatal bus error and its summary, in a piece of 256 sectors */
    /* A fault in the data phase of a tuned write leaves the controller as the write set it: the new timing mode and the write's sample delay. The restart must not inherit them. */
    u.base.reg[0x5c / 4] |= 0x80000110u; u.base.reg[0x144 / 4] = 0x80u;               /* a data CRC error and an end-bit error, as a CRC status failure raises them */
    u.prog_busy = 1;                                                /* the card is left programming, with DAT0 low, until the restart resets it with CMD0 */
    if (m.platform.irq_mode) *pmic_model_word(&m.power, 0x03400218) |= 2u;                  /* the owner masked INTID 193, but the status that raised it still asserts the line: pending until the status is cleared */
    enum a733_mmc_platform_result r = uhs_step();
    assert(r == A733_MMC_ERROR && m.platform.stage == 15 && !m.platform.fault);
}
static void write_recovery(uint8_t *buffer)
{
    /* A write CRC error at SDR104 steps down to SDR50 and the retry completes (a read is the stand-in for the retried write: the platform's recovery does not tell them apart). */
    begin(1, 1, 3); u.sdr50 = 1; assert(settle() == A733_MMC_READY); assert(m.platform.core.card.timing == 3);
    /* The write owner runs in interrupt mode (the third write session on 3-3 stopped because it could not leave it with the core faulted): the platform restart refuses
     * while the mode is on, and the owner's call to leave it must work in that state. */
    assert(a733_mmc_platform_irq(&m.platform, 1) && m.platform.irq_mode);
    write_fault(buffer, SUNXI_MMC_CARD);
    assert(a733_mmc_platform_recover(&m.platform, 9000000) == A733_MMC_INVALID);        /* the mode is still on */
    assert(a733_mmc_platform_irq(&m.platform, 0) && !m.platform.irq_mode);
    m.platform.recover_errors = 0;
    /* The record's fields while the fault stands: the core's error, protocol step and interrupt status, the pieces written and the timing. */
    {
        uint64_t detail = os_sd_where_detail(&m.platform.core, m.platform.core.card.timing), aux = os_sd_where_aux(&m.platform.core);
        uint32_t value = os_sd_note_value(34, detail);
        /* stage in 5:0; then status bits 15:1 in 20:6, timing 22:21, descriptor 28:23, error 31:29; aux: 96 blocks in 8:0, the first fault's 5 descriptors in 14:9 */
        assert((value & 0x3f) == 34 && ((value >> 6) & 0x7fff) == (0x8080u >> 1) && ((value >> 21) & 3) == 3 && ((value >> 23) & 63) == 12 && (value >> 29) == SUNXI_MMC_CARD);
        assert((aux & 511) == 96 && ((aux >> 9) & 15) == 9 /* bus error and summary */ && ((aux >> 13) & 3) == 3 /* a piece of 256 sectors */);
    }
    recover_to_ready(); assert(m.platform.core.card.timing == 3);                       /* the first error: tuned again */
    assert(!(u.base.reg[0x5c / 4] & 0x80000331u) && (u.base.reg[0x144 / 4] & 0xbfu) == (0x80u | m.platform.core.card.sample_delay));      /* the old mode and the tuned delay are back */
    assert(m.platform.core.card.mode_log[0].error == SUNXI_MMC_CARD && m.platform.core.card.mode_log[0].protocol == 27 && m.platform.core.card.mode_log[0].interrupts == 0x8080u);
    assert(m.platform.core.card.mode_log[0].blocks == 96 && m.platform.core.card.mode_log[0].descriptor == 12);        /* where the transfer stopped */
    write_fault(buffer, SUNXI_MMC_CARD); recover_to_ready();                           /* the second in a row: one step down */
    assert(!(u.base.reg[0x5c / 4] & 0x80000331u) && m.platform.core.card.timing == 2);
    assert(m.platform.core.card.timing == 2 && m.platform.core.card.uhs_outcome == SUNXI_MMC_UHS_SDR50 && m.platform.core.card.mode_steps == 2);
    assert(m.platform.core.card.mode_log[1].reason == SUNXI_MMC_MODE_ERRORS && m.platform.core.card.mode_log[1].from == 3);
    /* The session that stepped down and completed still carries both faults: the report word has them, with the core's error and interrupt status of each. */
    {
        uint64_t events = os_sd_fault_events(&m.platform.core.card);
        for (unsigned k = 0; k < 2; ++k) {
            uint64_t f = events >> (32 * k) & 0xffffffffu;
            assert((f & 15) == SUNXI_MMC_CARD && ((f >> 4) & 63) == 27 && ((f >> 10) & 0xffff) == 0x8080u && ((f >> 26) & 63) == 12);       /* and the descriptor the IDMAC had reached */
        }
        uint64_t where = os_sd_fault_blocks(&m.platform.core.card);
        assert((where >> 32 & 511) == 96 && (where >> 48 & 511) == 96 && (where >> 41 & 15) == 9 && (where >> 57 & 15) == 9 && (where >> 45 & 3) == 3 && (where >> 61 & 3) == 3);                                                                     /* the card-side blocks of each */
    }
    read_ok(buffer);                                                                    /* the retry completes and clears the count */
    assert(m.platform.recover_errors == 0 && !m.platform.core.writing);
    /* Down to the lowest mode (SDR12 on a card at 1.8 V), and a stop there is a clean fault: the record is intact and the platform is not restarted. */
    for (unsigned i = 0; i < 4; ++i) { write_fault(buffer, SUNXI_MMC_TIMEOUT); recover_to_ready(); }
    assert(m.platform.core.card.timing == 1 || m.platform.core.card.timing == 0);
    for (unsigned i = 0; i < 4 && m.platform.core.card.timing; ++i) { write_fault(buffer, SUNXI_MMC_CONTROLLER); recover_to_ready(); }
    assert(m.platform.core.card.timing == 0 && m.platform.core.card.card_hz == 25000000);
    write_fault(buffer, SUNXI_MMC_CARD); recover_to_ready();
    write_fault(buffer, SUNXI_MMC_CARD);
    assert(a733_mmc_platform_recover(&m.platform, 9000000) == A733_MMC_INVALID);
    assert(m.platform.core.state == SUNXI_MMC_FAULT && m.platform.core.result.error == SUNXI_MMC_CARD && m.platform.core.result.interrupts == 0x8080u && m.platform.core.protocol == 27);
    /* An unrecoverable error (a lost register, a deadline in the platform) is not offered for a restart. */
    begin(1, 1, 3); assert(settle() == A733_MMC_READY);
    m.platform.core.state = SUNXI_MMC_FAULT; m.platform.core.result.error = SUNXI_MMC_IO; m.platform.core.writing = 1;
    assert(uhs_step() == A733_MMC_RETAINED);
}
/* The retry budget of an owner is the ladder: faults with no success in between walk SDR104, SDR50, SDR25, SDR12 and end at the lowest mode, in A733_MMC_RECOVERIES calls
 * to the recovery, the last of them refused. A budget below it ends a session at SDR50 (the fourth format run on 3-3 did). */
static void recovery_budget(uint8_t *buffer)
{
    begin(1, 1, 3); u.sdr50 = 1; assert(settle() == A733_MMC_READY);
    unsigned calls = 0; enum a733_mmc_platform_result r;
    for (;;) {
        write_fault(buffer, SUNXI_MMC_CARD);
        r = a733_mmc_platform_recover(&m.platform, 9000000); ++calls;
        if (r != A733_MMC_PENDING) break;
        assert(settle() == A733_MMC_READY);
    }
    assert(r == A733_MMC_INVALID && calls == A733_MMC_RECOVERIES && m.platform.core.card.timing == 0 && m.platform.core.state == SUNXI_MMC_FAULT);
}
static void runtime_recovery(uint8_t *buffer)
{
    /* One error: identify again at the same ceiling, SDR104 tuned again, with no PF write, CMD11 or bank restore. */
    begin(1, 1, 3); assert(settle() == A733_MMC_READY); read_ok(buffer);
    unsigned cmd19 = u.cmd19; u.read_fail = 1; failed_read(buffer); recover_to_ready();
    assert(m.platform.core.card.timing == 3 && m.platform.core.card.mode_steps == 1 && m.platform.core.card.mode_log[0].reason == SUNXI_MMC_MODE_RETUNE);
    assert(u.cmd19 > cmd19 && pf_writes == 1 && u.cmd11 == 1 && !restore_writes && m.platform.signal == 2 && m.platform.core.card.reentered);
    read_ok(buffer); assert(m.platform.recover_errors == 0);
    /* Two errors in a row step down: this card has no SDR50, so SDR25 at 50 MHz, still at 1.8 V. */
    u.read_fail = 2; failed_read(buffer); recover_to_ready(); failed_read(buffer); recover_to_ready();
    assert(m.platform.core.card.timing == 1 && m.platform.core.card.signal_1v8 && m.platform.core.card.card_hz == 50000000 && u.sdr == 1);
    assert(m.platform.core.card.mode_steps == 3 && m.platform.core.card.mode_log[2].reason == SUNXI_MMC_MODE_ERRORS && m.platform.core.card.mode_log[2].from == 3);
    assert(pf_writes == 1 && u.cmd11 == 1 && !restore_writes);
    read_ok(buffer);
    /* An error storm in the tuned modes walks down SDR104, SDR50, SDR25: the card has SDR50 and every fast read fails. */
    begin(1, 1, 3); u.sdr50 = 1; assert(settle() == A733_MMC_READY); u.read_fail_fast = 1;
    for (unsigned i = 0; i < 8 && m.platform.core.card.timing >= 2; ++i) { failed_read(buffer); recover_to_ready(); }
    assert(m.platform.core.card.timing == 1 && m.platform.core.card.signal_1v8 && pf_writes == 1 && u.cmd11 == 1);
    read_ok(buffer);
    /* A 3.3 V card steps down from high speed to default speed, and a card that fails at every mode is final. */
    begin(2, 1, 2); assert(settle() == A733_MMC_READY); assert(m.platform.core.card.timing == 1 && !m.platform.core.card.signal_1v8);
    u.read_fail = 2; failed_read(buffer); recover_to_ready(); failed_read(buffer); recover_to_ready();
    assert(m.platform.core.card.timing == 0 && u.sdr == 0 && m.platform.core.card.card_hz == 25000000 && !pf_writes);
    read_ok(buffer);
    u.read_fail_always = 1; failed_read(buffer); recover_to_ready(); failed_read(buffer);
    assert(a733_mmc_platform_recover(&m.platform, 9000000) == A733_MMC_INVALID);
}
int main(void)
{
    static uint8_t buffer[4096];
    /* Adaptive PF mode and PERI0_300M: one selector write, then SDR104 at 150 MHz. */
    begin(1, 1, 3); assert(settle() == A733_MMC_READY);
    assert(pf_writes == 1 && m.platform.signal == 2 && *pmic_model_word(&m.power, 0x02000070) == 0);
    assert(((*pmic_model_word(&m.power, 0x02000048) >> 10) & 3) == 3 && ((*pmic_model_word(&m.power, 0x02000040) >> 10) & 3) == 1);
    assert(m.platform.core.card.timing == 3 && m.platform.core.card.card_hz == 150000000 && u.s18r_seen);
    assert(last_module == 0x82000000u && m.platform.running_module == 0x82000000u && module_writes == 1);
    assert(m.platform.core.card.sample_delay == 32 && m.platform.core.card.uhs_outcome == SUNXI_MMC_UHS_SDR104);
    read_ok(buffer); read_ok(buffer);
    /* Without the 300 MHz branch: 400 MHz / 2, a 100 MHz card clock. */
    begin(1, 0, 1); assert(settle() == A733_MMC_READY);
    assert(last_module == 0x81000001u && m.platform.core.card.card_hz == 100000000 && pf_writes == 1);
    read_ok(buffer);
    /* Manual 3.3 V PF mode offers no switch: high speed at 3.3 V, no PF write. */
    begin(2, 1, 2); assert(settle() == A733_MMC_READY);
    assert(!pf_writes && !m.platform.signal && last_module == 0x81000003u && !u.cmd11);
    assert(m.platform.core.card.uhs_outcome == SUNXI_MMC_UHS_HOST && m.platform.core.card.card_hz == 50000000);
    read_ok(buffer);
    /* A bank that never reports 1.8 V retains every resource after the one write. */
    begin(1, 1, 3); pf_settle = 0; assert(settle() == A733_MMC_RETAINED);
    assert(pf_writes == 1 && m.platform.core.result.error == SUNXI_MMC_SIGNAL && !u.cid_seen); retained();
    /* After the switch: losing 1.8 V, the selector or the 300 MHz branch retains. */
    lost(buffer, 0x02000048, 3u << 10, 2u << 10);
    lost(buffer, 0x02000070, 0, 1);
    lost(buffer, 0x02003988, 1u << 21, 0);
    /* The default aperture filter keeps the PF selector read-only; THLD is a
     * selected SMHC register. */
    assert(!a733_mmc_platform_native_address(SUNXI_MMC_SD, 0x02000070, 1));
    assert(a733_mmc_platform_native_address(SUNXI_MMC_SD, 0x04020100, 1));
    assert(!a733_mmc_platform_native_address_selected(SUNXI_MMC_EMMC, A733_MMC_EMMC_SDC2, 0x04022100, 1));
    assert(!a733_mmc_platform_native_address_selected(SUNXI_MMC_EMMC, A733_MMC_EMMC_SDC3, 0x04023100, 0));
    runtime_recovery(buffer);
    write_recovery(buffer); recovery_budget(buffer);
    puts("PASS UHS-I platform: one adaptive-mode PF write, 150 MHz from PERI0_300M, 100 MHz without it, "
         "3.3 V high speed in manual PF mode, retained bank timeout and three retained losses after the switch");
    return 0;
}
