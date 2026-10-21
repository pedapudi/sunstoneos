/* Native bounded controller implementation. Register/protocol references and
 * source identities are documented in docs/sdio.md. */
#include "sunxi_sdio.h"
#include "sunxi_smhc.h"

#define GCTRL SUNXI_SMHC_GCTRL
#define CLKCR SUNXI_SMHC_CLKCR
#define TMOUT SUNXI_SMHC_TMOUT
#define WIDTH SUNXI_SMHC_WIDTH
#define BLKSZ SUNXI_SMHC_BLKSZ
#define BCNT SUNXI_SMHC_BCNT
#define CMD SUNXI_SMHC_CMD
#define ARG SUNXI_SMHC_ARG
#define RESP SUNXI_SMHC_RESP0
#define IMASK SUNXI_SMHC_IMASK
#define RINT SUNXI_SMHC_RINT
#define STATUS SUNXI_SMHC_STATUS
#define NTSR SUNXI_SMHC_NTSR
#define DMAC SUNXI_SMHC_DMAC
#define IDIE SUNXI_SMHC_IDIE
#define EDSD SUNXI_SMHC_EDSD
#define DRIVE SUNXI_SMHC_DRIVE
#define SAMPLE SUNXI_SMHC_SAMPLE
#define FIFO SUNXI_SMHC_FIFO
#define VERSION SUNXI_SMHC_VERSION
#define START SUNXI_SMHC_CMD_START
#define AHB (1u << 31) /* GCTRL CPU FIFO access. */
#define CMD_DONE SUNXI_SMHC_INT_CMD_DONE
#define DATA_OVER SUNXI_SMHC_INT_DATA_DONE
#define SDIO_IRQ SUNXI_SMHC_INT_SDIO
#define ERRORS SUNXI_SMHC_ERRORS
#define UPDATE SUNXI_SMHC_UPDATE

