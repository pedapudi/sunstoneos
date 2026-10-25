/* Native storage controller. Source anchors: docs/mmc.md and docs/mmc-write.md. */
#include "sunxi_mmc.h"
#include "sunxi_smhc.h"
#include "a733_range.h"

#define START SUNXI_SMHC_CMD_START
#define UPDATE SUNXI_SMHC_UPDATE
#define RESP SUNXI_SMHC_CMD_RESP
#define LONG SUNXI_SMHC_CMD_LONG
#define CRC SUNXI_SMHC_CMD_CRC
#define DATA SUNXI_SMHC_CMD_DATA
#define WRITE SUNXI_SMHC_CMD_WRITE
#define AUTO SUNXI_SMHC_CMD_AUTO_STOP
#define WAIT SUNXI_SMHC_CMD_WAIT
#define CC SUNXI_SMHC_INT_CMD_DONE
#define DTC SUNXI_SMHC_INT_DATA_DONE
#define ACD SUNXI_SMHC_INT_AUTO_STOP_DONE
#define ERRORS SUNXI_SMHC_MMC_ERRORS
/* Includes locked, switch and SD authentication failures. ECC-disabled is
 * informational; erase-reset is also refused in this bounded profile. */
#define CARD_ERRORS 0xffffa088u
#define OUT_OF_RANGE (1u << 31)
#define TRAN_READY 0x900u
#define BUSY SUNXI_SMHC_STATUS_BUSY
#define FIFO_EMPTY SUNXI_SMHC_STATUS_FIFO_EMPTY
#define FIFO_FULL SUNXI_SMHC_STATUS_FIFO_FULL
#define VSW SUNXI_SMHC_CMD_VOLTAGE_SWITCH
#define VSD SUNXI_SMHC_INT_VOLTAGE_DONE
#define DAT3 SUNXI_SMHC_STATUS_DAT3
/* CLKCR MASK_DAT0 (manual 11.3.8.2): clock updates ignore the DAT0 the card
 * holds low during the switch, as the board kernel's every update does. */
#define MASK_DAT0 (1u << 31)
#define S18R (1u << 24) /* ACMD41 S18R argument and S18A response bit. */
/* Board kernel sunxi-mmc.c: each sample delay passes only after 20 matching
 * CMD19 blocks (MMC_TUNING_RETRY_TIMES), and a window narrower than 12 delays
 * is unusable (sample_win_th). */
#define TUNE_REPEATS 20u
#define TUNE_WINDOW 12u
/* Sampling errors expected at a bad sample delay: response error, response
 * and data CRC, response and data timeout, start and end bit. Others stay
 * terminal during tuning. */
#define TUNE_TOLERATED ((1u << 1) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | \
                        (1u << 13) | (1u << 15))
#define TUNE_TIMEOUT 0x040000ffu /* About 1.7 ms of data timeout at 150 MHz. */
#define UHS SUNXI_MMC_SD_UHS_SDR104
/* SUNXI_MMC_NO_UHS builds without the UHS-I profile: every UHS-I path is
 * behind this constant, so the compiler drops its code. */
#ifdef SUNXI_MMC_NO_UHS
#define UHS_ON 0
#else
#define UHS_ON 1
#endif

enum { P_IDLE, P_SD_IF, P_SD_APP, P_SD_OCR, P_MMC_QUERY, P_MMC_OCR,
       P_CID, P_RCA, P_CSD, P_SELECT, P_STATUS, P_BLOCKLEN, P_EXT,
       P_EXT_STATUS, P_READ, P_READ_STATUS, P_WRITE_PRE, P_WRITE, P_WRITE_STATUS,
       P_SD_SCR_APP, P_SD_SCR, P_SD_WIDTH_APP, P_SD_WIDTH,
       P_SD_SWITCH_CHECK, P_SD_SWITCH_SET, P_SD_CLOCK, P_SD_MODE_STATUS,
       P_SD_VOLTAGE, P_SD_SIGNAL, P_SD_UHS_CHECK, P_SD_UHS_SET, P_SD_TUNE,
       P_SD_TUNE_RECOVER };
/* SD physical layer 9.10 section 4.2.4.5 and the pinned Linux
 * tuning_blk_pattern_4bit: the 64-byte four-bit CMD19 tuning block. */
static const uint8_t tuning_pattern[64] = {
    0xff, 0x0f, 0xff, 0x00, 0xff, 0xcc, 0xc3, 0xcc, 0xc3, 0x3c, 0xcc, 0xff, 0xfe, 0xff, 0xfe, 0xef,
    0xff, 0xdf, 0xff, 0xdd, 0xff, 0xfb, 0xff, 0xfb, 0xbf, 0xff, 0x7f, 0xff, 0x77, 0xf7, 0xbd, 0xef,
    0xff, 0xf0, 0xff, 0xf0, 0x0f, 0xfc, 0xcc, 0x3c, 0xcc, 0x33, 0xcc, 0xcf, 0xff, 0xef, 0xff, 0xee,
    0xff, 0xfd, 0xff, 0xfd, 0xdf, 0xff, 0xbf, 0xff, 0xbb, 0xff, 0xf7, 0xff, 0xf7, 0x7f, 0x7b, 0xde };

