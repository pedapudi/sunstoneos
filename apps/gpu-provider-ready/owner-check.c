/* SPDX-License-Identifier: GPL-2.0-only */
/* One idle GPU row triggers both supply observations but no domain access. */
#include "power-domains-model.h"

int main(void)
{
    for (unsigned inherited = 0; inherited <= 8; inherited += 8) {
        domains_model_setup(0x28, 23);
        struct a733_power_domain_row row = {0};
        struct a733_rail_token unused = {0};
        d.ppu[4][0] |= 8; d.ppu[4][2] |= 8;
        d.ppu[5][0] |= inherited; d.ppu[5][2] |= inherited;
        uint32_t retained[sizeof d.ppu / sizeof(uint32_t)];
        memcpy(retained, d.ppu, sizeof retained);
        unsigned char supplies[sizeof m.pmic];
        memcpy(supplies, m.pmic, sizeof supplies);
        assert(a733_power_domain_row_init(&row, &d.owner, 5, 3, &unused));
        domains_boot();
        for (unsigned n = 0; m.rails.shared_held && n < 100; ++n) service_step();
        assert((a733_power_authority_load(&m.rails.authority) & 15) == 15);
        assert(!d.owner.failed && a733_power_service_idle(&d.owner));
        assert(!d.ppu_reads && !d.ppu_writes && !d.level_reads && !d.trace_n);
        assert(!memcmp(retained, d.ppu, sizeof retained));
        assert(!memcmp(supplies, m.pmic, sizeof supplies));
        assert(!unused.id && !row.counted && !atomic_load(&row.request) && !atomic_load(&row.state));
        assert(!m.rails.shared_held && !m.rails.exclusive_held && m.rails.free_slots == UINT32_MAX);
        for (unsigned i = 0; i < A733_RAIL_CLIENTS; ++i) assert(!m.rails.slots[i].id && !m.rails.pins[i]);
        for (unsigned i = 0; i < A733_RAIL_BORROWS; ++i) assert(!m.rails.borrows[i].id);
        for (unsigned i = 0; i < A733_PMIC_REQUESTS; ++i) assert(!m.provider.slots[i].state && !m.provider.slots[i].borrow.id);
        for (unsigned i = 0; i < A733_POWER_DOMAINS; ++i) assert(d.owner.domain[i].state == A733_POWER_DOMAIN_UNKNOWN && !d.owner.domain[i].count);
        printf("PASS idle GPU row: inherited=%u authority=15 domain_accesses=0 retained_NPU_unchanged=1 steps=%u\n", inherited, d.steps);
    }
    return 0;
}
