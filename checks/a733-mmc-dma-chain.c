/* The platform admits a data command with DMA enabled only after it walks the descriptor chain the core built (dma_chain in
 * drivers/storage/a733_mmc_platform.c). The first board write (v11, v12) was refused here: the chain check rejected every write-direction
 * command, so the CMD25 of the bounded DMA write retained the controller with no refused register and no deadline. This check includes the
 * platform source to reach the static function and requires: a CMD25 word is accepted while the core has an authenticated DMA write in progress
 * and its chain is exact; it is refused without one; a read-direction word is refused during a write; a changed descriptor is refused. */
#include "../drivers/storage/a733_mmc_platform.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BASE UINT32_C(0x04020000)
#define BLOCKS 256u
static uint32_t gctrl, dlba, bcnt;
static volatile struct sunxi_mmc_dma_descriptor chain[SUNXI_MMC_DMA_DESCRIPTORS + 1];
static uint8_t source[BLOCKS * 512u] __attribute__((aligned(64)));
static int now(void *c, uint64_t *out) { (void)c; *out = 100; return 1; }
static int rd32(void *c, uintptr_t a, uint32_t *out)
{
    (void)c;
    switch (a - BASE) {
    case SUNXI_SMHC_GCTRL: *out = gctrl; return 1;
    case SUNXI_SMHC_DLBA: *out = dlba; return 1;
    case SUNXI_SMHC_BCNT: *out = bcnt; return 1;
    default: return 0;
    }
}
static void layout(unsigned bytes)
{
    unsigned n = (bytes + SUNXI_MMC_DMA_SEGMENT - 1) / SUNXI_MMC_DMA_SEGMENT;
    memset((void *)chain, 0, sizeof chain);
    for (unsigned i = 0; i < n; ++i) {
        uint32_t config = SUNXI_SMHC_DES_OWN | SUNXI_SMHC_DES_CH | SUNXI_SMHC_DES_DIC;
        if (!i) config |= SUNXI_SMHC_DES_FD;
        if (i == n - 1) config = (config | SUNXI_SMHC_DES_LD) & ~SUNXI_SMHC_DES_DIC;
        unsigned left = bytes - i * SUNXI_MMC_DMA_SEGMENT;
        chain[i].config = config;
        chain[i].size = left < SUNXI_MMC_DMA_SEGMENT ? left : SUNXI_MMC_DMA_SEGMENT;
        chain[i].buffer = (uint32_t)(((uintptr_t)source + (uintptr_t)i * SUNXI_MMC_DMA_SEGMENT) >> 2);
        chain[i].next = (uint32_t)((uintptr_t)&chain[i + 1] >> 2);
    }
}
int main(void)
{
    static struct a733_mmc_platform p;
    p.base = BASE;
    p.ops.now_us = now; p.ops.read = rd32;
    p.deadline = UINT64_MAX; p.config.lifetime_deadline_us = UINT64_MAX;
    p.config.dma = chain;
    p.core.borrowed = source; p.core.borrowed_bytes = sizeof source;
    gctrl = SUNXI_SMHC_GCTRL_DMA_ENABLE; dlba = (uint32_t)((uintptr_t)chain >> 2); bcnt = sizeof source;
    layout(sizeof source);
    const uint32_t cmd25 = SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_WAIT | SUNXI_SMHC_CMD_AUTO_STOP | SUNXI_SMHC_CMD_WRITE | SUNXI_SMHC_CMD_DATA |
                           SUNXI_SMHC_CMD_CRC | SUNXI_SMHC_CMD_RESP | 25u;
    const uint32_t cmd18 = SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_WAIT | SUNXI_SMHC_CMD_AUTO_STOP | SUNXI_SMHC_CMD_DATA | SUNXI_SMHC_CMD_CRC |
                           SUNXI_SMHC_CMD_RESP | 18u;
    /* A bounded DMA write in progress. */
    p.core.dma_write = 1;
    assert(dma_chain(&p, cmd25) == 1);
    assert(dma_chain(&p, cmd18) == 0);                        /* a read-direction word during a write */
    chain[3].buffer += 1;
    assert(dma_chain(&p, cmd25) == 0);                        /* a changed descriptor */
    layout(sizeof source);
    bcnt = sizeof source - 512u;
    assert(dma_chain(&p, cmd25) == 0);                        /* a byte count that is not the borrowed source */
    bcnt = sizeof source;
    /* No authenticated write: the write direction stays refused, the read direction as before. */
    p.core.dma_write = 0;
    assert(dma_chain(&p, cmd25) == 0);
    assert(dma_chain(&p, cmd18) == 1);
    /* DMA not enabled in GCTRL: programmed I/O, nothing to walk. */
    gctrl = 0;
    assert(dma_chain(&p, cmd25) == 1 && dma_chain(&p, cmd18) == 1);
    /* Close from a finished operation: a collected read, or a bounded write the card confirmed; not an unconfirmed write, a borrowed buffer or an error. */
    memset((void *)&p.core.result, 0, sizeof p.core.result); p.core.borrowed = NULL; p.core.state = SUNXI_MMC_READY;
    assert(!close_ready(&p));                                                  /* nothing finished */
    p.core.result.received = 512; assert(close_ready(&p));                     /* a read */
    p.core.result.received = 0; p.core.result.write_started = 1; assert(!close_ready(&p));          /* a write that was not confirmed */
    p.core.result.confirmed_blocks = 256; assert(close_ready(&p));             /* a write the card confirmed */
    p.core.result.error = SUNXI_MMC_TIMEOUT; assert(!close_ready(&p));
    p.core.result.error = SUNXI_MMC_OK; p.core.borrowed = source; assert(!close_ready(&p));
    p.core.borrowed = NULL; p.core.state = SUNXI_MMC_WRITING; assert(!close_ready(&p));
    puts("PASS: platform chain admission accepts the bounded DMA write only while the core has one in progress");
    return 0;
}