static void fault(struct sunxi_mmc *h, enum sunxi_mmc_error e)
{ if (h->state != SUNXI_MMC_FAULT) { h->result.error = e; h->state = SUNXI_MMC_FAULT; } }
static int rd(struct sunxi_mmc *h, uint32_t o, uint32_t *v)
{
    if (h->config.ops.read32(h->config.context, o, v) == 1) return 1;
    fault(h, SUNXI_MMC_IO); return 0;
}
static int wr(struct sunxi_mmc *h, uint32_t o, uint32_t v)
{
    if (h->config.ops.write32(h->config.context, o, v) == 1) return 1;
    fault(h, SUNXI_MMC_IO); return 0;
}
static int fence(struct sunxi_mmc *h)
{
    if (h->config.ops.barrier(h->config.context) == 1) return 1;
    fault(h, SUNXI_MMC_IO); return 0;
}
static int time_ok(struct sunxi_mmc *h)
{
    uint64_t n;
    if (h->config.ops.now_us(h->config.context, &n) != 1) { fault(h, SUNXI_MMC_IO); return 0; }
    if (!a733_monotonic_advance(n, &h->last_us)) { fault(h, SUNXI_MMC_TIME); return 0; }
    if (n >= h->deadline_us) { fault(h, SUNXI_MMC_TIMEOUT); return 0; }
    return 1;
}
static void defer_command(struct sunxi_mmc *h, uint64_t delay_us)
{
    /* Sample after the observed clock/command completion, not before MMIO. */
    if (!time_ok(h)) return;
    if (!a733_deadline_after(h->last_us, delay_us, &h->retry_us)) fault(h, SUNXI_MMC_TIME);
}
static uint32_t bits(const uint32_t *r, unsigned start, unsigned count)
{
    unsigned word = 3 - start / 32, shift = start % 32;
    uint32_t v = r[word] >> shift;
    if (shift + count > 32) v |= r[word - 1] << (32 - shift);
    return v & ((1u << count) - 1);
}
int sunxi_mmc_capacity(enum sunxi_mmc_kind kind, const uint32_t c[4], uint32_t ocr,
                       const uint8_t ext[512], uint64_t *out)
{
    if (!a733_span_excl(c, 16) || !a733_span_excl(out, sizeof(*out)) ||
        a733_extents_overlap_prechecked((uintptr_t)c, 16, (uintptr_t)out, sizeof(*out)) ||
        (kind != SUNXI_MMC_SD && kind != SUNXI_MMC_EMMC) || !(ocr & START)) return 0;
    unsigned structure = bits(c, 126, 2), blockbits = bits(c, 80, 4);
    if ((bits(c, 84, 12) & 5u) != 5u || blockbits < 9 || blockbits > 11) return 0;
    uint64_t n;
    if (kind == SUNXI_MMC_SD) {
        if (structure > 1 || !!(ocr & (1u << 30)) != (structure == 1)) return 0;
        if (structure == 1) {
            if (blockbits != 9) return 0;
            n = ((uint64_t)bits(c, 48, 22) + 1) << 10;
        } else {
            n = ((uint64_t)bits(c, 62, 12) + 1) << (bits(c, 47, 3) + 2 + blockbits - 9);
            if (n > (1u << 22)) return 0; /* SDSC maximum 2 GiB. */
        }
    } else {
        if (!a733_span_excl(ext, 512) ||
            a733_extents_overlap_prechecked((uintptr_t)ext, 512, (uintptr_t)out, sizeof(*out)) ||
            !structure || bits(c, 122, 4) != 4 || ext[194] > 2 || ext[192] > 8 ||
            (ext[179] & 7u) || ext[183] || ext[185] || ext[61] || ext[15]) return 0;
        unsigned mode = (ocr >> 29) & 3u;
        if (mode != 0 && mode != 2) return 0;
        if (mode == 2) {
            if (ext[192] < 2) return 0;
            n = (uint32_t)ext[212] | ((uint32_t)ext[213] << 8) |
                ((uint32_t)ext[214] << 16) | ((uint32_t)ext[215] << 24);
        } else {
            uint32_t ext_sectors = (uint32_t)ext[212] | ((uint32_t)ext[213] << 8) |
                ((uint32_t)ext[214] << 16) | ((uint32_t)ext[215] << 24);
            if (ext[192] >= 2 && ext_sectors > (1u << 22)) return 0;
            n = ((uint64_t)bits(c, 62, 12) + 1) << (bits(c, 47, 3) + 2 + blockbits - 9);
            if (n > (1u << 22)) return 0;
        }
    }
    if (!n) return 0;
    *out = n; return 1;
}
int sunxi_mmc_init(struct sunxi_mmc *h, const struct sunxi_mmc_config *c)
{
    if (!a733_span_excl(h, sizeof(*h)) || !a733_span_excl(c, sizeof(*c)) ||
        a733_extents_overlap_prechecked((uintptr_t)h, sizeof(*h), (uintptr_t)c, sizeof(*c)) ||
        h->initialized || !c->ops.read32 || !c->ops.write32 || !c->ops.barrier ||
        !c->ops.now_us || !c->ops.admit || !c->ops.retained || !c->token ||
        !c->ops.read_fifo || !c->ops.write_fifo ||
        (c->kind != SUNXI_MMC_SD && c->kind != SUNXI_MMC_EMMC) ||
        c->module_hz < 200000 || c->module_hz > 800000 || (c->module_hz & 1u) ||
        (unsigned)c->sd_profile>UHS ||
        (c->sd_profile&&(c->kind!=SUNXI_MMC_SD||!c->ops.sd_module)) ||
        (c->sd_profile==UHS&&(!UHS_ON||!c->ops.sd_signal)) ||
        !c->voltage_mask || (c->voltage_mask & ~0x00300000u)) return 0;
    struct sunxi_mmc fresh = {0};
    fresh.config = *c; fresh.initialized = 1; *h = fresh; return 1;
}
int sunxi_mmc_open(struct sunxi_mmc *h, uint64_t deadline)
{
    if (!h || !h->initialized || h->entered || h->state != SUNXI_MMC_COLD ||
        deadline <= h->last_us) return 0;
    h->deadline_us = deadline; h->state = SUNXI_MMC_IDENTIFY; return 1;
}
static void queue(struct sunxi_mmc *h, unsigned protocol, uint32_t cmd, uint32_t arg,
                  uint8_t *data, size_t bytes)
{
    h->protocol = (uint16_t)protocol; h->command = cmd | START;
    h->argument = arg; h->data = data; h->data_bytes = bytes; h->position = 0;
    h->command_step = 0; h->seen = 0; h->response_seen = 0;
    for (unsigned i = 0; i < 4; ++i) h->response[i] = 0;
}
int sunxi_mmc_read(struct sunxi_mmc *h, uint64_t first, uint32_t blocks,
                   uint8_t *buffer, size_t bytes, uint64_t deadline)
{
    if (!h || !h->initialized || h->entered || h->state != SUNXI_MMC_READY ||
        deadline <= h->last_us || !blocks || blocks > SUNXI_MMC_MAX_BLOCKS ||
        bytes != blocks * 512u || !a733_span_excl(buffer, bytes) ||
        a733_extents_overlap_prechecked((uintptr_t)h, sizeof(*h), (uintptr_t)buffer, bytes) ||
        first >= h->card.sectors || blocks > h->card.sectors - first) return 0;
    uint64_t last = first + blocks - 1;
    if ((h->card.sector_addressed && last > UINT32_MAX) ||
        (!h->card.sector_addressed && last > UINT32_MAX / 512u)) return 0;
    uint32_t arg = (uint32_t)(h->card.sector_addressed ? first : first * 512);
    h->result = (struct sunxi_mmc_result){0}; h->deadline_us = deadline;
    h->borrowed = buffer; h->borrowed_bytes = bytes; h->state = SUNXI_MMC_READING;
    h->read_tail = blocks > 1 && blocks == h->card.sectors - first;
    queue(h, P_READ, (blocks == 1 ? 17u : (18u | AUTO)) | RESP | CRC | DATA | WAIT,
          arg, buffer, bytes);
    return 1;
}
static int same_card(const struct sunxi_mmc_card *a,const struct sunxi_mmc_card *b)
{
    for(unsigned i=0;i<4;++i)if(a->cid[i]!=b->cid[i]||a->csd[i]!=b->csd[i])return 0;
    return a->ocr==b->ocr&&a->sectors==b->sectors&&a->rca==b->rca&&
        a->sector_addressed==b->sector_addressed&&a->ext_revision==b->ext_revision;
}
static int same_request(const struct sunxi_mmc_write_request *a,const struct sunxi_mmc_write_request *b)
{
    return a->policy_token==b->policy_token&&a->first_sector==b->first_sector&&
        a->blocks==b->blocks&&a->source==b->source&&a->bytes==b->bytes&&a->deadline_us==b->deadline_us;
}
static int writable(const struct sunxi_mmc *h)
{
    /* CSD command class4, 512-byte writes, and neither whole-card WP flag.
     * Per-group protection is never changed; a card WP error remains terminal. */
    if(!(bits(h->card.csd,84,12)&(1u<<4))||bits(h->card.csd,22,4)!=9||
       bits(h->card.csd,12,2))return 0;
    return h->config.kind!=SUNXI_MMC_EMMC||
        (!(h->ext_csd[179]&7u)&&!h->ext_csd[61]&&!h->ext_csd[15]&&!h->ext_csd[33]&&
         !(h->ext_csd[171]&5u));
}
static int write_admitted(struct sunxi_mmc *h)
{
    const struct sunxi_mmc_write_identity *i=&h->write_identity;
    if(!h->writing||!h->write_authority.admit||i->core!=(uintptr_t)h||
       i->session!=h->config.token||i->kind!=h->config.kind||!same_card(&i->card,&h->card)||
       !writable(h)||h->deadline_us!=h->write_request.deadline_us||
       h->borrowed!=h->write_request.source||h->borrowed_bytes!=h->write_request.bytes)goto fail;
    struct sunxi_mmc_write_identity identity=*i;
    struct sunxi_mmc_write_request request=h->write_request;
    struct sunxi_mmc_write_authority authority=h->write_authority;
    int result=authority.admit(authority.context,&identity,&request);
    if(result!=1||identity.core!=(uintptr_t)h||identity.session!=h->config.token||
       identity.kind!=h->config.kind||!same_card(&identity.card,&h->card)||
       !same_card(&identity.card,&h->write_identity.card)||
       identity.core!=h->write_identity.core||identity.session!=h->write_identity.session||
       identity.kind!=h->write_identity.kind||!same_request(&request,&h->write_request)||
       authority.admit!=h->write_authority.admit||authority.context!=h->write_authority.context||
       !writable(h)||h->deadline_us!=request.deadline_us||h->borrowed!=request.source||
       h->borrowed_bytes!=request.bytes)goto fail;
    return 1;
fail: fault(h,SUNXI_MMC_ADMISSION);return 0;
}
int sunxi_mmc_write(struct sunxi_mmc *h,const struct sunxi_mmc_write_request *r,
                    const struct sunxi_mmc_write_authority *a)
{
    if(!h||!h->initialized||h->entered||h->state!=SUNXI_MMC_READY||
       !a733_span_excl(r,sizeof(*r))||!a733_span_excl(a,sizeof(*a))||!a->admit||!r->policy_token||
       a733_extents_overlap_prechecked((uintptr_t)h,sizeof(*h),(uintptr_t)r,sizeof(*r))||
       a733_extents_overlap_prechecked((uintptr_t)h,sizeof(*h),(uintptr_t)a,sizeof(*a))||
       !r->blocks||r->blocks>SUNXI_MMC_MAX_BLOCKS||r->bytes!=r->blocks*512u||
       !a733_span_excl(r->source,r->bytes)||
       a733_extents_overlap_prechecked((uintptr_t)h,sizeof(*h),(uintptr_t)r->source,r->bytes)||
       a733_extents_overlap_prechecked((uintptr_t)r,sizeof(*r),(uintptr_t)r->source,r->bytes)||
       a733_extents_overlap_prechecked((uintptr_t)a,sizeof(*a),(uintptr_t)r->source,r->bytes)||
       r->deadline_us<=h->last_us||r->first_sector>=h->card.sectors||
       r->blocks>h->card.sectors-r->first_sector||!writable(h))return 0;
    uint64_t last=r->first_sector+r->blocks-1;
    if((h->card.sector_addressed&&last>UINT32_MAX)||
       (!h->card.sector_addressed&&last>UINT32_MAX/512u))return 0;
    h->write_request=*r;h->write_authority=*a;
    h->write_identity=(struct sunxi_mmc_write_identity){(uintptr_t)h,h->config.token,h->config.kind,h->card};
    for(size_t n=0;n<r->bytes;++n)h->write_payload[n]=r->source[n];
    h->result=(struct sunxi_mmc_result){0};h->deadline_us=r->deadline_us;
    h->borrowed=(uint8_t *)r->source;h->borrowed_bytes=r->bytes;
    h->writing=1;h->state=SUNXI_MMC_WRITING;
    queue(h,P_WRITE_PRE,13u|RESP|CRC|WAIT,(uint32_t)h->card.rca<<16,0,0);
    return 1;
}
static int interrupts(struct sunxi_mmc *h)
{
    uint32_t v;
    if (!rd(h, 0x38, &v)) return 0;
    h->result.interrupts = v; h->seen |= v;
    /* Bit 10 reports switch completion while the CMD11 sequence runs. */
    uint32_t errors = v & ERRORS & ~(UHS_ON && h->signal_window ? VSD : 0u);
    if (errors) {
        /* A sampling error marks this CMD19 delay as failed; recovery resets
         * the controller FIFO and state machines before the next delay. */
        if (UHS_ON && h->tuning && h->protocol == P_SD_TUNE && !(errors & ~TUNE_TOLERATED)) {
            h->tune_failed = 1; return 0;
        }
        fault(h, SUNXI_MMC_CONTROLLER); return 0;
    }
    return 1;
}
/* Cold controller setup; admission is physical evidence supplied by a binding,
 * never inferred from a successful register read or a modeled callback. */
