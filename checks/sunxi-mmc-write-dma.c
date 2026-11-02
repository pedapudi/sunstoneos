/* Strict model of the SMHC controller and an SD card for CMD25 writes through descriptor DMA, driven by
 * the real core (drivers/storage/sunxi_mmc.c). The model accepts only the register accesses the vendor
 * sequence uses (docs/designs/mmc-multiblock-write.md) and asserts on any other. It consumes the
 * descriptor chain in order, clears each OWN bit, moves the data into a media array, and raises the
 * command-done, data-done and automatic-stop-done interrupts and the DAT0 busy status on a schedule of
 * register reads. The sentinel pattern outside the written range must survive. */
#define _GNU_SOURCE
#include "sunxi_mmc.h"
#include "sunxi_smhc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define SECTORS 600u
struct model {
    uint32_t reg[0x304 / 4];
    uint8_t *media;
    unsigned kind_sd, sector_addressed;
    uint64_t now;
    /* current transfer */
    int cmd_active, data_cmd, dma_on, next_desc, descs, ticks_cc, ticks_dtc, ticks_acd, busy_left, start_pending_reads;
    uint64_t first, bytes;
    unsigned prg_polls_left, programming_state;
    /* injections */
    unsigned busy_polls, prg_polls, crc_error, card_error_cmd, card_error_stop, idst_error, never_done, hang_busy;
    unsigned revoke_after, authority_calls, cache_before, cache_after, sync_order_bad;
    unsigned thld_initial;
    unsigned commands[16], command_count;
    unsigned writes_seen, reads_seen;
};
static struct model m;
static struct sunxi_mmc h;
static volatile struct sunxi_mmc_dma_descriptor *desc;
static uint8_t *source;

static void *low_alloc(size_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    assert(p != MAP_FAILED && (uintptr_t)p < (UINT64_C(1) << 32) - bytes);
    return p;
}
static uint8_t sentinel(unsigned i) { return (uint8_t)(i * 131u + 7u); }
static uint8_t payload(unsigned i) { return (uint8_t)(i * 29u + 3u); }

static void check_chain(void)
{
    unsigned n = (unsigned)((m.bytes + 4095) / 4096);
    assert(m.reg[SUNXI_SMHC_DLBA / 4] == (uint32_t)((uintptr_t)desc >> 2));
    for (unsigned i = 0; i < n; ++i) {
        uint32_t want = SUNXI_SMHC_DES_OWN | SUNXI_SMHC_DES_CH | SUNXI_SMHC_DES_DIC;
        if (!i) want |= SUNXI_SMHC_DES_FD;
        if (i == n - 1) want = (want | SUNXI_SMHC_DES_LD) & ~SUNXI_SMHC_DES_DIC;
        size_t left = m.bytes - (size_t)i * 4096;
        assert(desc[i].config == want);
        assert(desc[i].size == (left < 4096 ? left : 4096));
        assert(desc[i].buffer == (uint32_t)(((uintptr_t)source + (size_t)i * 4096) >> 2));
        assert(desc[i].next == (uint32_t)((uintptr_t)&desc[i + 1] >> 2));
    }
    assert(!desc[n].config && !desc[n].size && !desc[n].buffer && !desc[n].next);
    m.descs = (int)n;
}

static void dma_step(void)
{
    if (m.next_desc >= m.descs) return;
    volatile struct sunxi_mmc_dma_descriptor *d = &desc[m.next_desc];
    assert(d->config & SUNXI_SMHC_DES_OWN);
    memcpy(m.media + m.first * 512 + (size_t)m.next_desc * 4096, (void *)(uintptr_t)((uint64_t)d->buffer << 2), d->size);
    d->config &= ~SUNXI_SMHC_DES_OWN;
    ++m.next_desc;
}

