/* UHS-I profile through the actual storage platform, MMC core and shared PMIC,
 * rail, pin and clock providers. Card, MMIO and time are modeled; the earlier
 * fixture's own test main is absent. */
#include "mmc-platform-fixture.h"
#include "sunxi-mmc-uhs-card.h"
#define PERI400 ((1u << 11) | (1u << 2) | (1u << 1))
#define PERI300 ((1u << 11) | (1u << 9) | (1u << 5) | (1u << 4))
static struct uhs u;
static unsigned pf_settle, pf_writes, module_writes;
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
    /* The bank reports 1.8 V a few status reads after the selector write. */
    if (a == 0x02000048 && u.bank == 1 && pf_settle && !--pf_settle) {
        uint32_t *w = pmic_model_word(&s->power, a); *w = (*w & ~(3u << 10)) | (3u << 10);
        u.bank = 2; u.restarted = s->power.now;
    }
    *out = *pmic_model_word(&s->power, a); return 1;
}
static int uhs_platform_write(void *v, uintptr_t a, uint32_t value)
{
    struct storage_model *s = v; ++s->accesses; ++s->writes;
    assert(s->power.register_held && s->admitted);
    if (a == 0x02000070) {
        /* One write, 1.8 V, with the card clock stopped after CMD11. */
        assert(value == 0 && u.phase == 2 && !pf_writes && !u.base.reg[4/4]);
        ++pf_writes; u.bank = 1; *pmic_model_word(&s->power, a) = 0; return 1;
    }
    assert(a733_mmc_platform_native_address_selected(SUNXI_MMC_SD, 0, a, 1));
    if (a >= s->platform.base && a <= s->platform.base + 0x300) {
        live(s); u.base.now = s->power.now; return uhs_write(&u, (uint32_t)(a - s->platform.base), value);
    }
    if (a == 0x03400198) { assert(value == 2u); *pmic_model_word(&s->power, 0x03400118) &= ~value; return 1; }
    assert(a == s->platform.module || a == s->platform.bus);
    if (a == s->platform.module) {
        uint32_t setting = value & 0x07001f1fu;
        assert(setting == 29 || setting == 0x01000003 || setting == 0x01000001 || setting == 0x02000000);
        if (value & (1u << 31)) {
            if (!s->module_at) s->module_at = s->power.now;
            if (setting != 29) { ++module_writes; last_module = value; }
        }
    }
    *pmic_model_word(&s->power, a) = value; return 1;
}
static void begin(unsigned pf_mode, unsigned fast, unsigned caps)
{
    memset(&m, 0, sizeof(m)); memset(&u, 0, sizeof(u));
    pf_settle = 3; pf_writes = module_writes = 0; last_module = 0;
    pmic_model_setup(&m.power); seed(&u.base, &m.platform.core, SUNXI_MMC_SD, 1);
    field(u.base.csd, 84, 12, 5 | (1u << 10)); field(u.base.csd, 96, 8, 0x32); u.scr[0] = 2; u.scr[1] = 5;
    u.caps = caps; u.s18a = 1; u.sdr104 = 1; u.fatal_delay = 99; u.window[0][0] = 20; u.window[0][1] = 24;
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
        .sd_profile = SUNXI_MMC_SD_UHS_SDR104, .fabric = {0, 1u << 31, 1u << 31},
        .fabric_hz = {24000000, 24000000, 24000000}};
    for (unsigned i = 0; i < 7; ++i) { c.desired[i].function = i < 6 ? 2 : 0; c.desired[i].pull = 1; }
    const struct a733_mmc_platform_ops o = {storage_now, storage_current, storage_admit, storage_enter,
        storage_leave, storage_retain, uhs_platform_read, uhs_platform_write, storage_barrier, storage_outside,
        storage_read_fifo, storage_write_fifo};
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
    puts("PASS UHS-I platform: one adaptive-mode PF write, 150 MHz from PERI0_300M, 100 MHz without it, "
         "3.3 V high speed in manual PF mode, retained bank timeout and three retained losses after the switch");
    return 0;
}