static void setup(struct sunxi_mmc *h)
{
    uint32_t v;
    switch (h->setup) {
    case 0:
        if (h->config.ops.admit(h->config.context, h->config.token, h->config.kind,
                                h->config.module_hz) != 1) { fault(h, SUNXI_MMC_ADMISSION); return; }
        h->admitted = 1; ++h->setup; return;
    case 1:
        if (!rd(h, 0x300, &h->version)) return;
        v = h->version & 0xffffffu;
        if (v < 0x50300u || v >= 0x60000u) { fault(h, SUNXI_MMC_VARIANT); return; }
        if (!rd(h, 0x18, &v)) return;
        if (v & START) { fault(h, SUNXI_MMC_ADMISSION); return; }
        if (!rd(h, 0x3c, &v)) return;
        if (v & (BUSY | START)) { fault(h, SUNXI_MMC_ADMISSION); return; }
        if (!rd(h, 0x80, &v)) return;
        if (v & (1u << 7)) { fault(h, SUNXI_MMC_ADMISSION); return; }
        if (!wr(h, 0, 7)) return;
        ++h->setup; return;
    case 2:
        if (!rd(h, 0, &v) || (v & 7u)) return;
        if (!wr(h, 4, 0) || !wr(h, 0x30, 0) || !wr(h, 0x8c, 0) ||
            !wr(h, 0x80, 0) || !wr(h, 0, START)) return;
        ++h->setup; return;
    case 3:
        if (!rd(h, 0x5c, &v) || !wr(h, 0x5c, v & ~(START | 0x331u)) ||
            !rd(h, 0x104, &v) || !wr(h, 0x104, v & ~1u) ||
            !rd(h, 0x10c, &v) || !wr(h, 0x10c, v & ~START) ||
            !rd(h, 0x54, &v) || !wr(h, 0x54, (v & ~15u) | 3u) ||
            !rd(h, 0x140, &v) || !wr(h, 0x140, (v & ~(3u << 16)) | (3u << 16)) ||
            !wr(h, 0x144, 0x80) || !wr(h, 0xc, 0) || !wr(h, 8, UINT32_MAX) ||
            !fence(h) || !wr(h, 0x18, UPDATE)) return;
        ++h->setup; return;
    case 4:
        if (!rd(h, 0x18, &v) || (v & START)) return;
        if (!interrupts(h) || !wr(h, 4, 1u << 16) || !fence(h) || !wr(h, 0x18, UPDATE)) return;
        ++h->setup; return;
    case 5:
        if (!rd(h, 0x18, &v) || (v & START)) return;
        if (!interrupts(h) || !rd(h, 4, &v)) return;
        if (v != (1u << 16)) { fault(h, SUNXI_MMC_CONTROLLER); return; }
        if (!rd(h, 0xc, &v)) return;
        if (v) { fault(h, SUNXI_MMC_CONTROLLER); return; }
        if (!rd(h, 0, &v)) return;
        if ((v & ((1u << 31) | (1u << 5) | 7u)) != START) { fault(h, SUNXI_MMC_CONTROLLER); return; }
        h->setup = 6;
        queue(h, P_IDLE, 1u << 15, 0, 0, 0);
        defer_command(h, 1000); return;
    default: return;
    }
}
/* One STATUS sample sizes a FIFO burst of at most one 512-byte block per poll.
 * Reads take the reported fill level, or one word when only FIFO_EMPTY is
 * clear; writes take the free space. Controller completion does not publish
 * data; the protocol subsequently confirms the card's ready/TRAN state. */
