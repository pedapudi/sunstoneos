/* Actual GPU controller, TOP owner, PMIC provider and rail ledger; modeled IO. */
#include "power-domains-model.h"
static uint32_t core[9];
static unsigned pending, stalled, release_count;
static struct a733_gpu_power gpu;
static int now(void *context, uint64_t *out) { (void)context; return pmic_model_now(&m, out); }
static int acquire(void *context, struct a733_gpu_power_resources *out)
{ (void)context; *out = (struct a733_gpu_power_resources){400000000,800000}; return 1; }
static int release(void *context)
{
    (void)context; assert(!core[0] && !core[2] && !(d.ppu[5][2] & 15));
    assert(!gpu.row.counted && a733_power_domain_state(&gpu.row) == A733_POWER_ROW_OFF);
    ++release_count; return 1;
}
static int read_reg(void *context, enum a733_gpu_power_bank bank, uint32_t off, uint32_t *out)
{
    (void)context;
    if (bank == A733_GPU_CORE) {
        assert((d.ppu[5][2] & 15) == 8 && gpu.row.counted);
        assert(off == 0 || off == 8 || off == 0x20);
        if (off == 8 && pending) {
            assert((d.ccu_b20 & 0x80000000) && (d.ccu_b24 & 0x10001) == 0x10001);
            if (!stalled) { core[2] = core[0]; pending = 0; }
        }
        *out = core[off/4]; return 1;
    }
    uint32_t *word = level_word((bank == A733_GPU_CCU ? CCU_BASE : RCCU_BASE) + off);
    assert(word); *out = *word; return 1;
}
static int update(void *context, enum a733_gpu_power_bank bank, uint32_t off, uint32_t clear, uint32_t set)
{
    (void)context;
    if (bank == A733_GPU_CORE) {
        assert(gpu.row.counted && a733_power_domain_state(&gpu.row) == A733_POWER_ROW_ON);
        assert(clear == UINT32_MAX && ((off == 0 && (set == 0 || set == 8)) || (off == 0x20 && !set)));
        core[off/4] = set; if (!off) pending = 1; return 1;
    }
    assert(bank == A733_GPU_CCU && (off == 0xb20 || off == 0xb24));
    if (gpu.phase == A733_GPU_POWER_STOPPING) assert(!core[0] && !core[2] && !pending);
    uint32_t *word = level_word(CCU_BASE + off); *word = (*word & ~clear) | set;
    if (off == 0xb20) *word &= ~(1u << 27);
    return 1;
}
static enum a733_gpu_power_result run(unsigned on)
{
    assert((on ? a733_gpu_power_on(&gpu, m.now + 10000) : a733_gpu_power_off(&gpu, m.now + 10000)) == A733_GPU_POWER_AGAIN);
    enum a733_gpu_power_result result; unsigned polls = 0;
    while ((result = a733_gpu_power_poll(&gpu)) == A733_GPU_POWER_AGAIN) {
        assert(++polls < 100); service_step(); m.now += 1000;
    }
    return result;
}
int main(void)
{
    const struct a733_gpu_power_ops ops = {now, acquire, release, read_reg, update};
    for (unsigned mode = 0; mode < 6; ++mode) {
        domains_model_setup(0x28,23); d.ack_us = 0;
        memset(&gpu,0,sizeof(gpu)); memset(core,0,sizeof(core));
        core[0] = core[2] = (mode & 1) ? 8 : 0; core[8] = 0x010101ff;
        d.ppu[5][0] |= (mode & 2) ? 8 : 0; d.ppu[5][2] |= (mode & 2) ? 8 : 0;
        pending = stalled = release_count = 0;
        struct a733_rail_token token = {0};
        assert(a733_gpu_power_init(&gpu,&ops,0));
        assert(a733_power_domain_row_init(&gpu.row,&d.owner,5,9,&token));
        domains_boot(); token = domains_rail(9,A733_RAIL_DCDC4);
        uint32_t npu[0x1000/4]; memcpy(npu,d.ppu[4],sizeof(npu));
        unsigned twi_writes = m.writes; uint32_t ahb = d.ccu_5c0, mbus = d.ccu_5e0;
        stalled = mode == 4;
        enum a733_gpu_power_result result = run(1);
        if (mode != 4) {
            assert(result == A733_GPU_POWER_OK); stalled = mode == 5; result = run(0);
        }
        if (mode >= 4) {
            assert(result == A733_GPU_POWER_FAULT && gpu.error == A733_GPU_POWER_TIMEOUT);
            assert(gpu.lease_held && gpu.row.counted && !release_count);
            assert(a733_power_domain_state(&gpu.row) == A733_POWER_ROW_ON);
            assert((d.ppu[5][2]&15) == 8 && (d.ccu_b24&0x10001) == 0x10001);
            assert(a733_rail_release(&m.rails,&token,m.now+100000) == A733_RAIL_BUSY);
        } else {
            assert(result == A733_GPU_POWER_OK && release_count == 1 && !gpu.lease_held);
            assert(a733_rail_release(&m.rails,&token,m.now+100000) == A733_RAIL_READY);
        }
        assert(!memcmp(npu,d.ppu[4],sizeof(npu)) && m.writes == twi_writes);
        assert(d.ccu_5c0 == ahb && d.ccu_5e0 == mbus && m.pmic[0x13] == 30);
        for (unsigned i = 0; i < d.trace_n; ++i)
            assert(d.trace[i].address < 0x07066000 || d.trace[i].address >= 0x07067000);
    }
    puts("PASS six CORE/TOP composition cases: four inherited pairs, ON/OFF timeouts retain TOP row and rail pin; NPU, rail and shared gates preserved");
}
