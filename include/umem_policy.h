#ifndef SUNSTONE_UMEM_POLICY_H
#define SUNSTONE_UMEM_POLICY_H
/* The change of a 2 MiB chunk's cache policy in the kernel's EL1 translation tables (docs/designs/unified-memory.md,
 * "Cache policy applies to every mapping of a buffer"). The ledger calls umem_policy_convert() through its
 * convert_chunk hook for a chunk that is free, so no device and no task uses it.
 *
 * Every table that maps the chunk changes together. Between the invalid entry and the new one no entry maps the
 * chunk, so write-back and non-cacheable mappings of one page never coexist, in a table or in a TLB. Cache lines are
 * cleaned and invalidated through the old mapping before any descriptor changes. The instructions sit behind umem_cpu_ops so a host
 * model can check the order. */
#include <stdint.h>

struct umem_cpu_ops {
    void *ctx;
    void (*store)(void *ctx, uint64_t *entry, uint64_t value);   /* a plain store of a table entry */
    void (*dsb_ish)(void *ctx);
    void (*isb)(void *ctx);
    void (*tlbi_va_all_asids)(void *ctx, uint64_t va);           /* TLBI VAAE1IS */
    int (*dc_civac)(void *ctx, uint64_t va, uint64_t bytes);     /* clean and invalidate, complete before return; 0 on success */
};

/* A level-2 table that maps one gigabyte of the identity map starting at base (512 entries of 2 MiB). */
struct umem_policy_table { uint64_t *l2; uint64_t base; struct umem_policy_table *next; };

#define UMEM_POLICY_UPPER_SLOTS 11u    /* level-1 slots 2 to 12 */
struct umem_el1_map {
    struct umem_cpu_ops cpu;
    uint64_t *l1;                      /* the live kernel level-1 table, 512 entries */
    struct umem_policy_table *tables;  /* every level-2 table that maps DRAM at EL1 */
    uint64_t (*split_pages)[512];      /* a zeroed 4 KiB page for each upper slot that may split */
    struct umem_policy_table split[UMEM_POLICY_UPPER_SLOTS];
    unsigned split_mask;
    unsigned spaces;                   /* application roots copied from l1 */
};

/* kernel_slot1: the kernel's level-2 table for 0x4000_0000..0x8000_0000. */
int umem_policy_init(struct umem_el1_map *, const struct umem_cpu_ops *, uint64_t *l1, struct umem_policy_table *kernel_slot1,
                     uint64_t (*split_pages)[512]);
/* Register the private level-2 copy of an application space; its entries for free chunks follow every conversion. */
void umem_policy_add_table(struct umem_el1_map *, struct umem_policy_table *);
/* An application root now copies l1: slots that are still 1 GiB blocks can no longer split (the copies would keep the
 * block). Split the slots before any space is built. */
void umem_policy_note_space(struct umem_el1_map *);
/* Replace the 1 GiB block of an upper slot by a level-2 table of 2 MiB blocks with the same attributes. Boot only: no
 * other core may touch that gigabyte meanwhile. */
int umem_policy_split(struct umem_el1_map *, unsigned slot);
/* The ledger's convert_chunk hook (ctx is the struct umem_el1_map). Returns 0, or -1 when a table does not map the
 * chunk as a block, or a slot cannot split. */
int umem_policy_convert(void *policy, uint64_t chunk, unsigned policy_id);
#endif