static int fifo_step(struct sunxi_mmc *h)
{
    uint32_t words[SUNXI_MMC_FIFO_WORDS], s;
    if (!rd(h, 0x3c, &h->result.status)) return 0;
    s = h->result.status;
    unsigned level = SUNXI_SMHC_STATUS_FIFO_LEVEL(s), n;
    size_t left = (h->data_bytes - h->position) / 4;
    if (h->command & WRITE)
        n = (s & FIFO_FULL) || level >= SUNXI_SMHC_FIFO_DEPTH ? 0 : SUNXI_SMHC_FIFO_DEPTH - level;
    else n = (s & FIFO_EMPTY) ? 0 : level ? level : 1;
    if (n > left) n = (unsigned)left;
    if (n > SUNXI_MMC_FIFO_WORDS) n = SUNXI_MMC_FIFO_WORDS;
    if (!n) return 1;
    if (h->command & WRITE) {
        for (unsigned i = 0; i < n; ++i) {
            const uint8_t *p = h->data + h->position + 4 * i;
            words[i] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        }
        h->result.submitted += 4 * n;
        if (h->config.ops.write_fifo(h->config.context, words, n) != 1) { fault(h, SUNXI_MMC_IO); return 0; }
        h->position += 4 * n;
    } else {
        if (h->config.ops.read_fifo(h->config.context, words, n) != 1) { fault(h, SUNXI_MMC_IO); return 0; }
        for (unsigned i = 0; i < n; ++i)
            for (unsigned b = 0; b < 4; ++b) h->data[h->position++] = (uint8_t)(words[i] >> (8 * b));
        if (h->protocol == P_READ) h->result.received = h->position;
    }
    return 1;
}
static int command_step(struct sunxi_mmc *h)
{
    uint32_t v;
    switch (h->command_step) {
    case 0:
        if (!rd(h, 0x18, &v)) return 0;
        if (v & START) { fault(h, SUNXI_MMC_CONTROLLER); return 0; }
        if (!rd(h, 0x3c, &h->result.status)) return 0;
        if (h->result.status & BUSY) return 0;
        if (!interrupts(h)) return 0;
        /* Acknowledge stale command/data flags only; leave card-detect/SDIO. */
        if (!wr(h, 0x38, h->result.interrupts & 0xffffu)) return 0;
        h->seen = 0;
        if (h->data_bytes && !wr(h, 0, START | 2u)) return 0;
        h->command_step = 1; return 0;
    case 1:
        if (h->data_bytes) {
            if (!rd(h, 0, &v) || (v & 2u)) return 0;
            if ((v & (START | (1u << 5) | 7u)) != START) { fault(h, SUNXI_MMC_CONTROLLER); return 0; }
            uint32_t block = h->data_bytes < 512 ? (uint32_t)h->data_bytes : 512;
            if (!wr(h, 0x10, block) || !wr(h, 0x14, (uint32_t)h->data_bytes)) return 0;
            /* SDR104 read threshold: start a block only with room for all of
             * it, sized to the block as the board kernel's thld_ctl does. */
            if (UHS_ON && h->card.timing == 3 && !(h->command & WRITE)) {
                uint32_t t;
                if (!rd(h, 0x100, &t) || !wr(h, 0x100, (t & ~0x1fff0001u) | (block << 16) | 1u)) return 0;
            }
        }
        if ((h->command & AUTO) && !wr(h, 0x58, 0)) return 0;
        if (!wr(h, 0x1c, h->argument) || !fence(h)) return 0;
        if(h->command&WRITE)h->result.write_started=1;
        if(!wr(h, 0x18, h->command))return 0;
        h->command_step = 2; return 0;
    case 2:
        if (!interrupts(h)) return 0;
        if((h->command&WRITE)&&(h->seen&DTC)&&h->position<h->data_bytes) {
            fault(h,SUNXI_MMC_CONTROLLER);return 0;
        }
        if ((h->seen & CC) && !h->response_seen) {
            if (h->command & LONG) {
                for (unsigned i = 0; i < 4; ++i)
                    if (!rd(h, 0x2c - 4 * i, &h->response[i])) return 0;
            } else if (h->command & RESP) {
                if (!rd(h, 0x20, &h->response[0])) return 0;
            }
            h->response_seen = 1;
            if (h->data_bytes) {
                h->result.response = h->response[0];
                if (h->response[0] & CARD_ERRORS) { fault(h, SUNXI_MMC_CARD); return 0; }
            }
        }
        if (h->position < h->data_bytes && (!(h->command & WRITE) || h->response_seen) &&
            !fifo_step(h)) return 0;
        v = CC | (h->data_bytes ? DTC : 0) | ((h->command & AUTO) ? ACD : 0);
        if ((h->seen & v) != v || h->position != h->data_bytes) return 0;
        h->command_step = 3; return 0;
    case 3:
        if (!rd(h, 0x18, &v) || (v & START)) return 0;
        /* After the CMD11 response the card holds DAT0 low by design. */
        if (!rd(h, 0x3c, &h->result.status) ||
            (h->result.status & BUSY & ~(UHS_ON && h->protocol == P_SD_VOLTAGE ? 1u << 9 : 0u))) return 0;
        if (h->data_bytes && !(h->result.status & FIFO_EMPTY)) { fault(h, SUNXI_MMC_CONTROLLER); return 0; }
        if (!interrupts(h)) return 0;
        if (h->command & AUTO) {
            if (!rd(h, 0x24, &h->result.stop_response)) return 0;
            /* SD 4.10 section 4.3.3 and JESD84-B51 section 6.8.3 let a card
             * report OUT_OF_RANGE after an open-ended CMD18 reaches the last
             * user-area sector; the pinned Linux block driver ignores it. */
            if (h->result.stop_response & CARD_ERRORS & ~(h->read_tail ? OUT_OF_RANGE : 0)) {
                fault(h, SUNXI_MMC_CARD); return 0;
            }
        }
        return 1;
    default: fault(h, SUNXI_MMC_CONTROLLER); return 0;
    }
}
static int card_status(struct sunxi_mmc *h, uint32_t ignored)
{
    h->result.response = h->response[0];
    if (h->response[0] & CARD_ERRORS & ~ignored) { fault(h, SUNXI_MMC_CARD); return 0; }
    return 1;
}
static int card_response(struct sunxi_mmc *h) { return card_status(h, 0); }
static void ready_status(struct sunxi_mmc *h, unsigned next)
{ queue(h, next, 13u | RESP | CRC | WAIT, (uint32_t)h->card.rca << 16, 0, 0); }
static int ready_except(struct sunxi_mmc *h, uint32_t ignored)
{
    if (!card_status(h, ignored)) return 0;
    if ((h->response[0] & 0x1f00u) != TRAN_READY) { fault(h, SUNXI_MMC_CARD); return 0; }
    return 1;
}
static int ready(struct sunxi_mmc *h) { return ready_except(h, 0); }
static void next_write(struct sunxi_mmc *h)
{
    uint64_t first=h->write_request.first_sector+h->result.confirmed_blocks;
    uint32_t arg=(uint32_t)(h->card.sector_addressed?first:first*512u);
    queue(h,P_WRITE,24u|RESP|CRC|DATA|WRITE|WAIT,arg,
          h->write_payload+h->result.confirmed_blocks*512u,512);
}
/* SD v2 capability data is read at the admitted identification clock. No
 * capability probe failure permits a mode fallback or repeated switch command. */