static int read32(void *ctx, uint32_t off, uint32_t *out)
{
    (void)ctx; ++m.reads_seen;
    assert(!(off & 3) && off < 0x304);
    switch (off) {
    case SUNXI_SMHC_GCTRL: *out = m.reg[0] & ~7u; return 1;                /* resets self-clear */
    case SUNXI_SMHC_CMD:
        *out = m.reg[off / 4];
        if (m.start_pending_reads && !--m.start_pending_reads) m.reg[off / 4] &= ~SUNXI_SMHC_CMD_START;
        return 1;
    case SUNXI_SMHC_RESP0: case SUNXI_SMHC_RESP1: case SUNXI_SMHC_DMAC: case SUNXI_SMHC_THLD:
        *out = off == SUNXI_SMHC_DMAC ? m.reg[off / 4] & ~SUNXI_SMHC_DMAC_RESET : m.reg[off / 4]; return 1;
    case SUNXI_SMHC_RINT:
        if (m.cmd_active) {
            if (m.ticks_cc) { if (!--m.ticks_cc) m.reg[off / 4] |= SUNXI_SMHC_INT_CMD_DONE; }
            if (m.data_cmd == 25 && m.dma_on) {
                if (m.next_desc < m.descs) dma_step();
                else if (m.never_done) {}
                else if (m.ticks_dtc) { if (!--m.ticks_dtc) { m.reg[off / 4] |= SUNXI_SMHC_INT_DATA_DONE; m.busy_left = (int)m.busy_polls; } }
                else if (m.ticks_acd) { if (!--m.ticks_acd) m.reg[off / 4] |= SUNXI_SMHC_INT_AUTO_STOP_DONE; }
            }
            if (m.crc_error && m.next_desc > 0) m.reg[off / 4] |= 1u << 7;
        }
        *out = m.reg[off / 4]; return 1;
    case SUNXI_SMHC_STATUS:
        *out = SUNXI_SMHC_STATUS_FIFO_EMPTY;
        if (m.busy_left > 0 || m.hang_busy) { *out |= 1u << 9; if (m.busy_left > 0) --m.busy_left; }
        else if (m.cmd_active && m.data_cmd == 25 && m.ticks_dtc) *out |= 1u << 10;
        return 1;
    case SUNXI_SMHC_CBCR: *out = (uint32_t)((size_t)m.next_desc * 4096 < m.bytes ? (size_t)m.next_desc * 4096 : m.bytes); return 1;
    case SUNXI_SMHC_IDST:
        *out = m.reg[off / 4];
        if (m.dma_on && m.next_desc < m.descs) *out |= 1u << 13;          /* IDMAC state: busy */
        if (m.idst_error && m.next_desc > 0) *out |= 1u << 5;
        return 1;
    default: assert(!"unexpected controller read"); return 0;
    }
}

static void command(uint32_t value)
{
    unsigned index = value & 63u;
    assert(m.command_count < 16);
    m.commands[m.command_count++] = index;
    m.reg[SUNXI_SMHC_CMD / 4] = value;
    m.start_pending_reads = 1;
    m.cmd_active = 1; m.data_cmd = 0; m.reg[SUNXI_SMHC_RINT / 4] = 0; m.ticks_cc = 1;
    if (index == 13) {
        assert(value == (SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_WAIT | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_RESP | 13u));
        assert(m.reg[SUNXI_SMHC_ARG / 4] == 0x12340000u);
        if (m.writes_seen && m.prg_polls_left) { --m.prg_polls_left; m.reg[SUNXI_SMHC_RESP0 / 4] = 7u << 9; }
        else m.reg[SUNXI_SMHC_RESP0 / 4] = 0x900;
        return;
    }
    assert(index == 25);
    assert(value == (SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_WAIT | SUNXI_SMHC_CMD_AUTO_STOP | SUNXI_SMHC_CMD_WRITE |
                     SUNXI_SMHC_CMD_DATA | SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_RESP | 25u));
    assert(m.reg[SUNXI_SMHC_BLKSZ / 4] == 512 && m.reg[SUNXI_SMHC_BCNT / 4] == m.bytes);
    assert(m.reg[SUNXI_SMHC_A12A / 4] == 0);
    assert(m.reg[SUNXI_SMHC_ARG / 4] == (m.sector_addressed ? m.first : m.first * 512));
    uint32_t thld = m.reg[SUNXI_SMHC_THLD / 4];
    assert(((thld >> 16) & 0xfff) == 512 && (thld & 4) && !(thld & 1));       /* vendor thld_ctl for a write */
    assert(m.reg[SUNXI_SMHC_FTRGL / 4] == SUNXI_SMHC_FTRGL_VALUE && m.reg[SUNXI_SMHC_IDIE / 4] == 0);
    assert(m.reg[SUNXI_SMHC_DMAC / 4] == SUNXI_SMHC_DMAC_ON && (m.reg[0] & SUNXI_SMHC_GCTRL_DMA_ENABLE));
    assert(!(m.reg[0] & SUNXI_SMHC_GCTRL_CPU_FIFO));
    assert(m.cache_before == 1 && !m.cache_after);                             /* cleaned before the command */
    check_chain();
    m.data_cmd = 25; m.dma_on = 1; m.next_desc = 0; m.ticks_dtc = 2; m.ticks_acd = 2;
    m.reg[SUNXI_SMHC_RESP0 / 4] = m.card_error_cmd ? 1u << 25 : 0x900;
    m.reg[SUNXI_SMHC_RESP1 / 4] = m.card_error_stop ? 1u << 25 : 0x900;
    ++m.writes_seen;
}

