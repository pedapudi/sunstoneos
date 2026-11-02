#!/usr/bin/env python3
"""Drive the bounded CMD25 DMA write through the platform's own core callbacks (drivers/storage/a733_mmc_platform.c) against the strict write model.

checks/sunxi-mmc-write-dma.c drives the core directly with the model's callbacks, so the platform's admission of the write (its
descriptor-chain check, the IDMAC value filter, its cache-sync and guard callbacks) is never run by it. The first board write was refused in that
admission (dma_chain rejected every write-direction command). This check builds a test from the model's source: its controller, card and `h` are
the platform's core (`platform.core`), its register accesses go through core_read/core_write/core_dma_sync, and each of the nominal writes and one
interrupt-completion write must complete with the platform in the path, and with the native adapter's register filter
(drivers/storage/a733_mmc_platform_native.c, built with A733_MMC_NATIVE_HOST_TEST) between the platform and the model, bound to a verified bounds object. --platform names an older platform source for the mutation check."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
ap = argparse.ArgumentParser()
ap.add_argument('--evidence', type=Path, required=True)
ap.add_argument('--platform', type=Path, default=root / 'drivers/storage/a733_mmc_platform.c')
a = ap.parse_args()
a.evidence.mkdir(parents=True, exist_ok=True)
model = (root / 'checks/sunxi-mmc-write-dma.c').read_text()
model = model[:model.index('int main(void)')]
for name in ('finish', 'command', 'update', 'live', 'lock', 'rd', 'wr', 'masked', 'running', 'registers', 'controller', 'metadata', 'profile', 'voltage',
             'presence', 'supply', 'close_idle', 'barrier', 'admit', 'retained', 'outside', 'dma_chain', 'fail', 'timely'):
    model = re.sub(r'(?<![\w.>])%s\b(?=\s*[(,])' % name, 'model_' + name, model)
assert 'static struct sunxi_mmc h;' in model and 'assert(sunxi_mmc_init(&h, &c));' in model and 'id->session == 77' in model
# The platform reads the list base and the byte count back to walk the chain before it admits the command.
assert 'default: assert(!"unexpected controller read"); return 0;' in model
model = model.replace('default: assert(!"unexpected controller read"); return 0;',
                      'case SUNXI_SMHC_DLBA: case SUNXI_SMHC_BCNT: case SUNXI_SMHC_BLKSZ: case SUNXI_SMHC_ARG: *out = m.reg[off / 4]; return 1;\n    default: assert(!"unexpected controller read"); return 0;')
model = model.replace('m.polls, m.sleeps, m.isr_runs, h.command_step); exit(1); }',
                      'm.polls, m.sleeps, m.isr_runs, h.command_step); fprintf(stderr, "platform fault %u line %u why %u refused %u\\n", platform.fault, platform.fault_line, platform.fault_why, platform.refused); exit(1); }')
assert 'static int start(unsigned blocks)' in model
model = model.replace('static int start(unsigned blocks)', 'static int model_start(unsigned blocks)')
model = model.replace('static struct sunxi_mmc h;', 'static struct a733_mmc_platform platform;\n#define h (platform.core)')
model = model.replace('assert(sunxi_mmc_init(&h, &c));', 'assert(platform_core_init(&c));')
model = model.replace('id->session == 77', 'id->session == (uintptr_t)&platform')
head = '''#define _GNU_SOURCE
#include "%s"
#include "a733_platform_locks.h"
#include "a733_mmc_platform_native.h"
#include "a733_storage_bounds.h"
#include "a733_storage_digest.h"
struct sunxi_mmc_config;
static int platform_core_init(const struct sunxi_mmc_config *c);
static int start(unsigned blocks);
''' % a.platform.resolve()
tail = '''
static struct a733_clock_leases registry;
static struct a733_platform_locks locks;
static struct a733_mmc_platform_native native;
static struct a733_storage_bounds bounds;
static uint8_t blob[48 + 16 * 2];
/* The native adapter's register hooks: the controller window is the model, everything else reads zero. */
uint32_t a733_native_host_load(uintptr_t a)
{ uint32_t v = 0; if (a >= platform.base && a < platform.base + 0x304) read32(NULL, (uint32_t)(a - platform.base), &v); return v; }
void a733_native_host_store(uintptr_t a, uint32_t v)
{ if (a >= platform.base && a < platform.base + 0x304) assert(write32(NULL, (uint32_t)(a - platform.base), v) == 1); }
int a733_native_host_current(void *n) { (void)n; return 1; }
int a733_platform_lock_enter(struct a733_platform_locks *l, enum a733_platform_lock k, uint64_t d) { (void)l; (void)k; (void)d; return 1; }
void a733_platform_lock_leave(struct a733_platform_locks *l, enum a733_platform_lock k) { (void)l; (void)k; }
int a733_platform_lock_retain(struct a733_platform_locks *l, enum a733_platform_lock k) { (void)l; (void)k; return 1; }
_Noreturn void platform_panic(const char *file, unsigned line) { fprintf(stderr, "platform panic %s:%u\\n", file, line); abort(); }
static uint8_t card_byte(uint64_t sector, unsigned i) { return (uint8_t)(sector * 13 + i * 3 + 1); }
static int read_card(void *c, uint64_t sector, uint8_t *out) { (void)c; for (unsigned i = 0; i < 512; ++i) out[i] = card_byte(sector, i); return 1; }
static void put64(uint8_t *p, uint64_t v) { for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (8 * i)); }
static int admission(void *c, const struct a733_mmc_platform_config *cfg, const struct a733_clock_token *t) { (void)c; (void)cfg; (void)t; return 1; }
static int platform_core_init(const struct sunxi_mmc_config *c)
{
    memset(&platform, 0, sizeof platform);
    platform.base = UINT32_C(0x04020000);
    platform.config.kind = SUNXI_MMC_SD; platform.config.sd_profile = SUNXI_MMC_SD_HIGH_SPEED4; platform.config.dma = c->dma;
    platform.config.lifetime_deadline_us = UINT64_MAX; platform.deadline = UINT64_MAX;
    platform.clocks = &registry; platform.core_call = 1; platform.held = 1;
    memset(&locks, 0, sizeof locks); locks.initialized = 1;
    memset(&native, 0, sizeof native);
    static int dummy_task;
    assert(a733_mmc_platform_native_init_selected(&native, &locks, &dummy_task, SUNXI_MMC_SD, A733_MMC_EMMC_SDC2, admission, NULL));
    native.sd_profile = SUNXI_MMC_SD_HIGH_SPEED4;
    assert(native.operations.enter(&native, 1000));
    platform.ops = native.operations; platform.cookie = &native;
    platform.ops.now_us = clock_now;                                       /* the model's clock; the native adapter has none */
    platform.ops.dma_sync = dma_sync;
    struct sunxi_mmc_config cfg = {{core_read, core_write, core_barrier, core_now, core_admit, core_retained, core_sd_module,
        core_read_fifo, core_write_fifo, NULL, core_dma_sync}, &platform, (uintptr_t)&platform, SUNXI_MMC_SD, 800000, 0x00300000, SUNXI_MMC_SD_HIGH_SPEED4, c->dma};
    return sunxi_mmc_init(&platform.core, &cfg);
}
/* One verified bounds object admitting sectors 0 to 599 for writes, bound to the adapter once the card record is set. */
static void bind_bounds(void)
{
    memcpy(blob, A733_BOUNDS_MAGIC, 8); blob[8] = 1; blob[12] = 1;
    struct storage_digest d; storage_digest_init(&d);
    uint8_t rec[520]; put64(rec, 0); read_card(NULL, 0, rec + 8); storage_digest_add(&d, rec, sizeof rec);
    storage_digest_end(&d, blob + 16);
    put64(blob + 48, 0); put64(blob + 56, 600);
    put64(blob + 64, 0); put64(blob + 72, 1);
    memset(&bounds, 0, sizeof bounds);
    assert(a733_storage_bounds_parse(&bounds, blob, sizeof blob) && a733_storage_bounds_verify(&bounds, read_card, NULL));
    assert(a733_mmc_platform_native_bind_bounds(&native, &platform, &bounds));
}
static int start(unsigned blocks)
{
    bind_bounds();
    struct sunxi_mmc_write_authority a;
    a733_storage_bounds_authority(&bounds, &a);
    struct sunxi_mmc_write_request r = {A733_BOUNDED_WRITE_TOKEN, m.first, blocks, source, blocks * 512u, 1000000};
    return sunxi_mmc_write_dma(&h, &r, &a);
}
int main(void)
{
    static const unsigned counts[] = {1, 2, 7, 8, 9, 64, 255, 256};
    for (unsigned i = 0; i < sizeof counts / sizeof counts[0]; ++i) {
        unsigned blocks = counts[i];
        setup(1, blocks, 40 + i);
        csd_writable();
        m.prg_polls = i % 3; m.prg_polls_left = m.prg_polls; m.busy_polls = 1 + i % 5;
        assert(start(blocks));
        assert(h.state == SUNXI_MMC_WRITING && h.dma_write);
        for (unsigned n = 0; n < 20000 && h.state == SUNXI_MMC_WRITING; ++n) { platform.core_call = 1; sunxi_mmc_poll(&h); }
        if (h.state != SUNXI_MMC_DONE) { fprintf(stderr, "%u blocks: state %d error %d, platform fault %u line %u refused %u\\n", blocks, h.state, h.result.error, platform.fault, platform.fault_line, platform.refused); return 1; }
        assert(h.result.confirmed_blocks == blocks && h.result.write_started);
        media_is(blocks);
        struct sunxi_mmc_result result;
        assert(sunxi_mmc_take(&h, &result) && result.error == SUNXI_MMC_OK && !platform.fault);
    }
    irq_case("normal 128 KiB", 256, 4000, 300000, 0, 0, 0);
    puts("PASS: the bounded CMD25 DMA write of 1 to 256 blocks completes through the platform core callbacks, and by interrupt completion");
    return 0;
}
'''
(a.evidence / 'platform-write.c').write_text(head + model + tail)
cc = shutil.which('clang-21') or shutil.which('clang')
sources = ['drivers/storage/a733_storage_update.c', 'drivers/storage/storage_digest.c', 'drivers/storage/sunxi_mmc.c', 'drivers/storage/storage_wait.c', 'drivers/clock/a733_clock_leases.c',
           'drivers/power/a733_rail_leases.c', 'drivers/power/a733_pmic.c', 'drivers/gpu/a733_gpu_resources.c', 'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c', 'drivers/storage/a733_storage_bounds.c', 'drivers/storage/a733_mmc_platform_native.c']
binary = a.evidence / 'platform-write'
done = subprocess.run([cc, '-std=gnu11', '-O1', '-g', '-w', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DA733_MMC_NATIVE_HOST_TEST=1', '-DA733_MMC_BOUNDED_WRITER=1', '-Iinclude', '-Ichecks/host', '-I' + str(Path.home() / '.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2/include'), '-Ichecks', '-I' + str(root / 'drivers/storage'),
                       str(a.evidence / 'platform-write.c'), *sources, '-o', str(binary)], cwd=root, capture_output=True, text=True)
if done.returncode:
    sys.exit(done.stderr[:6000])
done = subprocess.run([str(binary)], capture_output=True, text=True)
print(done.stdout, end='')
if done.returncode:
    sys.exit(done.stdout + done.stderr)
