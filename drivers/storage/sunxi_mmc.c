/* Native storage controller. Source anchors: docs/mmc.md and docs/mmc-write.md. */
#include "sunxi_mmc.h"

#define START (1u << 31)
#define UPDATE (START | (1u << 21) | (1u << 13))
#define RESP (1u << 6)
#define LONG (1u << 7)
#define CRC (1u << 8)
#define DATA (1u << 9)
#define WRITE (1u << 10)
#define AUTO (1u << 12)
#define WAIT (1u << 13)
#define CC (1u << 2)
#define DTC (1u << 3)
#define ACD (1u << 14)
#define ERRORS (0xbfc2u | (1u << 18) | (1u << 31))
/* Includes locked, switch and SD authentication failures. ECC-disabled is
 * informational; erase-reset is also refused in this bounded profile. */
#define CARD_ERRORS 0xffffa088u
#define TRAN_READY 0x900u
#define BUSY ((1u << 10) | (1u << 9) | (15u << 4))
#define FIFO_EMPTY (1u << 2)
#define FIFO_FULL (1u << 3)

enum { P_IDLE, P_SD_IF, P_SD_APP, P_SD_OCR, P_MMC_QUERY, P_MMC_OCR,
       P_CID, P_RCA, P_CSD, P_SELECT, P_STATUS, P_BLOCKLEN, P_EXT,
       P_EXT_STATUS, P_READ, P_READ_STATUS, P_WRITE_PRE, P_WRITE, P_WRITE_STATUS };