static void sd_modes(struct sunxi_mmc *h)
{
    if(!h->config.sd_profile){h->state=SUNXI_MMC_READY;return;}
    queue(h,P_SD_SCR_APP,55u|RESP|CRC,(uint32_t)h->card.rca<<16,0,0);
}
static int sd_rate(struct sunxi_mmc *h)
{
    static const unsigned units[]={100000,1000000,10000000,100000000};
    static const unsigned values[]={0,10,12,13,15,20,25,30,35,40,45,50,55,60,70,80};
    unsigned speed=bits(h->card.csd,96,8),u=speed&7,m=(speed>>3)&15;
    if((speed&128)||u>3||!values[m])return 0;
    uint32_t cap=(uint32_t)((uint64_t)units[u]*values[m]/10);
    if(cap>25000000)cap=25000000;
    unsigned divisor=h->config.sd_profile>=SUNXI_MMC_SD_HIGH_SPEED4?4:
        (unsigned)((UINT64_C(400000000)+2u*cap-1)/(2u*cap));
    if(divisor<4||divisor>32||(h->config.sd_profile==SUNXI_MMC_SD_DEFAULT4&&divisor<8))return 0;
    h->card.module_divisor=divisor;return 1;
}
static void hs_check(struct sunxi_mmc *h)
{queue(h,P_SD_SWITCH_CHECK,6u|RESP|CRC|DATA|WAIT,0x00fffff1,h->card.switch_status,64);}
static void tune_next(struct sunxi_mmc *h)
{queue(h,P_SD_TUNE,19u|RESP|CRC|DATA|WAIT,0,h->tuning_block,64);}
/* SDR104 timing, from the board kernel's v4p6x method that its tuning selects:
 * old sample mode (NTSR MODE_SEL 0), 180-degree command and data drive and the
 * software sample delay; each read also sets the read threshold. TMOUT bounds
 * a failed CMD19. Writing restores the set after a controller reset; reading
 * verifies it. */
static int uhs_registers(struct sunxi_mmc *h,unsigned write)
{
    uint32_t v,delay=0x80u|h->card.sample_delay;
    if(write)return rd(h,0x140,&v)&&wr(h,0x140,v|(3u<<16))&&rd(h,0x5c,&v)&&wr(h,0x5c,v&~(START|0x331u))&&
        wr(h,0x144,delay)&&wr(h,8,h->tuning?TUNE_TIMEOUT:UINT32_MAX)&&wr(h,0xc,1)&&wr(h,4,1u<<16);
    if(!rd(h,0x140,&v))return 0;
    if(((v>>16)&3)!=3)goto bad;
    if(!rd(h,0x5c,&v))return 0;
    if(v&(START|0x331u))goto bad;
    if(!rd(h,0x144,&v))return 0;
    if((v&0xbfu)!=delay)goto bad;
    if(!rd(h,0xc,&v))return 0;
    if(v!=1)goto bad;
    if(!rd(h,4,&v))return 0;
    if(v!=(1u<<16))goto bad;
    return 1;
bad:fault(h,SUNXI_MMC_CONTROLLER);return 0;
}
static int set_delay(struct sunxi_mmc *h,unsigned d)
{
    uint32_t v;h->card.sample_delay=(uint8_t)d;
    if(!wr(h,0x144,0x80u|d)||!rd(h,0x144,&v))return 0;
    if((v&0xbfu)!=(0x80u|d)){fault(h,SUNXI_MMC_CONTROLLER);return 0;}
    return 1;
}
/* After every delay, choose the first longest run of passing delays and its
 * midpoint, as the board kernel's get_best_sdly does. A run shorter than the
 * vendor threshold returns the card to SDR25 at 50 MHz without a power cycle. */
static void tune_advance(struct sunxi_mmc *h,unsigned passed)
{
    if(passed)h->card.tuning_map|=UINT64_C(1)<<h->tune_delay;
    h->tune_repeat=0;
    if(++h->tune_delay<SUNXI_MMC_TUNING_DELAYS){if(set_delay(h,h->tune_delay))tune_next(h);return;}
    unsigned best=0,start=0,run=0;
    for(unsigned d=0;d<SUNXI_MMC_TUNING_DELAYS;++d){
        if((h->card.tuning_map>>d)&1u){if(++run>best){best=run;start=d+1-run;}}else run=0;
    }
    h->tuning=0;h->card.tuning_start=(uint8_t)start;h->card.tuning_width=(uint8_t)best;
    if(best>=TUNE_WINDOW){
        if(!set_delay(h,start+best/2)||!wr(h,8,UINT32_MAX))return;
        h->card.uhs_outcome=SUNXI_MMC_UHS_SDR104;ready_status(h,P_SD_MODE_STATUS);return;
    }
    h->card.uhs_outcome=SUNXI_MMC_UHS_NO_WINDOW;h->card.timing=1;h->card.module_divisor=4;
    h->card.sample_delay=0;h->after_clock=1;h->protocol=P_SD_CLOCK;h->sd_ios=0;
}
static void sd_clock(struct sunxi_mmc *h)
{
    uint32_t v;unsigned hs=h->card.timing==1,uhs=UHS_ON&&h->card.timing==3;
    switch(h->sd_ios){
    case 0:
        if(!rd(h,0x18,&v)||(v&START)||!rd(h,0x3c,&v)||(v&BUSY))return;
        if(!interrupts(h)||!wr(h,4,0)||!fence(h)||!wr(h,0x18,UPDATE))return;
        ++h->sd_ios;return;
    case 1:
        if(!rd(h,0x18,&v)||(v&START))return;
        if(!interrupts(h)||!rd(h,4,&v))return;
        if(v){fault(h,SUNXI_MMC_CONTROLLER);return;}
        /* Fresh time/ownership surround the provider's bounded rate change. */
        if(!time_ok(h)||h->config.ops.retained(h->config.context,h->config.token)!=1){fault(h,SUNXI_MMC_ADMISSION);return;}
        if(h->config.ops.sd_module(h->config.context,h->config.token,h->card.module_divisor)!=1){fault(h,SUNXI_MMC_IO);return;}
        if(!time_ok(h)||h->config.ops.retained(h->config.context,h->config.token)!=1){fault(h,SUNXI_MMC_ADMISSION);return;}
        ++h->sd_ios;return;
    case 2:
        if(uhs){
            h->tuning=1;h->card.sample_delay=0;
            if(!uhs_registers(h,1)||!fence(h)||!wr(h,0x18,UPDATE))return;
            ++h->sd_ios;return;
        }
        /* UHS fallback at 1.8 V leaves tuning with SDR25's fixed phases and
         * without the read threshold, which the vendor uses only in SDR50/104. */
        if(UHS_ON&&h->config.sd_profile==UHS&&(!wr(h,0x144,0x80u)||!wr(h,8,UINT32_MAX)||
           !rd(h,0x100,&v)||!wr(h,0x100,v&~0x1fff0001u)))return;
        /* Pinned v5p3x defaults: CMD drive180, data drive90/180 and
         * CMD/data sample phases0/1 for <=26MHz/default and <=52MHz/HS. */
        if(!rd(h,0x140,&v)||!wr(h,0x140,(v&~(3u<<16))|((hs?3u:1u)<<16))||
           !rd(h,0x5c,&v)||!wr(h,0x5c,(v&~(START|0x331u))|START|(hs?0x110u:0))||
           !wr(h,4,1u<<16)||!fence(h)||!wr(h,0x18,UPDATE))return;
        ++h->sd_ios;return;
    case 3:
        if(!rd(h,0x18,&v)||(v&START))return;
        if(!interrupts(h))return;
        if(uhs){if(!uhs_registers(h,0))return;}
        else {
            if(!rd(h,4,&v))return;
            if(v!=(1u<<16)){fault(h,SUNXI_MMC_CONTROLLER);return;}
            if(!rd(h,0xc,&v)||v!=1||!rd(h,0x140,&v)||((v>>16)&3)!=(hs?3u:1u)||
               !rd(h,0x5c,&v)||(v&(START|0x331u))!=(START|(hs?0x110u:0))){fault(h,SUNXI_MMC_CONTROLLER);return;}
        }
        {
            uint32_t parent=UHS_ON&&(h->card.module_divisor&SUNXI_MMC_MODULE_300M)?300000000u:400000000u;
            unsigned divisor=UHS_ON?h->card.module_divisor&0xffu:h->card.module_divisor;
            h->card.module_hz=parent/divisor;h->card.card_hz=parent/(2*divisor);
        }
        if(uhs){h->tune_delay=0;h->tune_repeat=0;h->card.tuning_map=0;tune_next(h);return;}
        if(UHS_ON&&h->config.sd_profile==UHS){
            if(!rd(h,0x100,&v))return;
            if(v&0x1fff0001u){fault(h,SUNXI_MMC_CONTROLLER);return;}
        }
        if(UHS_ON&&h->after_clock){hs_check(h);return;}
        ready_status(h,P_SD_MODE_STATUS);return;
    default:fault(h,SUNXI_MMC_CONTROLLER);return;
    }
}
/* CMD11 host sequence: pinned Linux mmc_set_uhs_voltage and
 * mmc_host_set_uhs_voltage with the board kernel's sunxi-mmc callbacks.
 * Steps: check DAT held low at least 1 ms after the response, stop the card
 * clock with the voltage-switch update, select 1.8 V for the pin bank and wait
 * for it, keep the clock stopped 10 ms, restart it, then require DAT3 high and
 * the controller's switch-done bit within 10 ms after a further 1 ms. */