static int write32(void *ctx, uint32_t off, uint32_t value)
{
    (void)ctx;
    assert(!(off & 3) && off < 0x304);
    switch (off) {
    case SUNXI_SMHC_GCTRL: case SUNXI_SMHC_BLKSZ: case SUNXI_SMHC_BCNT: case SUNXI_SMHC_ARG: case SUNXI_SMHC_FTRGL:
    case SUNXI_SMHC_A12A: case SUNXI_SMHC_DLBA: case SUNXI_SMHC_IDIE: m.reg[off / 4] = value;
        if (off == SUNXI_SMHC_GCTRL && (value & SUNXI_SMHC_GCTRL_DMA_ENABLE)) assert(m.cache_before == 1);
        return 1;
    case SUNXI_SMHC_THLD: m.reg[off / 4] = value; return 1;
    case SUNXI_SMHC_RINT: m.reg[off / 4] &= ~value; return 1;
    case SUNXI_SMHC_IDST: m.reg[off / 4] &= ~value; return 1;
    case SUNXI_SMHC_DMAC:
        if (value & SUNXI_SMHC_DMAC_RESET) { m.dma_on = 0; m.reg[off / 4] = value; return 1; }
        assert(value == SUNXI_SMHC_DMAC_ON); m.reg[off / 4] = value; return 1;
    case SUNXI_SMHC_CMD: command(value); return 1;
    default: assert(!"unexpected controller write"); return 0;
    }
}
static int no_fifo_read(void *c, uint32_t *w, unsigned n) { (void)c; (void)w; (void)n; assert(!"a DMA write used the FIFO"); return 0; }
static int no_fifo_write(void *c, const uint32_t *w, unsigned n) { (void)c; (void)w; (void)n; assert(!"a DMA write used the FIFO"); return 0; }
static int barrier(void *c) { (void)c; return 1; }
static int clock_now(void *c, uint64_t *out) { (void)c; m.now += 50; *out = m.now; return 1; }
static int admit(void *c, uintptr_t token, enum sunxi_mmc_kind kind, uint32_t hz) { (void)c; assert(token == 77 && kind == SUNXI_MMC_SD && hz == 800000); return 1; }
static int retained(void *c, uintptr_t token) { (void)c; assert(token == 77); return 1; }
static int dma_sync(void *c, uint8_t *b, size_t n, unsigned phase)
{
    (void)c;
    assert(b == source && n == m.bytes);
    if (phase == SUNXI_MMC_DMA_BEFORE) { ++m.cache_before; assert(!m.cache_after); }
    else { ++m.cache_after; assert(m.cache_before == 1 && m.next_desc == m.descs); }   /* invalidate only after the chain closed */
    return 1;
}
static int authorization(void *c, const struct sunxi_mmc_write_identity *id, const struct sunxi_mmc_write_request *r)
{
    (void)c; ++m.authority_calls;
    assert(id->core == (uintptr_t)&h && id->session == 77 && id->kind == SUNXI_MMC_SD);
    if (m.revoke_after && m.authority_calls > m.revoke_after) return 0;
    return r->policy_token == 901 && r->first_sector == m.first && r->bytes == m.bytes && r->source == source;
}