static int span(const void *p, size_t n) { return p && n <= UINTPTR_MAX - (uintptr_t)p; }
static int overlap(const void *a, size_t an, const void *b, size_t bn)
{
    return (uintptr_t)a < (uintptr_t)b + bn && (uintptr_t)b < (uintptr_t)a + an;
}
void sunxi_sdio_stop(struct sunxi_sdio *h, enum sunxi_sdio_error error)
{
    if (h && h->state != SUNXI_SDIO_FAULT) {
        h->result.error = error == SUNXI_SDIO_NO_ERROR ? SUNXI_SDIO_CARD_ERROR : error;
        h->state = SUNXI_SDIO_FAULT;
    }
}
static int rd(struct sunxi_sdio *h, uint32_t offset, uint32_t *value)
{
    if (h->state == SUNXI_SDIO_FAULT) return 0;
    if (h->config.ops.read32(h->config.context, offset, value)) return 1;
    sunxi_sdio_stop(h, SUNXI_SDIO_IO_ERROR); return 0;
}
static int wr(struct sunxi_sdio *h, uint32_t offset, uint32_t value)
{
    if (h->state == SUNXI_SDIO_FAULT) return 0;
    if (h->config.ops.write32(h->config.context, offset, value)) return 1;
    sunxi_sdio_stop(h, SUNXI_SDIO_IO_ERROR); return 0;
}
static int barrier(struct sunxi_sdio *h)
{
    if (h->config.ops.barrier(h->config.context)) return 1;
    sunxi_sdio_stop(h, SUNXI_SDIO_IO_ERROR); return 0;
}
int sunxi_sdio_time(struct sunxi_sdio *h, uint64_t *out)
{
    uint64_t now;
    if (!h || !span(out, sizeof(*out)) || overlap(h, sizeof(*h), out, sizeof(*out)) ||
        h->state == SUNXI_SDIO_FAULT) return 0;
    if (!h->config.ops.now(h->config.context, &now)) {
        sunxi_sdio_stop(h, SUNXI_SDIO_IO_ERROR); return 0;
    }
    if (now < h->last_now) { sunxi_sdio_stop(h, SUNXI_SDIO_CLOCK_ERROR); return 0; }
    h->last_now = now; *out = now; return 1;
}
static int time_limit(struct sunxi_sdio *h)
{
    uint64_t now;
    if (!sunxi_sdio_time(h, &now)) return 0;
    if (h->deadline && now >= h->deadline) { sunxi_sdio_stop(h, SUNXI_SDIO_TIMEOUT); return 0; }
    return 1;
}
int sunxi_sdio_init(struct sunxi_sdio *h, const struct sunxi_sdio_config *c)
{
    if (!span(h, sizeof(*h)) || !span(c, sizeof(*c)) || overlap(h, sizeof(*h), c, sizeof(*c)) ||
        !c->ops.read32 || !c->ops.write32 || !c->ops.barrier || !c->ops.now ||
        !c->module_hz || c->module_hz > 800000 || h->initialized) return 0;
    struct sunxi_sdio initial = {0};
    initial.initialized = 1; initial.config = *c; initial.card_hz = c->module_hz / 2;
    if (!initial.card_hz) return 0;
    *h = initial; return 1;
}
int sunxi_sdio_prepare(struct sunxi_sdio *h, uint64_t deadline)
{
    if (!h || h->entered || h->state != SUNXI_SDIO_UNPREPARED || deadline <= h->last_now) return 0;
    h->deadline = deadline; h->step = 0; h->state = SUNXI_SDIO_PREPARING; return 1;
}
static int valid_request(const struct sunxi_sdio_request *r)
{
    if (r->command != 3 && r->command != 5 && r->command != 7 &&
        r->command != 52 && r->command != 53) return 0;
    if (r->command != 53) {
        if ((r->command == 3 && r->argument) ||
            (r->command == 5 && (r->argument & 0xff000000u)) ||
            (r->command == 7 && (r->argument & 0xffffu)) ||
            (r->command == 52 && (r->argument & ((1u << 26) | (1u << 8))))) return 0;
        return r->data == NULL && r->bytes == 0 && r->block_bytes == 0;
    }
    uint32_t fn = (r->argument >> 28) & 7u, count = r->argument & 511u;
    if (!fn || !span(r->data, r->bytes) || !r->bytes || r->bytes > SUNXI_SDIO_MAX_BYTES ||
        (r->bytes & 3u)) return 0;
    size_t expected;
    if (r->argument & (1u << 27)) {
        if (!count || r->block_bytes < 4 || r->block_bytes > 512 || (r->block_bytes & 3u)) return 0;
        expected = count * (size_t)r->block_bytes;
    } else {
        if (r->block_bytes) return 0;
        expected = count ? count : 512;
    }
    if (r->bytes != expected) return 0;
    if ((r->argument & (1u << 26)) && r->bytes - 1 > 0x1ffffu - ((r->argument >> 9) & 0x1ffffu)) return 0;
    return 1;
}
int sunxi_sdio_submit(struct sunxi_sdio *h, const struct sunxi_sdio_request *r, uint64_t deadline)
{
    if (!h || h->entered || h->state != SUNXI_SDIO_READY || deadline <= h->last_now ||
        !span(r, sizeof(*r)) || overlap(h, sizeof(*h), r, sizeof(*r)) || !valid_request(r)) return 0;
    if (r->bytes && (overlap(h, sizeof(*h), r->data, r->bytes) ||
                    overlap(r, sizeof(*r), r->data, r->bytes))) return 0;
    h->request = *r; h->result = (struct sunxi_sdio_result){0}; h->deadline = deadline;
    h->step = 0; h->response_seen = 0; h->state = SUNXI_SDIO_QUEUED; return 1;
}
static void prepare_step(struct sunxi_sdio *h)
{
    uint32_t value;
    switch (h->step) {
    case 0:
        if (!rd(h, VERSION, &h->version)) return;
        value = h->version & 0xffffffu;
        if (value < 0x50300u || value >= 0x60000u) { sunxi_sdio_stop(h, SUNXI_SDIO_VARIANT_ERROR); return; }
        if (!wr(h, GCTRL, 7)) return;
        h->step = 1; return;
    case 1:
        if (!rd(h, GCTRL, &value) || (value & 7)) return;
        if (!wr(h, CLKCR, 0) || !wr(h, IMASK, 0) || !wr(h, IDIE, 0) ||
            !wr(h, DMAC, 0) || !wr(h, GCTRL, AHB)) return;
        h->step = 2; return;
    case 2:
        if (!rd(h, NTSR, &value) || !wr(h, NTSR, value & ~START)) return;
        if (!rd(h, EDSD, &value) || !wr(h, EDSD, value & ~1u)) return;
        if (!rd(h, DRIVE, &value) || !wr(h, DRIVE, (value & ~(3u << 16)) | (3u << 16))) return;
        if (!wr(h, SAMPLE, 1u << 7) || !wr(h, WIDTH, 0) || !wr(h, TMOUT, UINT32_MAX) ||
            !barrier(h) || !wr(h, CMD, UPDATE)) return;
        h->step = 3; return;
    case 3:
        if (!rd(h, CMD, &value) || (value & START)) return;
        if (!wr(h, CLKCR, 1u << 16) || !barrier(h) || !wr(h, CMD, UPDATE)) return;
        h->step = 4; return;
    default:
        if (!rd(h, CMD, &value) || (value & START)) return;
        if (!rd(h, CLKCR, &value)) return;
        if (value != (1u << 16)) { sunxi_sdio_stop(h, SUNXI_SDIO_SETUP_ERROR); return; }
        if (!rd(h, WIDTH, &value)) return;
        if (value != 0) { sunxi_sdio_stop(h, SUNXI_SDIO_SETUP_ERROR); return; }
        if (!rd(h, NTSR, &value)) return;
        if (value & START) { sunxi_sdio_stop(h, SUNXI_SDIO_SETUP_ERROR); return; }
        /* poll's final clock check still covers completion. */
        h->state = SUNXI_SDIO_READY; return;
    }
}
static void issue_step(struct sunxi_sdio *h)
{
    uint32_t value;
    if (!rd(h, STATUS, &value)) return;
    h->result.status = value;
    if (value & ((1u << 9) | (1u << 10))) return;
    if (h->request.bytes && h->step == 0) {
        if (!rd(h, GCTRL, &value) || !wr(h, GCTRL, (value | AHB | 2u) & ~(1u << 5))) return;
        h->step = 1; return;
    }
    if (h->request.bytes && (!rd(h, GCTRL, &value) || (value & 2))) return;
    uint32_t command = START | SUNXI_SMHC_CMD_RESP | h->request.command;
    if (h->request.command != 5) command |= SUNXI_SMHC_CMD_CRC; /* R4 has no CRC. */
    if (h->request.bytes) {
        command |= SUNXI_SMHC_CMD_DATA | SUNXI_SMHC_CMD_WAIT;
        if (h->request.argument & START) command |= SUNXI_SMHC_CMD_WRITE;
        uint32_t block = h->request.block_bytes ? h->request.block_bytes : (uint32_t)h->request.bytes;
        if (!wr(h, BLKSZ, block) || !wr(h, BCNT, (uint32_t)h->request.bytes)) return;
    }
    /* Preserve persistent SDIO/card interrupt bits; never request auto-CMD12. */
    if (!wr(h, RINT, 0xffffu) || !wr(h, ARG, h->request.argument) ||
        !barrier(h) || !wr(h, CMD, command)) return;
    h->state = SUNXI_SDIO_ACTIVE;
}
/* Default-speed (26 MHz class) phases from the pinned v5p3x table: command
 * drive 180 degrees, data drive 90 degrees, command/data sample phase zero. */