static void sd_signal(struct sunxi_mmc *h)
{
    uint32_t v,r;unsigned state=0;
    switch(h->sd_sig){
    case 0:
        if(!rd(h,0x3c,&h->result.status))return;
        if(h->result.status&DAT3){fault(h,SUNXI_MMC_SIGNAL);return;}
        if(!interrupts(h)||!wr(h,4,MASK_DAT0)||!fence(h)||!wr(h,0x18,UPDATE|VSW))return;
        ++h->sd_sig;return;
    case 1:
        if(!rd(h,0x18,&v)||(v&START))return;
        if(!interrupts(h)||!wr(h,0x38,h->result.interrupts&0xffffu)||!wr(h,4,0)||!rd(h,4,&v))return;
        if(v){fault(h,SUNXI_MMC_CONTROLLER);return;}
        if(!time_ok(h)||h->config.ops.retained(h->config.context,h->config.token)!=1){fault(h,SUNXI_MMC_ADMISSION);return;}
        if(h->config.ops.sd_signal(h->config.context,h->config.token,SUNXI_MMC_SIGNAL_SET,&state)!=1){fault(h,SUNXI_MMC_IO);return;}
        if(!time_ok(h))return;
        h->signal_us=h->last_us;++h->sd_sig;return;
    case 2:
        if(h->config.ops.sd_signal(h->config.context,h->config.token,SUNXI_MMC_SIGNAL_POLL,&state)!=1||state>1){fault(h,SUNXI_MMC_IO);return;}
        if(!time_ok(h))return;
        if(!state){
            /* The board kernel's PF switch polls every 100 us for 10 ms. */
            if(h->last_us-h->signal_us>=10000){fault(h,SUNXI_MMC_SIGNAL);return;}
            defer_command(h,100);return;
        }
        h->card.signal_1v8=1;defer_command(h,10000);++h->sd_sig;return;
    case 3:
        if(!wr(h,4,(1u<<16)|MASK_DAT0)||!fence(h)||!wr(h,0x18,UPDATE|VSW))return;
        ++h->sd_sig;return;
    case 4:
        if(!rd(h,0x18,&v)||(v&START))return;
        /* Clear the update's flags; keep an early switch-done bit. */
        if(!interrupts(h)||!wr(h,0x38,h->result.interrupts&0xffffu&~VSD)||!wr(h,4,1u<<16)||!rd(h,4,&v))return;
        if(v!=(1u<<16)){fault(h,SUNXI_MMC_CONTROLLER);return;}
        defer_command(h,1000);h->signal_us=h->last_us;++h->sd_sig;return;
    case 5:
        if(!rd(h,0x3c,&h->result.status)||!rd(h,0x38,&r))return;
        h->result.interrupts=r;
        if(r&ERRORS&~VSD){fault(h,SUNXI_MMC_CONTROLLER);return;}
        if((h->result.status&DAT3)&&(r&VSD)){
            if(!wr(h,0x38,r&0xffffu))return;
            h->signal_window=0;queue(h,P_CID,2u|RESP|LONG|CRC,0,0,0);return;
        }
        if(!time_ok(h))return;
        if(h->last_us-h->signal_us>=10000){fault(h,SUNXI_MMC_SIGNAL);return;}
        defer_command(h,100);return;
    default:fault(h,SUNXI_MMC_CONTROLLER);return;
    }
}
/* After a failed CMD19: wait for the command and data state machines, reset
 * the FIFO and state machines, restore and verify the SDR104 registers, then
 * record the delay as failed. */