static void setup(unsigned sector_addressed, unsigned blocks, uint64_t first)
{
    memset(&m, 0, sizeof m);
    static uint8_t *media; if (!media) media = low_alloc(SECTORS * 512);
    if (!source) source = low_alloc(256 * 512 + 4096);
    if (!desc) desc = low_alloc(SUNXI_MMC_DMA_DESCRIPTORS * sizeof *desc);
    for (unsigned i = 0; i < SECTORS * 512; ++i) media[i] = sentinel(i);
    for (unsigned i = 0; i < 256 * 512; ++i) source[i] = payload(i);
    memset((void *)desc, 0, SUNXI_MMC_DMA_DESCRIPTORS * sizeof *desc);
    m.media = media; m.sector_addressed = sector_addressed; m.first = first; m.bytes = (uint64_t)blocks * 512;
    m.reg[SUNXI_SMHC_THLD / 4] = 0x0ff10001u;                              /* stale read threshold left by an earlier read */
    m.busy_polls = 3; m.prg_polls_left = 0;
    const struct sunxi_mmc_config c = {{read32, write32, barrier, clock_now, admit, retained, NULL, no_fifo_read, no_fifo_write, NULL, dma_sync},
        &m, 77, SUNXI_MMC_SD, 800000, 0x00300000, SUNXI_MMC_SD_LEGACY, desc};
    memset(&h, 0, sizeof h);
    assert(sunxi_mmc_init(&h, &c));
    /* An already identified, selected card (the identification suites run elsewhere). */
    h.card.sectors = SECTORS; h.card.rca = 0x1234; h.card.sector_addressed = (uint8_t)sector_addressed;
    h.card.ocr = (1u << 31) | (sector_addressed ? 1u << 30 : 0) | 0x00300000;
    h.setup = 6; h.admitted = 1; h.state = SUNXI_MMC_READY;
}
static void csd_writable(void)
{
    /* Card bit n is word (127-n)/32, bit (n%32); class 4 = bit 84+4, block length bits 25:22 = 9. */
    memset(h.card.csd, 0, sizeof h.card.csd);
    h.card.csd[(127 - 88) / 32] |= 1u << (88 % 32);
    for (unsigned b = 0; b < 4; ++b) if ((9u >> b) & 1u) h.card.csd[(127 - (22 + b)) / 32] |= 1u << ((22 + b) % 32);
}
static struct sunxi_mmc_write_request request(unsigned blocks)
{ return (struct sunxi_mmc_write_request){901, m.first, blocks, source, blocks * 512u, 1000000}; }
static int start(unsigned blocks)
{
    struct sunxi_mmc_write_request r = request(blocks);
    struct sunxi_mmc_write_authority a = {&m, authorization};
    return sunxi_mmc_write_dma(&h, &r, &a);
}
static void finish(void)
{
    for (unsigned n = 0; n < 20000 && h.state == SUNXI_MMC_WRITING; ++n) sunxi_mmc_poll(&h);
    assert(h.state == SUNXI_MMC_DONE || h.state == SUNXI_MMC_FAULT);
}
static void media_is(unsigned blocks)
{
    for (unsigned i = 0; i < SECTORS * 512; ++i) {
        int inside = i >= m.first * 512 && i < (m.first + blocks) * 512;
        assert(m.media[i] == (inside ? payload(i - (unsigned)m.first * 512) : sentinel(i)));
    }
}
static void media_untouched_outside(unsigned blocks)
{
    for (unsigned i = 0; i < SECTORS * 512; ++i)
        if (!(i >= m.first * 512 && i < (m.first + blocks) * 512)) assert(m.media[i] == sentinel(i));
}