#define WIDE_DRIVE (1u << 16)
#define SAMPLE_PHASES 0x330u
static int controller_errors(struct sunxi_sdio *h)
{
    uint32_t raw;
    if (!rd(h, RINT, &raw)) return 0;
    h->sdio_pending |= (raw & SDIO_IRQ) != 0;
    if (raw & ERRORS) { sunxi_sdio_stop(h, SUNXI_SDIO_CONTROLLER_ERROR); return 0; }
    return 1;
}
int sunxi_sdio_widen(struct sunxi_sdio *h, unsigned divisor, uint64_t deadline)
{
    if (!h || h->entered || h->state != SUNXI_SDIO_READY || h->wide || !h->config.ops.module ||
        divisor < 8 || divisor > 32 || deadline <= h->last_now) return 0;
    h->deadline = deadline; h->step = 0; h->divisor = (uint8_t)divisor;
    h->request = (struct sunxi_sdio_request){0}; h->result = (struct sunxi_sdio_result){0};
    h->state = SUNXI_SDIO_WIDENING; return 1;
}
static void widen_step(struct sunxi_sdio *h)
{
    uint32_t value;
    unsigned divisor = h->divisor;
    switch (h->step) {
    case 0:
        /* Stop the card clock only between commands, with no card busy state. */
        if (!rd(h, CMD, &value) || (value & START) || !rd(h, STATUS, &value)) return;
        h->result.status = value;
        if (value & SUNXI_SMHC_STATUS_BUSY) return;
        if (!controller_errors(h) || !wr(h, CLKCR, 0) || !barrier(h) || !wr(h, CMD, UPDATE)) return;
        h->step = 1; return;
    case 1:
        if (!rd(h, CMD, &value) || (value & START)) return;
        if (!controller_errors(h) || !rd(h, CLKCR, &value)) return;
        if (value) { sunxi_sdio_stop(h, SUNXI_SDIO_SETUP_ERROR); return; }
        /* Fresh deadline observations surround the platform's module change. */
        if (!time_limit(h)) return;
        if (h->config.ops.module(h->config.context, divisor) != 1) { sunxi_sdio_stop(h, SUNXI_SDIO_IO_ERROR); return; }
        if (!time_limit(h)) return;
        h->step = 2; return;
    case 2:
        if (!rd(h, DRIVE, &value) || !wr(h, DRIVE, (value & ~(3u << 16)) | WIDE_DRIVE) ||
            !rd(h, NTSR, &value) || !wr(h, NTSR, value & ~SAMPLE_PHASES) ||
            !wr(h, WIDTH, 1) || !wr(h, CLKCR, 1u << 16) || !barrier(h) || !wr(h, CMD, UPDATE)) return;
        h->step = 3; return;
    default:
        if (!rd(h, CMD, &value) || (value & START)) return;
        if (!controller_errors(h)) return;
        uint32_t clock, width, drive, timing;
        if (!rd(h, CLKCR, &clock) || !rd(h, WIDTH, &width) || !rd(h, DRIVE, &drive) || !rd(h, NTSR, &timing)) return;
        if (clock != (1u << 16) || width != 1 || (drive & (3u << 16)) != WIDE_DRIVE ||
            (timing & (START | SAMPLE_PHASES))) { sunxi_sdio_stop(h, SUNXI_SDIO_SETUP_ERROR); return; }
        h->card_hz = 400000000u / (2u * divisor); h->wide = 1; h->state = SUNXI_SDIO_READY; return;
    }
}
static void transfer_step(struct sunxi_sdio *h)
{
    uint32_t raw, status, word;
    if (!rd(h, RINT, &raw)) return;
    h->sdio_pending |= (raw & SDIO_IRQ) != 0;
    h->result.interrupts |= raw;
    if (raw & ERRORS) { sunxi_sdio_stop(h, SUNXI_SDIO_CONTROLLER_ERROR); return; }
    if ((h->result.interrupts & CMD_DONE) && !h->response_seen) {
        if (!rd(h, RESP, &h->result.response)) return;
        h->response_seen = 1;
        /* R1 error bits31..16, excluding informational CARD_IS_LOCKED bit25.
         * Current-state, ready, ECC-disabled and other status bits are not errors. */
        uint32_t errors = (h->request.command == 52 || h->request.command == 53) ? 0xcb00u :
                          h->request.command == 3 ? 0xe000u : h->request.command == 7 ? 0xfdff0000u : 0;
        if (h->result.response & errors) { sunxi_sdio_stop(h, SUNXI_SDIO_RESPONSE_ERROR); return; }
    }
    /* One-bit operation samples STATUS before every word. Four-bit operation
     * sizes each burst from one FIFO-level sample: the fill level for reads, or
     * one word when the level reads zero but FIFO-empty is clear, and the free
     * space for writes. Raw underrun/overflow remains a terminal error. */
    int write = (h->request.argument & START) != 0;
    unsigned budget = h->wide ? SUNXI_SDIO_FIFO_BUDGET_WIDE : SUNXI_SDIO_FIFO_BUDGET;
    for (unsigned moved = 0; moved < budget && h->result.transferred < h->request.bytes;) {
        if (!rd(h, STATUS, &status)) return;
        h->result.status = status;
        unsigned level = SUNXI_SMHC_STATUS_FIFO_LEVEL(status), n;
        if (write) n = (status & SUNXI_SMHC_STATUS_FIFO_FULL) || level >= SUNXI_SMHC_FIFO_DEPTH ? 0 : SUNXI_SMHC_FIFO_DEPTH - level;
        else n = (status & SUNXI_SMHC_STATUS_FIFO_EMPTY) ? 0 : level ? level : 1;
        size_t left = (h->request.bytes - h->result.transferred) / 4;
        if (n > left) n = (unsigned)left;
        if (n > budget - moved) n = budget - moved;
        if (n && !h->wide) n = 1;
        if (!n) break;
        for (unsigned i = 0; i < n; ++i) {
            uint8_t *p = h->request.data + h->result.transferred;
            if (write) {
                word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
                if (!wr(h, FIFO, word)) return;
            } else {
                if (!rd(h, FIFO, &word)) return;
                for (unsigned b = 0; b < 4; ++b) p[b] = (uint8_t)(word >> (8 * b));
            }
            h->result.transferred += 4;
        }
        moved += n;
    }
    if (!h->response_seen || (h->request.bytes &&
        (!(h->result.interrupts & DATA_OVER) || h->result.transferred != h->request.bytes))) return;
    if (!rd(h, STATUS, &status)) return;
    h->result.status = status;
    if ((h->request.command == 7 || (h->request.bytes && (h->request.argument & START))) && (status & (1u << 9))) return;
    /* Observe late controller errors before accepting a completed transfer. */
    if (!rd(h, RINT, &raw)) return;
    h->result.interrupts |= raw; h->sdio_pending |= (raw & SDIO_IRQ) != 0;
    if (raw & ERRORS) { sunxi_sdio_stop(h, SUNXI_SDIO_CONTROLLER_ERROR); return; }
    if (!wr(h, RINT, h->result.interrupts & 0xffffu)) return;
    h->state = SUNXI_SDIO_DONE;
}
void sunxi_sdio_poll(struct sunxi_sdio *h)
{
    if (!h || h->entered || h->state == SUNXI_SDIO_FAULT || h->state == SUNXI_SDIO_UNPREPARED || h->state == SUNXI_SDIO_DONE) return;
    h->entered = 1;
    enum sunxi_sdio_state state = h->state;
    uint8_t step = h->step, seen = h->response_seen;
    size_t moved = h->result.transferred;
    if (!time_limit(h)) goto out;
    switch (h->state) {
    case SUNXI_SDIO_PREPARING: prepare_step(h); break;
    case SUNXI_SDIO_WIDENING: widen_step(h); break;
    case SUNXI_SDIO_QUEUED: issue_step(h); break;
    case SUNXI_SDIO_ACTIVE: transfer_step(h); break;
    case SUNXI_SDIO_READY: {
        uint32_t raw;
        if (rd(h, RINT, &raw)) h->sdio_pending |= (raw & SDIO_IRQ) != 0;
        break;
    }
    default: break;
    }
    if (h->state != SUNXI_SDIO_FAULT && time_limit(h) &&
        (h->state == SUNXI_SDIO_READY || h->state == SUNXI_SDIO_DONE)) h->deadline = 0;
out:
    if ((h->state != state || h->step != step || h->response_seen != seen ||
         h->result.transferred != moved) && h->progress != UINT64_MAX) ++h->progress;
    h->entered = 0;
}
int sunxi_sdio_take_result(struct sunxi_sdio *h, struct sunxi_sdio_result *r)
{
    if (!h || h->entered || (h->state != SUNXI_SDIO_DONE && h->state != SUNXI_SDIO_FAULT) ||
        !span(r, sizeof(*r)) || overlap(h, sizeof(*h), r, sizeof(*r)) ||
        (h->request.bytes && overlap(r, sizeof(*r), h->request.data, h->request.bytes))) return 0;
    *r = h->result;
    if (h->state == SUNXI_SDIO_DONE) h->state = SUNXI_SDIO_READY;
    return 1;
}
int sunxi_sdio_ack_irq(struct sunxi_sdio *h)
{
    uint32_t raw;
    if (!h || h->entered || h->state != SUNXI_SDIO_READY) return 0;
    h->entered = 1;
    int ok = time_limit(h) && wr(h, RINT, SDIO_IRQ) && rd(h, RINT, &raw) && time_limit(h);
    if (ok) h->sdio_pending = (raw & SDIO_IRQ) != 0;
    h->entered = 0; return ok;
}
