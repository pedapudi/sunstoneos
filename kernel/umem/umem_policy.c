#include "umem_policy.h"
#include "umem.h"
#include "a733_device_memory_layout.h"

#define ATTR_MASK (UINT64_C(7) << 2)
#define SLOT1_BASE UINT64_C(0x40000000)
#define BLOCK UINT64_C(0x200000)

int umem_policy_init(struct umem_el1_map *p, const struct umem_cpu_ops *cpu, uint64_t *l1, struct umem_policy_table *kernel_slot1,
                     uint64_t (*split_pages)[512])
{
    if (!p || !cpu || !cpu->store || !cpu->dsb_ish || !cpu->isb || !cpu->tlbi_va_all_asids || !cpu->dc_ivac || !l1 || !kernel_slot1 ||
        !kernel_slot1->l2) return -1;
    p->cpu = *cpu;
    p->l1 = l1;
    p->split_pages = split_pages;
    p->split_mask = 0;
    p->spaces = 0;
    kernel_slot1->base = SLOT1_BASE;
    kernel_slot1->next = 0;
    p->tables = kernel_slot1;
    return 0;
}

void umem_policy_add_table(struct umem_el1_map *p, struct umem_policy_table *t)
{
    t->next = p->tables;
    p->tables = t;
}

void umem_policy_note_space(struct umem_el1_map *p) { ++p->spaces; }

int umem_policy_split(struct umem_el1_map *p, unsigned slot)
{
    if (slot < 2 || slot >= 2 + UMEM_POLICY_UPPER_SLOTS || !p->split_pages || p->spaces) return -1;
    unsigned i = slot - 2;
    if (p->split_mask & (1u << i)) return 0;
    uint64_t block = p->l1[slot];
    if ((block & 3) != 1) return -1;                      /* a 1 GiB block */
    uint64_t *page = p->split_pages[i];
    uint64_t base = (uint64_t)slot << 30;
    for (unsigned j = 0; j < 512; ++j) {
        uint64_t attrs = block & ~UINT64_C(0x0000ffffc0000000);       /* every field except the output address */
        page[j] = (attrs & ~UINT64_C(3)) | ((base + (uint64_t)j * BLOCK) & UINT64_C(0x0000ffffffe00000)) | 1;
    }
    p->cpu.dsb_ish(p->cpu.ctx);
    p->cpu.store(p->cpu.ctx, &p->l1[slot], 0);
    p->cpu.dsb_ish(p->cpu.ctx);
    p->cpu.tlbi_va_all_asids(p->cpu.ctx, base);
    p->cpu.dsb_ish(p->cpu.ctx);
    p->cpu.store(p->cpu.ctx, &p->l1[slot], (uint64_t)(uintptr_t)page | 3);
    p->cpu.dsb_ish(p->cpu.ctx);
    p->cpu.isb(p->cpu.ctx);
    p->split[i] = (struct umem_policy_table){page, base, 0};
    umem_policy_add_table(p, &p->split[i]);
    p->split_mask |= 1u << i;
    return 0;
}

static uint64_t *entry_of(struct umem_policy_table *t, uint64_t chunk)
{
    if (chunk < t->base || chunk - t->base >= UMEM_GIB) return 0;
    return &t->l2[(chunk - t->base) / BLOCK];
}

int umem_policy_convert(void *ctx, uint64_t chunk, unsigned policy)
{
    struct umem_el1_map *p = ctx;
    if (!p || (chunk & (BLOCK - 1)) || policy > UMEM_NC || chunk < SLOT1_BASE) return -1;
    unsigned slot = (unsigned)(chunk >> 30);
    if (slot >= 2 && !(p->split_mask & (1u << (slot - 2)))) return -1;   /* an unsplit slot is one block */
    uint64_t want = (uint64_t)(policy == UMEM_NC ? 2 : 1) << 2;
    /* Every table that maps the chunk must map it as a valid block with the other attribute. */
    unsigned tables = 0;
    for (struct umem_policy_table *t = p->tables; t; t = t->next) {
        uint64_t *e = entry_of(t, chunk);
        if (!e) continue;
        if ((*e & 3) != 1 || (*e & ATTR_MASK) == want) return -1;
        ++tables;
    }
    if (!tables) return -1;
    /* Break: clear the valid bit of every entry; the other bits stay for the make step. */
    for (struct umem_policy_table *t = p->tables; t; t = t->next) {
        uint64_t *e = entry_of(t, chunk);
        if (e) p->cpu.store(p->cpu.ctx, e, *e & ~UINT64_C(1));
    }
    p->cpu.dsb_ish(p->cpu.ctx);
    p->cpu.tlbi_va_all_asids(p->cpu.ctx, chunk);
    p->cpu.dsb_ish(p->cpu.ctx);
    p->cpu.isb(p->cpu.ctx);
    /* Make: the same descriptors with the new attribute index. */
    for (struct umem_policy_table *t = p->tables; t; t = t->next) {
        uint64_t *e = entry_of(t, chunk);
        if (e) p->cpu.store(p->cpu.ctx, e, (*e & ~ATTR_MASK) | want | 1);
    }
    p->cpu.dsb_ish(p->cpu.ctx);
    p->cpu.isb(p->cpu.ctx);
    /* The lines of the old attribute: the chunk is free, so its contents are dead and the invalidate loses nothing. */
    return p->cpu.dc_ivac(p->cpu.ctx, chunk, BLOCK) ? -1 : 0;
}