int main(void)
{
    static const unsigned counts[] = {1, 2, 7, 8, 9, 64, 255, 256};
    for (unsigned sa = 0; sa <= 1; ++sa)
        for (unsigned i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
            unsigned blocks = counts[i];
            setup(sa, blocks, 40 + i);
            csd_writable();
            m.prg_polls = i % 3; m.prg_polls_left = m.prg_polls; m.busy_polls = 1 + i % 5;
            assert(start(blocks));
            assert(h.state == SUNXI_MMC_WRITING && h.dma_write);
            finish();
            assert(h.state == SUNXI_MMC_DONE);
            assert(h.result.confirmed_blocks == blocks && h.result.submitted == blocks * 512u && h.result.write_started);
            assert(m.cache_before == 1 && m.cache_after == 1 && m.command_count >= 3);
            assert(m.commands[0] == 13 && m.commands[1] == 25 && m.commands[m.command_count - 1] == 13);
            media_is(blocks);
            struct sunxi_mmc_result result;
            assert(sunxi_mmc_take(&h, &result) && h.state == SUNXI_MMC_READY && !h.borrowed && !h.dma_write);
            assert(result.error == SUNXI_MMC_OK && m.authority_calls > 3);
            /* The controller is left quiescent: no DMA active, resets complete. */
            assert(!m.dma_on && !(m.reg[0] & SUNXI_SMHC_GCTRL_DMA_ENABLE));
        }
    /* Validation is pure: no register access, no authority call, no borrow. */
    setup(1, 2, 10); csd_writable();
    struct sunxi_mmc_write_request r = request(2);
    struct sunxi_mmc_write_authority a = {&m, authorization};
    assert(!sunxi_mmc_write_dma(&h, &r, NULL));
    r.blocks = 0; r.bytes = 0; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    r = request(2); r.blocks = 257; r.bytes = 257 * 512; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    r = request(2); r.bytes = 1000; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    r = request(2); r.source = source + 32; assert(!sunxi_mmc_write_dma(&h, &r, &a));                 /* not 64-byte aligned */
    r = request(2); r.first_sector = SECTORS - 1; assert(!sunxi_mmc_write_dma(&h, &r, &a));            /* past the card */
    r = request(2); r.policy_token = 0; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    r = request(2); r.source = (const uint8_t *)&h; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    r = request(2); r.source = (const uint8_t *)desc; assert(!sunxi_mmc_write_dma(&h, &r, &a));       /* overlaps the descriptors */
    r = request(2); r.deadline_us = 0; assert(!sunxi_mmc_write_dma(&h, &r, &a));
    h.config.dma = NULL; r = request(2); assert(!sunxi_mmc_write_dma(&h, &r, &a)); h.config.dma = desc;
    memset(h.card.csd, 0, sizeof h.card.csd); assert(!start(2));                                      /* not writable */
    assert(!m.reads_seen && !m.authority_calls && !h.borrowed && h.state == SUNXI_MMC_READY);
    /* Faults retain the operation and never write outside the range. */
    struct { const char *name; unsigned *knob; unsigned value; enum sunxi_mmc_error error; } faults[] = {
        {"data crc", &m.crc_error, 1, SUNXI_MMC_CONTROLLER},
        {"card error in the CMD25 response", &m.card_error_cmd, 1, SUNXI_MMC_CARD},
        {"card error in the CMD12 response", &m.card_error_stop, 1, SUNXI_MMC_CARD},
        {"IDMAC error", &m.idst_error, 1, SUNXI_MMC_CONTROLLER},
        {"data never completes", &m.never_done, 1, SUNXI_MMC_TIMEOUT},
        {"DAT0 stays busy", &m.hang_busy, 1, SUNXI_MMC_TIMEOUT},
    };
    for (unsigned i = 0; i < sizeof faults / sizeof faults[0]; ++i) {
        setup(1, 9, 100); csd_writable();
        *faults[i].knob = faults[i].value;
        struct sunxi_mmc_write_request fr = request(9); struct sunxi_mmc_write_authority fa = {&m, authorization};
        assert(sunxi_mmc_write_dma(&h, &fr, &fa));
        for (unsigned n = 0; n < 20000 && h.state == SUNXI_MMC_WRITING; ++n) {
            if (n == 3000) m.now += 2000000;                                                              /* deadline for the hang cases */
            sunxi_mmc_poll(&h);
        }
        if (!(h.state == SUNXI_MMC_FAULT)) { fprintf(stderr, "no fault: %s\n", faults[i].name); return 1; }
        if (h.result.error != faults[i].error) { fprintf(stderr, "%s: error %d expected %d\n", faults[i].name, h.result.error, faults[i].error); return 1; }
        assert(h.borrowed == source && h.result.confirmed_blocks == 0);
        unsigned calls = m.reads_seen, auth = m.authority_calls;
        sunxi_mmc_poll(&h); assert(calls == m.reads_seen && auth == m.authority_calls);                    /* retained: no more access */
        media_untouched_outside(9);
    }
    /* The authority is asked on every poll and revocation stops the transfer between descriptors. */
    setup(1, 64, 200); csd_writable(); m.revoke_after = 8;
    assert(start(64)); finish();
    assert(h.state == SUNXI_MMC_FAULT && h.result.error == SUNXI_MMC_ADMISSION);
    media_untouched_outside(64);
    puts("PASS: CMD25 descriptor-DMA writes of 1 to 256 blocks, vendor register sequence and threshold, descriptor chain, DAT0 busy and programming status, cache phases, refusals without access, six retained faults and revocation");
    return 0;
}