static void tune_recover(struct sunxi_mmc *h)
{
    uint32_t v;
    switch(h->tune_recover){
    case 0:
        if(!time_ok(h))return;
        h->protocol=P_SD_TUNE_RECOVER;h->tune_failed=0;h->signal_us=h->last_us;++h->tune_recover;return;
    case 1:
        if(!rd(h,0x18,&v)||!rd(h,0x3c,&h->result.status))return;
        if((v&START)||(h->result.status&BUSY)){
            if(!time_ok(h))return;
            if(h->last_us-h->signal_us>=10000){fault(h,SUNXI_MMC_TIMEOUT);return;}
            defer_command(h,100);return;
        }
        if(!rd(h,0,&v)||!wr(h,0,v|7u))return;
        ++h->tune_recover;return;
    case 2:
        if(!rd(h,0,&v)||(v&7u))return;
        if(!(v&START)){fault(h,SUNXI_MMC_CONTROLLER);return;}
        if(!rd(h,0x38,&v)||!wr(h,0x38,v&0xffffu)||!uhs_registers(h,1)||!fence(h)||!wr(h,0x18,UPDATE))return;
        ++h->tune_recover;return;
    case 3:
        if(!rd(h,0x18,&v)||(v&START)||!uhs_registers(h,0))return;
        h->tune_recover=0;h->protocol=P_SD_TUNE;tune_advance(h,0);return;
    default:fault(h,SUNXI_MMC_CONTROLLER);return;
    }
}
static void protocol(struct sunxi_mmc *h)
{
    uint32_t r = h->response[0], voltage = h->config.voltage_mask;
    unsigned mmc = h->config.kind == SUNXI_MMC_EMMC;
    switch (h->protocol) {
    case P_IDLE:
        queue(h, mmc ? P_MMC_QUERY : P_SD_IF, mmc ? (1u | RESP) : (8u | RESP | CRC),
              mmc ? 0 : 0x1aa, 0, 0);
        defer_command(h, 2000); return;
    case P_SD_IF:
        if (r != 0x1aa) { fault(h, SUNXI_MMC_UNSUPPORTED); return; }
        if (UHS_ON && h->config.sd_profile == UHS) {
            /* The first ACMD41 argument fixes S18R for every repetition. */
            unsigned caps = 0;
            if (h->config.ops.sd_signal(h->config.context, h->config.token,
                                        SUNXI_MMC_SIGNAL_QUERY, &caps) != 1 || (caps & ~3u)) {
                fault(h, SUNXI_MMC_IO); return;
            }
            h->card.uhs_caps = (uint8_t)caps; h->s18r = !!(caps & SUNXI_MMC_SIGNAL_1V8);
        }
        queue(h, P_SD_APP, 55u | RESP | CRC, 0, 0, 0); return;
    case P_SD_APP:
        if (!card_response(h)) return;
        if (!(r & (1u << 5))) { fault(h, SUNXI_MMC_CARD); return; }
        queue(h, P_SD_OCR, 41u | RESP, voltage | (1u << 30) | (h->s18r ? S18R : 0), 0, 0); return;
    case P_MMC_QUERY:
        if (!(r & voltage) || (((r >> 29) & 3u) != 0 && ((r >> 29) & 3u) != 2)) {
            fault(h, SUNXI_MMC_UNSUPPORTED); return;
        }
        queue(h, P_MMC_OCR, 1u | RESP, (r & voltage) | (1u << 30), 0, 0); return;
    case P_SD_OCR:
    case P_MMC_OCR:
        if (!(r & voltage) || (!mmc && !h->s18r && (r & S18R)) ||
            (mmc && ((r >> 29) & 3u) != 0 && ((r >> 29) & 3u) != 2)) {
            fault(h, SUNXI_MMC_UNSUPPORTED); return;
        }
        if (!(r & START)) {
            if (!a733_deadline_after(h->last_us, 1000, &h->retry_us)) { fault(h, SUNXI_MMC_TIME); return; }
            queue(h, mmc ? P_MMC_OCR : P_SD_APP, mmc ? (1u | RESP) : (55u | RESP | CRC),
                  mmc ? ((r & voltage) | (1u << 30)) : 0, 0, 0); return;
        }
        h->card.ocr = r; h->card.sector_addressed = !!(r & (1u << 30));
        /* S18A requests CMD11 before CMD2. Like the pinned Linux core, the
         * switch does not also require CCS. */
        if (UHS_ON && !mmc && h->s18r && (r & S18R)) {
            h->signal_window = 1;
            queue(h, P_SD_VOLTAGE, 11u | RESP | CRC | VSW, 0, 0, 0); return;
        }
        if (!mmc && h->config.sd_profile == UHS)
            h->card.uhs_outcome = h->s18r ? SUNXI_MMC_UHS_DECLINED : SUNXI_MMC_UHS_HOST;
        queue(h, P_CID, 2u | RESP | LONG | CRC, 0, 0, 0); return;
    case P_SD_VOLTAGE:
        if (!UHS_ON || !card_response(h)) { fault(h, SUNXI_MMC_CARD); return; }
        h->protocol = P_SD_SIGNAL; h->sd_sig = 0; defer_command(h, 1000); return;
    case P_CID:
        for (unsigned i = 0; i < 4; ++i) h->card.cid[i] = h->response[i];
        queue(h, P_RCA, 3u | RESP | CRC, mmc ? (1u << 16) : 0, 0, 0); return;
    case P_RCA:
        if (mmc) {
            if (!card_response(h)) return;
            h->card.rca = 1;
        } else {
            if ((r & 0xe000u) || !(r >> 16)) { fault(h, SUNXI_MMC_CARD); return; }
            h->card.rca = (uint16_t)(r >> 16);
        }
        queue(h, P_CSD, 9u | RESP | LONG | CRC, (uint32_t)h->card.rca << 16, 0, 0); return;
    case P_CSD:
        for (unsigned i = 0; i < 4; ++i) h->card.csd[i] = h->response[i];
        if ((!mmc && !sunxi_mmc_capacity(SUNXI_MMC_SD, h->card.csd, h->card.ocr, 0, &h->card.sectors)) ||
            (mmc && (!bits(h->card.csd, 126, 2) || bits(h->card.csd, 122, 4) != 4))) {
            fault(h, SUNXI_MMC_UNSUPPORTED); return;
        }
        queue(h, P_SELECT, 7u | RESP | CRC | WAIT, (uint32_t)h->card.rca << 16, 0, 0); return;
    case P_SELECT:
        if (!card_response(h)) return;
        ready_status(h, P_STATUS); return;
    case P_STATUS:
        if (!ready(h)) return;
        if (!h->card.sector_addressed) {
            queue(h, P_BLOCKLEN, 16u | RESP | CRC | WAIT, 512, 0, 0); return;
        }
        if (mmc) queue(h, P_EXT, 8u | RESP | CRC | DATA | WAIT, 0, h->ext_csd, 512);
        else {h->card.bus_width=1;h->card.module_hz=h->config.module_hz;h->card.card_hz=h->config.module_hz/2;sd_modes(h);}
        return;
    case P_BLOCKLEN:
        if (!card_response(h)) return;
        if (mmc) queue(h, P_EXT, 8u | RESP | CRC | DATA | WAIT, 0, h->ext_csd, 512);
        else ready_status(h, P_EXT_STATUS);
        return;
    case P_EXT:
        if (!card_response(h)) return;
        if (!sunxi_mmc_capacity(SUNXI_MMC_EMMC, h->card.csd, h->card.ocr, h->ext_csd, &h->card.sectors)) {
            fault(h, SUNXI_MMC_UNSUPPORTED); return;
        }
        h->card.ext_revision = h->ext_csd[192]; ready_status(h, P_EXT_STATUS); return;
    case P_EXT_STATUS:
        if (ready(h)) {h->card.bus_width=1;h->card.module_hz=h->config.module_hz;h->card.card_hz=h->config.module_hz/2;if(mmc)h->state=SUNXI_MMC_READY;else sd_modes(h);}
        return;
    case P_SD_SCR_APP:
        if(!card_response(h))return;
        if(!(r&(1u<<5))){fault(h,SUNXI_MMC_CARD);return;}
        queue(h,P_SD_SCR,51u|RESP|CRC|DATA|WAIT,0,h->card.scr,8);return;
    case P_SD_SCR:
        if(!card_response(h))return;
        if((h->card.scr[0]>>4)!=0||(h->card.scr[0]&15)>2||(h->card.scr[1]&5)!=5){fault(h,SUNXI_MMC_UNSUPPORTED);return;}
        queue(h,P_SD_WIDTH_APP,55u|RESP|CRC,(uint32_t)h->card.rca<<16,0,0);return;
    case P_SD_WIDTH_APP:
        if(!card_response(h))return;
        if(!(r&(1u<<5))){fault(h,SUNXI_MMC_CARD);return;}
        queue(h,P_SD_WIDTH,6u|RESP|CRC|WAIT,2,0,0);return;
    case P_SD_WIDTH:
        if(!card_response(h)||!wr(h,0xc,1)||!rd(h,0xc,&r))return;
        if(r!=1){fault(h,SUNXI_MMC_CONTROLLER);return;}h->card.bus_width=4;
        if(h->config.sd_profile>=SUNXI_MMC_SD_HIGH_SPEED4){
            if((h->card.scr[0]&15)<1||!(bits(h->card.csd,84,12)&(1u<<10))){fault(h,SUNXI_MMC_UNSUPPORTED);return;}
            /* At 1.8 V, ask for SDR104 (function 3); SDR25 is the fallback. */
            if(UHS_ON&&h->card.signal_1v8){queue(h,P_SD_UHS_CHECK,6u|RESP|CRC|DATA|WAIT,0x00fffff3,h->card.switch_status,64);return;}
            hs_check(h);return;
        }
        if(!sd_rate(h)){fault(h,SUNXI_MMC_UNSUPPORTED);return;}
        h->protocol=P_SD_CLOCK;return;
    case P_SD_SWITCH_CHECK:
        if(!card_response(h))return;
        if(!(h->card.switch_status[13]&2)||(h->card.switch_status[29]&2)){fault(h,SUNXI_MMC_UNSUPPORTED);return;}
        queue(h,P_SD_SWITCH_SET,6u|RESP|CRC|DATA|WAIT,0x80fffff1,h->card.switch_status,64);return;
    case P_SD_SWITCH_SET:
        if(!card_response(h))return;
        if((h->card.switch_status[16]&15)!=1||!(h->card.switch_status[13]&2)||(h->card.switch_status[29]&2)){fault(h,SUNXI_MMC_CARD);return;}
        h->card.timing=1;
        if(UHS_ON&&h->after_clock){h->after_clock=0;ready_status(h,P_SD_MODE_STATUS);return;}
        if(!sd_rate(h)){fault(h,SUNXI_MMC_UNSUPPORTED);return;}
        h->protocol=P_SD_CLOCK;h->sd_ios=0;return;
    case P_SD_UHS_CHECK:
        /* Status byte 13 bit 3: SDR104 supported; byte 29 bit 3: busy;
         * byte 16 low nibble: the function the set would select. */
        if(!UHS_ON){fault(h,SUNXI_MMC_CARD);return;}
        if(!card_response(h))return;
        if((h->card.switch_status[13]&8)&&!(h->card.switch_status[29]&8)&&(h->card.switch_status[16]&15)==3){
            queue(h,P_SD_UHS_SET,6u|RESP|CRC|DATA|WAIT,0x80fffff3,h->card.switch_status,64);return;
        }
        h->card.uhs_outcome=SUNXI_MMC_UHS_NO_SDR104;hs_check(h);return;
    case P_SD_UHS_SET:
        if(!UHS_ON){fault(h,SUNXI_MMC_CARD);return;}
        if(!card_response(h))return;
        if((h->card.switch_status[16]&15)!=3){h->card.uhs_outcome=SUNXI_MMC_UHS_NO_SDR104;hs_check(h);return;}
        h->card.timing=3;
        h->card.module_divisor=(h->card.uhs_caps&SUNXI_MMC_SIGNAL_300M)?(SUNXI_MMC_MODULE_300M|1u):2u;
        h->protocol=P_SD_CLOCK;h->sd_ios=0;return;
    case P_SD_TUNE:
        if(!UHS_ON){fault(h,SUNXI_MMC_CARD);return;}
        if(!card_response(h))return;
        for(unsigned i=0;i<64;++i)if(h->tuning_block[i]!=tuning_pattern[i]){h->tune_failed=1;return;}
        if(++h->tune_repeat<TUNE_REPEATS){tune_next(h);return;}
        tune_advance(h,1);return;
    case P_SD_MODE_STATUS:
        if(ready(h))h->state=SUNXI_MMC_READY;return;
    case P_READ:
        if (card_response(h)) ready_status(h, P_READ_STATUS);
        return;
    case P_READ_STATUS:
        /* The same end-of-card rule covers a status bit not yet cleared. */
        if (ready_except(h, h->read_tail ? OUT_OF_RANGE : 0)) h->state = SUNXI_MMC_DONE;
        return;
    case P_WRITE_PRE:
        if(ready(h))next_write(h);
        return;
    case P_WRITE:
        if(card_response(h))ready_status(h,P_WRITE_STATUS);
        return;
    case P_WRITE_STATUS:
        if(!card_response(h))return;
        if((r&0x1f00u)==TRAN_READY) {
            ++h->result.confirmed_blocks;
            if(h->result.confirmed_blocks==h->write_request.blocks)h->state=SUNXI_MMC_DONE;
            else next_write(h);
        } else if(((r>>9)&15u)==7u||((r&0x1f00u)==0x800u)) {
            if(!a733_deadline_after(h->last_us,1000,&h->retry_us)){fault(h,SUNXI_MMC_TIME);return;}
            ready_status(h,P_WRITE_STATUS);
        } else fault(h,SUNXI_MMC_CARD);
        return;
    default: fault(h, SUNXI_MMC_CARD); return;
    }
}
void sunxi_mmc_poll(struct sunxi_mmc *h)
{
    if (!h || !h->initialized || h->entered ||
        (h->state != SUNXI_MMC_IDENTIFY && h->state != SUNXI_MMC_READING&&h->state!=SUNXI_MMC_WRITING)) return;
    h->entered = 1;
    if (!time_ok(h)) goto out;
    if (h->admitted && h->config.ops.retained(h->config.context, h->config.token) != 1) {
        fault(h, SUNXI_MMC_ADMISSION); goto out;
    }
    if(h->writing&&(!write_admitted(h)||!time_ok(h)))goto out;
    if (h->setup < 6) setup(h);
    else if(h->protocol==P_SD_CLOCK)sd_clock(h);
    else if(h->last_us<h->retry_us){}
    else if(UHS_ON&&h->protocol==P_SD_SIGNAL)sd_signal(h);
    else if(UHS_ON&&(h->tune_failed||h->protocol==P_SD_TUNE_RECOVER))tune_recover(h);
    else if(command_step(h))protocol(h);
    if (h->state != SUNXI_MMC_FAULT) (void)time_ok(h);
out:
    h->entered = 0;
}
int sunxi_mmc_take(struct sunxi_mmc *h, struct sunxi_mmc_result *out)
{
    if (!h || !h->initialized || h->entered || !a733_span_excl(out, sizeof(*out)) ||
        a733_extents_overlap_prechecked((uintptr_t)h, sizeof(*h), (uintptr_t)out, sizeof(*out)) ||
        (h->borrowed &&
            a733_extents_overlap_prechecked((uintptr_t)h->borrowed, h->borrowed_bytes, (uintptr_t)out, sizeof(*out))) ||
        (h->state != SUNXI_MMC_DONE && h->state != SUNXI_MMC_FAULT)) return 0;
    *out = h->result;
    if (h->state == SUNXI_MMC_DONE) {
        h->state = SUNXI_MMC_READY; h->borrowed = 0; h->borrowed_bytes = 0; h->data = 0; h->data_bytes = 0;
        h->writing=0;
    }
    return 1;
}
