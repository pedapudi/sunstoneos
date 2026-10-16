#include "sunxi_mmc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mmc-reference.h"

/* Card model follows the command/state protocol. The MMIO model supplies FIFO
 * words independently of completion interrupts, so receiving bytes cannot
 * substitute for the controller's transfer and automatic-stop evidence. */
struct model {
    struct sunxi_mmc *host;
    uint32_t reg[0x304 / 4], csd[4], cid[4];
    uint8_t ext[512];
    uint64_t now;
    unsigned kind, sector, state, app, ocr_calls, selected, cmd16, reads;
    unsigned calls, commands[64], args[64], command_count, fifo_total, fifo_pos;
    unsigned data_command, fifo_this_poll, max_fifo, admitted, retain_calls;
    int admit_result, retain_result, bad_if, bad_rca, bad_card, no_dtc, no_acd;
    int bad_stop, bad_status, crc_after, late_crc, timed_cmd, fail_write, busy;
    int backwards, expiry_on_done, reentry, removal;
};
static void field(uint32_t *r, unsigned first, unsigned count, uint32_t v)
{
    for (unsigned b = 0; b < count; ++b) {
        unsigned p = first + b, word = 3 - p / 32, bit = p % 32;
        r[word] = (r[word] & ~(1u << bit)) | (((v >> b) & 1u) << bit);
    }
}
static void seed(struct model *m, struct sunxi_mmc *h, unsigned kind, unsigned sector)
{
    memset(m, 0, sizeof(*m)); memset(h, 0, sizeof(*h));
    m->host = h; m->kind = kind; m->sector = sector;
    m->admit_result = m->retain_result = 1; m->timed_cmd = -1; m->crc_after = -1;
    m->reg[0x300/4] = 0x03050530; m->reg[0x5c/4] = 0x81710331;
    m->reg[0x10c/4] = 0x80001234; m->reg[0x54/4] = 0x12345676;
    m->reg[0x58/4] = 0xffff; m->reg[0x104/4] = 7; m->reg[0x140/4] = 0xa5a40000;
    field(m->csd, 126, 2, kind == SUNXI_MMC_SD ? sector : 3);
    if (kind == SUNXI_MMC_EMMC) field(m->csd, 122, 4, 4);
    field(m->csd, 80, 4, 9); field(m->csd, 84, 12, 5);
    if (kind == SUNXI_MMC_SD && sector) field(m->csd, 48, 22, 0x7fff);
    else { field(m->csd, 62, 12, 0xfff); field(m->csd, 47, 3, 5); }
    for (unsigned i = 0; i < 4; ++i) m->cid[i] = 0x12345678u + i;
    m->ext[194] = 2; m->ext[192] = 8;
    if (sector) m->ext[215] = 1;
}
static uint32_t status(struct model *m)
{
    uint32_t v = (m->fifo_pos == m->fifo_total ? 4u : 0u);
    if (m->busy) v |= 1u << 9;
    return v;
}
static int read32(void *ctx, uint32_t off, uint32_t *out)
{
    struct model *m = ctx; ++m->calls;
    assert(off <= 0x300 && !(off & 3));
    if (off == 0x38) {
        if (m->data_command && m->fifo_pos == m->fifo_total) {
            if (!m->no_dtc) m->reg[off/4] |= 8;
            if ((m->data_command & (1u << 12)) && !m->no_acd) m->reg[off/4] |= 1u << 14;
        }
        if (m->crc_after >= 0 && m->fifo_pos >= (unsigned)m->crc_after)
            m->reg[off/4] |= 1u << 7;
        if (m->removal) m->reg[off/4] |= 1u << 31;
    }
    if (off == 0x3c) {
        *out = status(m);
        if (m->late_crc && m->data_command && m->fifo_pos == m->fifo_total &&
            (m->reg[0x38/4] & 8u)) m->reg[0x38/4] |= 1u << 7;
        return 1;
    }
    if (off == 0x200) {
        assert(m->fifo_pos < m->fifo_total);
        assert(++m->fifo_this_poll <= 16);
        if (m->fifo_this_poll > m->max_fifo) m->max_fifo = m->fifo_this_poll;
        *out = 0;
        for (unsigned b = 0; b < 4; ++b) {
            unsigned p = m->fifo_pos++;
            uint8_t v = (m->data_command & 63u) == 8 ? m->ext[p] : (uint8_t)(p * 37u + 11u);
            *out |= (uint32_t)v << (b * 8);
        }
        return 1;
    }
    *out = m->reg[off/4]; return 1;
}
static void command(struct model *m, uint32_t cmd)
{
    assert((cmd & (1u << 10)) == 0); /* No media write command/data direction. */
    unsigned n = cmd & 63, arg = m->reg[0x1c/4];
    if (cmd & (1u << 21)) { m->reg[0x18/4] = 0; return; }
    assert(m->command_count < 64);
    m->commands[m->command_count] = n; m->args[m->command_count++] = arg;
    m->reg[0x18/4] = cmd & ~(1u << 31);
    m->reg[0x38/4] = (int)n == m->timed_cmd ? 1u << 8 : 4;
    m->reg[0x20/4] = (m->state << 9) | 0x100;
    m->data_command = 0; m->fifo_total = m->fifo_pos = 0;
    switch (n) {
    case 0:
        assert(cmd & (1u << 15)); assert(arg == 0); m->state = 0; break;
    case 8:
        if (m->kind == SUNXI_MMC_SD) {
            assert(!m->state && arg == 0x1aa && !(cmd & (1u << 9)));
            m->reg[0x20/4] = m->bad_if ? 0x2aa : 0x1aa;
        } else {
            assert(m->selected && arg == 0 && (cmd & (1u << 9)));
            assert(m->reg[0x14/4] == 512);
            m->data_command = cmd; m->fifo_total = 512;
        }
        break;
    case 55:
        assert(m->kind == SUNXI_MMC_SD && !arg && !m->state);
        m->app = 1; m->reg[0x20/4] |= 1u << 5; break;
    case 41:
        assert(m->kind == SUNXI_MMC_SD && m->app && arg == 0x40300000u);
        m->app = 0; ++m->ocr_calls;
        m->reg[0x20/4] = 0x00300000u | (m->sector ? 1u << 30 : 0);
        if (m->ocr_calls == 2) { m->reg[0x20/4] |= 1u << 31; m->state = 1; }
        break;
    case 1:
        assert(m->kind == SUNXI_MMC_EMMC);
        assert(arg == (m->ocr_calls ? 0x40300000u : 0));
        ++m->ocr_calls; m->reg[0x20/4] = 0x00300000u | (m->sector ? 1u << 30 : 0);
        if (m->ocr_calls == 3) { m->reg[0x20/4] |= 1u << 31; m->state = 1; }
        break;
    case 2:
        assert(m->state == 1 && arg == 0); m->state = 2;
        for (unsigned i = 0; i < 4; ++i) m->reg[(0x2c - i * 4)/4] = m->cid[i];
        break;
    case 3:
        assert(m->state == 2); m->state = 3;
        if (m->kind == SUNXI_MMC_SD) { assert(arg == 0); m->reg[0x20/4] = m->bad_rca ? 0 : 0x12340700; }
        else assert(arg == 0x10000);
        break;
    case 9:
        assert(m->state == 3);
        assert(arg == (m->kind == SUNXI_MMC_SD ? 0x12340000u : 0x10000u));
        for (unsigned i = 0; i < 4; ++i) m->reg[(0x2c - i * 4)/4] = m->csd[i];
        break;
    case 7:
        assert(m->state == 3); m->state = 4; m->selected = 1; break;
    case 13:
        assert(m->selected); m->reg[0x20/4] = m->reads && m->bad_status ? 0x800 : 0x900; break;
    case 16:
        assert(m->selected && !m->sector && arg == 512); ++m->cmd16; break;
    case 17: case 18:
        assert(m->selected && m->reg[0x10/4] == 512);
        assert(!m->fifo_total && (cmd & (1u << 9)));
        assert((n == 18) == !!(cmd & (1u << 12)));
        if (n == 18) assert(m->reg[0x58/4] == 0);
        ++m->reads; m->data_command = cmd; m->fifo_total = m->reg[0x14/4];
        assert(m->fifo_total >= 512 && m->fifo_total <= 4096);
        m->reg[0x20/4] = m->bad_card ? (1u << 25) : 0x900;
        m->reg[0x24/4] = m->bad_stop ? (1u << 19) : 0x900;
        break;
    default: assert(!"unsupported card command");
    }
}
static int write32(void *ctx, uint32_t off, uint32_t value)
{
    struct model *m = ctx; ++m->calls; assert(off <= 0x300 && !(off & 3));
    if (off == 0x38) { assert(!(value & 0xffff0000u)); m->reg[off/4] &= ~value; }
    else if (off == 0) { m->reg[off/4] = value & ~7u; if (value & 7) m->fifo_total = m->fifo_pos = 0; }
    else m->reg[off/4] = value;
    if (off == 0x18) {
        command(m, value);
        if (m->fail_write && (value & 63u) == 17) return 0; /* Already issued. */
    }
    return 1;
}
static int barrier(void *ctx) { ++((struct model *)ctx)->calls; return 1; }
static int now_us(void *ctx, uint64_t *out)
{
    struct model *m = ctx; ++m->calls;
    if (m->reentry) {
        struct sunxi_mmc_result result;
        assert(!sunxi_mmc_take(m->host, &result));
        assert(!sunxi_mmc_open(m->host, UINT64_MAX));
        sunxi_mmc_poll(m->host);
    }
    if (m->backwards) { *out = 0; return 1; }
    m->now += 50;
    if (m->expiry_on_done && m->host->state == SUNXI_MMC_DONE) m->now = m->host->deadline_us;
    *out = m->now; return 1;
}
static int admit(void *ctx, uintptr_t token, enum sunxi_mmc_kind kind, uint32_t hz)
{
    struct model *m = ctx; ++m->calls; ++m->admitted;
    assert(token == 0xa733 && (unsigned)kind == m->kind && hz == 800000);
    return m->admit_result;
}
static int retained(void *ctx, uintptr_t token)
{
    struct model *m = ctx; ++m->calls; ++m->retain_calls;
    assert(token == 0xa733); return m->retain_result;
}
static void init(struct model *m, struct sunxi_mmc *h, unsigned kind, unsigned sector)
{
    seed(m, h, kind, sector);
    struct sunxi_mmc_config c = {{read32, write32, barrier, now_us, admit, retained},
        m, 0xa733, (enum sunxi_mmc_kind)kind, 800000, 0x00300000};
    assert(sunxi_mmc_init(h, &c)); assert(!m->calls);
    assert(!sunxi_mmc_init(h, &c));
    assert(sunxi_mmc_open(h, 1000000)); assert(!m->calls);
}
static void run(struct model *m)
{
    for (unsigned i = 0; i < 20000; ++i) {
        if (m->host->state == SUNXI_MMC_READY || m->host->state == SUNXI_MMC_DONE ||
            m->host->state == SUNXI_MMC_FAULT) return;
        m->fifo_this_poll = 0; sunxi_mmc_poll(m->host);
    }
    assert(!"model failed to reach bounded terminal state");
}
static void identified(struct model *m, struct sunxi_mmc *h, unsigned kind, unsigned sector)
{
    init(m, h, kind, sector); run(m); assert(h->state == SUNXI_MMC_READY);
    assert(m->admitted == 1 && m->retain_calls);
    assert(!memcmp(h->card.cid, m->cid, 16) && !memcmp(h->card.csd, m->csd, 16));
    assert(m->cmd16 == !sector && h->card.sector_addressed == sector);
    assert(m->reg[0x5c/4] == 0x01710000 && m->reg[0x10c/4] == 0x1234);
    assert(m->reg[0x54/4] == 0x12345673 && m->reg[0x104/4] == 6);
    assert(m->reg[0x140/4] == 0xa5a70000 && m->reg[0x144/4] == 0x80);
    assert(m->reg[0x80/4] == 0 && m->reg[0x8c/4] == 0 && m->reg[0xc/4] == 0);
}
static void terminal(struct model *m, enum sunxi_mmc_error error, uint8_t *buffer)
{
    struct sunxi_mmc *h = m->host; struct sunxi_mmc_result result;
    assert(h->state == SUNXI_MMC_FAULT && h->result.error == error);
    assert(h->borrowed == buffer);
    unsigned calls = m->calls;
    sunxi_mmc_poll(h); assert(sunxi_mmc_take(h, &result));
    assert(h->state == SUNXI_MMC_FAULT && m->calls == calls && h->borrowed == buffer);
    assert(!sunxi_mmc_open(h, UINT64_MAX));
}
static void capacity_checks(void)
{
    struct model m; struct sunxi_mmc h; uint64_t n;
    for (unsigned kind = 1; kind <= 2; ++kind) {
        for (unsigned sector = 0; sector <= 1; ++sector) {
            seed(&m, &h, kind, sector);
            for (unsigned k = 0; k < 40; ++k) {
                if (kind == SUNXI_MMC_SD && sector) field(m.csd, 48, 22, k * 991u);
                else { field(m.csd, 62, 12, k * 97u); field(m.csd, 47, 3, k % 6); }
                uint32_t ocr = 0x80300000u | (sector ? 1u << 30 : 0);
                assert(sunxi_mmc_capacity((enum sunxi_mmc_kind)kind, m.csd, ocr, m.ext, &n));
                struct mmc_card ref = {0}; memcpy(ref.raw_csd, m.csd, 16);
                assert(!(kind == SUNXI_MMC_SD ? sd_reference(&ref) : mmc_reference(&ref)));
                uint64_t expected = (uint64_t)ref.csd.capacity << (ref.csd.read_blkbits - 9);
                if (kind == SUNXI_MMC_EMMC && sector) expected = 1u << 24;
                assert(n == expected);
            }
        }
    }
    seed(&m, &h, SUNXI_MMC_SD, 1); field(m.csd, 48, 22, 0x3fffff);
    assert(sunxi_mmc_capacity(SUNXI_MMC_SD, m.csd, 0xc0300000, 0, &n) && n == (1ull << 32));
    field(m.csd, 126, 2, 2); n = 99;
    assert(!sunxi_mmc_capacity(SUNXI_MMC_SD, m.csd, 0xc0300000, 0, &n) && n == 99);
    seed(&m, &h, SUNXI_MMC_EMMC, 1);
    unsigned offsets[] = {15, 61, 179, 183, 185, 192, 194};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i) {
        unsigned off = offsets[i]; uint8_t old = m.ext[off]; m.ext[off] = 255;
        assert(!sunxi_mmc_capacity(SUNXI_MMC_EMMC, m.csd, 0xc0300000, m.ext, &n)); m.ext[off] = old;
    }
    memset(m.ext + 212, 0, 4);
    assert(!sunxi_mmc_capacity(SUNXI_MMC_EMMC, m.csd, 0xc0300000, m.ext, &n));
    m.ext[215] = 1;
    assert(!sunxi_mmc_capacity(SUNXI_MMC_EMMC, m.csd, 0x80300000, m.ext, &n));
}
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--auto-stop-argument")) {
        struct model m; struct sunxi_mmc h; uint8_t data[1024];
        for (unsigned kind = 1; kind <= 2; ++kind) {
            identified(&m, &h, kind, 1); assert(m.reg[0x58/4] == 0xffff);
            assert(sunxi_mmc_read(&h, 0, 2, data, sizeof(data), m.now + 100000));
            run(&m); assert(h.state == SUNXI_MMC_DONE && m.reg[0x58/4] == 0);
        }
        puts("PASS: SD/eMMC automatic CMD12 explicitly uses argument zero despite inherited 0xffff");
        return 0;
    }
    assert(argc == 1);
    capacity_checks();
    struct model m; struct sunxi_mmc h; uint8_t buffer[4097]; struct sunxi_mmc_result result;
    for (unsigned kind = 1; kind <= 2; ++kind) {
        for (unsigned sector = 0; sector <= 1; ++sector) {
            identified(&m, &h, kind, sector); m.reentry = 1;
            unsigned calls = m.calls;
            assert(!sunxi_mmc_read(&h, h.card.sectors, 1, buffer, 512, m.now + 100000));
            assert(!sunxi_mmc_read(&h, h.card.sectors - 1, 2, buffer, 1024, m.now + 100000));
            assert(!sunxi_mmc_read(&h, UINT64_MAX, 8, buffer, 4096, m.now + 100000));
            assert(!sunxi_mmc_read(&h, 0, 9, buffer, 4096, m.now + 100000));
            assert(!sunxi_mmc_read(&h, 0, 1, (uint8_t *)&h, 512, m.now + 100000));
            assert(!sunxi_mmc_read(&h, 0, 1, (uint8_t *)(UINTPTR_MAX - 4), 512, m.now + 100000));
            assert(m.calls == calls);
            for (unsigned blocks = 1; blocks <= 8; blocks += 7) {
                assert(sunxi_mmc_read(&h, h.card.sectors - blocks, blocks, buffer + 1, blocks * 512, m.now + 100000));
                run(&m); assert(h.state == SUNXI_MMC_DONE && h.result.received == blocks * 512);
                for (unsigned i = 0; i < blocks * 512; ++i) assert(buffer[i + 1] == (uint8_t)(i * 37u + 11u));
                assert(m.commands[m.command_count - 1] == 13);
                assert(m.args[m.command_count - 2] == (h.card.sectors - blocks) * (sector ? 1u : 512u));
                assert(!sunxi_mmc_take(&h, (struct sunxi_mmc_result *)(void *)(buffer + 1)));
                assert(sunxi_mmc_take(&h, &result) && result.error == SUNXI_MMC_OK && !h.borrowed);
            }
            assert(m.max_fifo == 16);
        }
    }
    /* Maximum SDXC capacity keeps a 64-bit count and accepts its final LBA. */
    init(&m, &h, SUNXI_MMC_SD, 1); field(m.csd, 48, 22, 0x3fffff); run(&m);
    assert(h.card.sectors == (1ull << 32));
    assert(sunxi_mmc_read(&h, UINT32_MAX, 1, buffer, 512, m.now + 100000)); run(&m);
    assert(h.state == SUNXI_MMC_DONE && m.args[m.command_count - 2] == UINT32_MAX);
    assert(sunxi_mmc_take(&h, &result));
    /* Synthetic corrupt metadata cannot bypass argument-range admission. */
    h.card.sectors = UINT64_MAX;
    unsigned calls = m.calls;
    assert(!sunxi_mmc_read(&h, UINT32_MAX, 2, buffer, 1024, m.now + 10000));
    h.card.sector_addressed = 0;
    assert(!sunxi_mmc_read(&h, UINT32_MAX / 512u, 2, buffer, 1024, m.now + 10000));
    assert(m.calls == calls);
    for (unsigned scenario = 0; scenario < 12; ++scenario) {
        identified(&m, &h, SUNXI_MMC_SD, 1);
        switch (scenario) {
        case 0: m.no_dtc = 1; break;
        case 1: m.no_acd = 1; break;
        case 2: m.bad_stop = 1; break;
        case 3: m.bad_status = 1; break;
        case 4: m.crc_after = 64; break;
        case 5: m.bad_card = 1; break;
        case 6: m.fail_write = 1; break;
        case 7: m.late_crc = 1; break;
        case 8: m.removal = 1; break;
        case 9: m.retain_result = 2; break;
        case 10: m.expiry_on_done = 1; m.reentry = 1; break;
        case 11: m.busy = 1; break;
        }
        assert(sunxi_mmc_read(&h, 0, scenario == 6 ? 1 : 2, buffer, scenario == 6 ? 512 : 1024, m.now + 10000));
        run(&m);
        enum sunxi_mmc_error e = (scenario <= 1 || scenario == 10 || scenario == 11) ? SUNXI_MMC_TIMEOUT :
            (scenario == 2 || scenario == 3 || scenario == 5) ? SUNXI_MMC_CARD :
            scenario == 6 ? SUNXI_MMC_IO : scenario == 9 ? SUNXI_MMC_ADMISSION : SUNXI_MMC_CONTROLLER;
        terminal(&m, e, buffer);
        if (scenario <= 3 || scenario == 10) assert(h.result.received == 1024);
        if (scenario == 4) assert(h.result.received == 64);
        assert(m.command_count < 20); /* No retry/reset after any failure. */
    }
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        init(&m, &h, scenario == 1 ? SUNXI_MMC_EMMC : SUNXI_MMC_SD, 1);
        switch (scenario) {
        case 0: m.timed_cmd = 8; break;
        case 1: m.timed_cmd = 1; break;
        case 2: m.admit_result = 2; break;
        case 3: m.reg[0x18/4] = 1u << 31; break;
        case 4: m.reg[0x300/4] = 0x50200; break;
        case 5: m.bad_if = 1; break;
        case 6: m.bad_rca = 1; break;
        case 7: m.backwards = 1; h.last_us = 1; break;
        }
        run(&m);
        enum sunxi_mmc_error e = scenario <= 1 ? SUNXI_MMC_CONTROLLER :
            scenario <= 3 ? SUNXI_MMC_ADMISSION : scenario == 4 ? SUNXI_MMC_VARIANT :
            scenario == 5 ? SUNXI_MMC_UNSUPPORTED : scenario == 6 ? SUNXI_MMC_CARD : SUNXI_MMC_TIME;
        terminal(&m, e, 0);
        if (scenario == 0 || scenario == 1) assert(m.command_count == 2);
        if (scenario == 2 || scenario == 3 || scenario == 4 || scenario == 7) assert(!m.command_count);
    }
    puts("PASS: pinned CSD parity, SD/eMMC identification and bounded reads, full-range admission, independent completion/status, retained faults and callback guards");
    return 0;
}