static int span(const void *p, size_t n)
{ return p && n <= UINTPTR_MAX - (uintptr_t)p; }
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{ return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an; }
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
    if (n < h->last_us) { fault(h, SUNXI_MMC_TIME); return 0; }
    h->last_us = n;
    if (n >= h->deadline_us) { fault(h, SUNXI_MMC_TIMEOUT); return 0; }
    return 1;
}
static void defer_command(struct sunxi_mmc *h, uint64_t delay_us)
{
    /* Sample after the observed clock/command completion, not before MMIO. */
    if (!time_ok(h)) return;
    if (h->last_us > UINT64_MAX - delay_us) { fault(h, SUNXI_MMC_TIME); return; }
    h->retry_us = h->last_us + delay_us;
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
    if (!span(c, 16) || !span(out, sizeof(*out)) || overlap(c, 16, out, sizeof(*out)) ||
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
        if (!span(ext, 512) || overlap(ext, 512, out, sizeof(*out)) || !structure ||
            bits(c, 122, 4) != 4 || ext[194] > 2 || ext[192] > 8 ||
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
    if (!span(h, sizeof(*h)) || !span(c, sizeof(*c)) || overlap(h, sizeof(*h), c, sizeof(*c)) ||
        h->initialized || !c->ops.read32 || !c->ops.write32 || !c->ops.barrier ||
        !c->ops.now_us || !c->ops.admit || !c->ops.retained || !c->token ||
        (c->kind != SUNXI_MMC_SD && c->kind != SUNXI_MMC_EMMC) ||
        c->module_hz < 200000 || c->module_hz > 800000 || (c->module_hz & 1u) ||
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
        bytes != blocks * 512u || !span(buffer, bytes) || overlap(h, sizeof(*h), buffer, bytes) ||
        first >= h->card.sectors || blocks > h->card.sectors - first) return 0;
    uint64_t last = first + blocks - 1;
    if ((h->card.sector_addressed && last > UINT32_MAX) ||
        (!h->card.sector_addressed && last > UINT32_MAX / 512u)) return 0;
    uint32_t arg = (uint32_t)(h->card.sector_addressed ? first : first * 512);
    h->result = (struct sunxi_mmc_result){0}; h->deadline_us = deadline;
    h->borrowed = buffer; h->borrowed_bytes = bytes; h->state = SUNXI_MMC_READING;
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
       !span(r,sizeof(*r))||!span(a,sizeof(*a))||!a->admit||!r->policy_token||
       overlap(h,sizeof(*h),r,sizeof(*r))||overlap(h,sizeof(*h),a,sizeof(*a))||
       !r->blocks||r->blocks>SUNXI_MMC_MAX_BLOCKS||r->bytes!=r->blocks*512u||
       !span(r->source,r->bytes)||overlap(h,sizeof(*h),r->source,r->bytes)||
       overlap(r,sizeof(*r),r->source,r->bytes)||overlap(a,sizeof(*a),r->source,r->bytes)||
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
    if (v & ERRORS) { fault(h, SUNXI_MMC_CONTROLLER); return 0; }
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
/* At most sixteen FIFO words per poll. Controller completion does not publish
 * data; the protocol subsequently confirms the card's ready/TRAN state. */
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
            if (!wr(h, 0x10, 512) || !wr(h, 0x14, (uint32_t)h->data_bytes)) return 0;
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
        if (h->data_bytes) {
            for (unsigned n = 0; n < 16 && h->position < h->data_bytes; ++n) {
                if((h->command&WRITE)&&!h->response_seen)break;
                if (!rd(h, 0x3c, &h->result.status)) return 0;
                if(h->command&WRITE) {
                    if(h->result.status&FIFO_FULL)break;
                    v=0;for(unsigned b=0;b<4;++b)v|=(uint32_t)h->data[h->position+b]<<(8*b);
                    h->result.submitted+=4;
                    if(!wr(h,0x200,v))return 0;
                    h->position+=4;
                } else {
                    if (h->result.status & FIFO_EMPTY) break;
                    if (!rd(h, 0x200, &v)) return 0;
                    for (unsigned b = 0; b < 4; ++b) h->data[h->position++] = (uint8_t)(v >> (8 * b));
                    if (h->protocol == P_READ) h->result.received = h->position;
                }
            }
        }
        v = CC | (h->data_bytes ? DTC : 0) | ((h->command & AUTO) ? ACD : 0);
        if ((h->seen & v) != v || h->position != h->data_bytes) return 0;
        h->command_step = 3; return 0;
    case 3:
        if (!rd(h, 0x18, &v) || (v & START)) return 0;
        if (!rd(h, 0x3c, &h->result.status) || (h->result.status & BUSY)) return 0;
        if (h->data_bytes && !(h->result.status & FIFO_EMPTY)) { fault(h, SUNXI_MMC_CONTROLLER); return 0; }
        if (!interrupts(h)) return 0;
        if (h->command & AUTO) {
            if (!rd(h, 0x24, &h->result.stop_response)) return 0;
            if (h->result.stop_response & CARD_ERRORS) { fault(h, SUNXI_MMC_CARD); return 0; }
        }
        return 1;
    default: fault(h, SUNXI_MMC_CONTROLLER); return 0;
    }
}
static int card_response(struct sunxi_mmc *h)
{
    h->result.response = h->response[0];
    if (h->response[0] & CARD_ERRORS) { fault(h, SUNXI_MMC_CARD); return 0; }
    return 1;
}
static void ready_status(struct sunxi_mmc *h, unsigned next)
{ queue(h, next, 13u | RESP | CRC | WAIT, (uint32_t)h->card.rca << 16, 0, 0); }
static int ready(struct sunxi_mmc *h)
{
    if (!card_response(h)) return 0;
    if ((h->response[0] & 0x1f00u) != TRAN_READY) { fault(h, SUNXI_MMC_CARD); return 0; }
    return 1;
}
static void next_write(struct sunxi_mmc *h)
{
    uint64_t first=h->write_request.first_sector+h->result.confirmed_blocks;
    uint32_t arg=(uint32_t)(h->card.sector_addressed?first:first*512u);
    queue(h,P_WRITE,24u|RESP|CRC|DATA|WRITE|WAIT,arg,
          h->write_payload+h->result.confirmed_blocks*512u,512);
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
        queue(h, P_SD_APP, 55u | RESP | CRC, 0, 0, 0); return;
    case P_SD_APP:
        if (!card_response(h)) return;
        if (!(r & (1u << 5))) { fault(h, SUNXI_MMC_CARD); return; }
        queue(h, P_SD_OCR, 41u | RESP, voltage | (1u << 30), 0, 0); return;
    case P_MMC_QUERY:
        if (!(r & voltage) || (((r >> 29) & 3u) != 0 && ((r >> 29) & 3u) != 2)) {
            fault(h, SUNXI_MMC_UNSUPPORTED); return;
        }
        queue(h, P_MMC_OCR, 1u | RESP, (r & voltage) | (1u << 30), 0, 0); return;
    case P_SD_OCR:
    case P_MMC_OCR:
        if (!(r & voltage) || (!mmc && (r & (1u << 24))) ||
            (mmc && ((r >> 29) & 3u) != 0 && ((r >> 29) & 3u) != 2)) {
            fault(h, SUNXI_MMC_UNSUPPORTED); return;
        }
        if (!(r & START)) {
            if (h->last_us > UINT64_MAX - 1000) { fault(h, SUNXI_MMC_TIME); return; }
            h->retry_us = h->last_us + 1000;
            queue(h, mmc ? P_MMC_OCR : P_SD_APP, mmc ? (1u | RESP) : (55u | RESP | CRC),
                  mmc ? ((r & voltage) | (1u << 30)) : 0, 0, 0); return;
        }
        h->card.ocr = r; h->card.sector_addressed = !!(r & (1u << 30));
        queue(h, P_CID, 2u | RESP | LONG | CRC, 0, 0, 0); return;
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
        else h->state = SUNXI_MMC_READY;
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
        if (ready(h)) h->state = SUNXI_MMC_READY;
        return;
    case P_READ:
        if (card_response(h)) ready_status(h, P_READ_STATUS);
        return;
    case P_READ_STATUS:
        if (ready(h)) h->state = SUNXI_MMC_DONE;
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
            if(h->last_us>UINT64_MAX-1000){fault(h,SUNXI_MMC_TIME);return;}
            h->retry_us=h->last_us+1000;ready_status(h,P_WRITE_STATUS);
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
    else if (h->last_us >= h->retry_us && command_step(h)) protocol(h);
    if (h->state != SUNXI_MMC_FAULT) (void)time_ok(h);
out:
    h->entered = 0;
}
int sunxi_mmc_take(struct sunxi_mmc *h, struct sunxi_mmc_result *out)
{
    if (!h || !h->initialized || h->entered || !span(out, sizeof(*out)) ||
        overlap(h, sizeof(*h), out, sizeof(*out)) ||
        (h->borrowed && overlap(h->borrowed, h->borrowed_bytes, out, sizeof(*out))) ||
        (h->state != SUNXI_MMC_DONE && h->state != SUNXI_MMC_FAULT)) return 0;
    *out = h->result;
    if (h->state == SUNXI_MMC_DONE) {
        h->state = SUNXI_MMC_READY; h->borrowed = 0; h->borrowed_bytes = 0; h->data = 0; h->data_bytes = 0;
        h->writing=0;
    }
    return 1;
}
