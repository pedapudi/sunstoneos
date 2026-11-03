#ifndef SUNSTONE_UMEM_H
#define SUNSTONE_UMEM_H
/* The unified memory ledger (docs/designs/unified-memory.md): one allocator over the DRAM above the OS image, for
 * buffers that the CPU, the GPU, the NPU and DMA masters share.
 *
 * The code touches no hardware. The caller supplies the few operations that do (cache maintenance, the change of a
 * 2 MiB chunk's cache policy in the kernel's table, the lock and the translation of a physical metadata address) in
 * struct umem_hooks, so the host checks run the code that the image links.
 *
 * A buffer is one physically contiguous extent of 4 KiB pages with an owner, a cache policy (write-back or
 * non-cacheable), a state (CPU, DEVICE or RETAINED) and a set of mappings in address spaces. The ledger keeps free
 * and allocated extents in one address-ordered treap, free extents in size-class lists, and takes its own entry
 * storage from itself at run time. */
#include <stddef.h>
#include <stdint.h>

#define UMEM_PAGE UINT64_C(0x1000)
#define UMEM_CHUNK UINT64_C(0x200000)      /* the unit of cache policy: one level-2 block */
#define UMEM_GIB UINT64_C(0x40000000)
#define UMEM_OWNER_KERNEL 0xffffu          /* the kernel's own extents; no application space maps them */
#define UMEM_OWNER_MIXED 0xfffeu           /* last_owner of a free extent that merged extents of several owners */
#define UMEM_MIN_CELLS 16u                 /* the least entry storage umem_init accepts */
#define UMEM_CELL_CLASSES 40u

enum umem_status {
    UMEM_OK = 0,
    UMEM_INVALID = -1,      /* malformed argument; nothing changed */
    UMEM_NO_SPACE = -2,     /* no extent satisfies the request; nothing changed */
    UMEM_NOT_OWNED = -3,    /* a stale or foreign handle */
    UMEM_BUSY = -4,         /* a device mapping, a device-owned buffer or an unfinished preparation */
    UMEM_NO_META = -5,      /* the ledger cannot find a page for its own entries */
    UMEM_HW = -6,           /* a hook refused; the ledger state stays consistent */
    UMEM_STALE = -7         /* the handle is out of date: the buffer was freed */
};

enum umem_policy { UMEM_WB = 0, UMEM_NC = 1 };               /* MAIR index 1 (0xff), MAIR index 2 (0x44) */
enum umem_state { UMEM_CPU = 0, UMEM_DEVICE = 1, UMEM_RETAINED = 2 };
enum umem_op { UMEM_OP_ZERO, UMEM_OP_CLEAN, UMEM_OP_CLEAN_INVALIDATE, UMEM_OP_STORE_BARRIER, UMEM_OP_LOAD_BARRIER };

/* Devices a request names. A bit gives the device's physical address limit, its alignment rule and nothing else. */
enum {
    UMEM_DEV_NPU = 1u << 0, UMEM_DEV_GPU = 1u << 1, UMEM_DEV_DMA = 1u << 2, UMEM_DEV_DISPLAY = 1u << 3,
    UMEM_DEV_CAMERA = 1u << 4, UMEM_DEV_CODEC = 1u << 5, UMEM_DEV_USB = 1u << 6, UMEM_DEV_PCIE = 1u << 7,
    UMEM_DEV_CRYPTO = 1u << 8, UMEM_DEV_SD = 1u << 9
};
#define UMEM_DEVICES 10u

/* Request flags. UMEM_RAW: ready at once with the previous contents; the caller overwrites or zeroes the buffer. */
enum { UMEM_AT = 1, UMEM_REUSE = 2, UMEM_NO_CONVERT = 4, UMEM_RAW = 8 };

struct umem_request {
    uint64_t bytes;
    uint64_t align;          /* power of two, at least a page; 0 is a page */
    uint64_t phys_max;       /* exclusive upper bound of the extent; 0 is no bound beyond the devices' */
    uint64_t window;         /* power of two: the extent may not cross a multiple of it; 0 is none */
    uint64_t base;           /* with UMEM_AT: the pinned address */
    uint32_t device_mask;
    uint16_t owner;
    uint16_t flags;
    uint8_t policy;
    uint8_t access;          /* UMEM_PROT_* that EL0 may have in the owner's space; the space builders read it */
};

union umem_cell;
struct umem_buf {            /* the kernel's handle; generation 0 is invalid */
    uint64_t base, bytes;
    uint32_t generation;
    union umem_cell *cell;
};
struct umem_extent { uint64_t base, bytes; uint16_t owner; uint8_t policy, access; };
struct umem_range { uint64_t offset, bytes; };

struct umem_hooks {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    /* The address through which the kernel reads and writes a physical metadata page; null is the identity. */
    void *(*phys_to_virt)(void *ctx, uint64_t physical);
    /* Change the cache policy of the free chunk at `chunk` (UMEM_CHUNK aligned) in the kernel's table. */
    int (*convert_chunk)(void *ctx, uint64_t chunk, unsigned policy);
    /* Zero, clean or clean and invalidate [base, base + bytes), or order stores and loads. */
    int (*maintain)(void *ctx, enum umem_op op, uint64_t base, uint64_t bytes);
};

enum umem_space_kind { UMEM_SPACE_CPU = 1, UMEM_SPACE_DEVICE = 2 };
/* One address space: an application's EL0 tables, a GPU context, an NPU directory or an IOMMU bank. */
struct umem_space_ops {
    unsigned kind;
    int (*map)(void *space, const struct umem_buf *, uint64_t offset, uint64_t bytes, unsigned prot, uint64_t *address);
    int (*unmap)(void *space, uint64_t address, uint64_t bytes);
    int (*flush)(void *space, uint64_t address, uint64_t bytes);
    int (*quiesced)(void *space);
};
enum { UMEM_PROT_READ = 1, UMEM_PROT_WRITE = 2 };

struct umem {
    struct umem_hooks hooks;
    union umem_cell *root;                        /* treap of every extent, by base */
    union umem_cell *free_head[2][UMEM_CELL_CLASSES];   /* by policy, then log2 of the pages */
    union umem_cell *spare;
    uint64_t spare_count;
    uint8_t *chunk_policy;                        /* one byte per chunk */
    uint64_t chunk_base, chunk_count;
    uint64_t free_bytes, usable_bytes, meta_bytes;
    uint64_t device_limit[UMEM_DEVICES];          /* working physical limit, exclusive */
    uint32_t generation, rng;
    uint64_t steps;                               /* comparisons and list visits, for the host checks */
    unsigned growing;
    unsigned exact_first;                         /* scan the request's own size class first (UMEM_REUSE) */
};

/* cells: initial entry storage for `cell_count` entries (at least UMEM_MIN_CELLS); chunk_policy: one byte per chunk
 * from chunk_base for chunk_count chunks, covering every range. Ranges are sorted, disjoint, UMEM_CHUNK aligned. */
int umem_init(struct umem *, const struct umem_hooks *, void *cells, size_t cell_count,
              uint8_t *chunk_policy, uint64_t chunk_base, uint64_t chunk_count,
              const struct umem_range *ranges, unsigned range_count);
size_t umem_cell_bytes(void);

/* The working limit of a device (exclusive physical address); a board run that confirms a device above 4 GiB raises it. */
int umem_device_limit_set(struct umem *, unsigned device_bit, uint64_t limit);

/* Reserve an extent. The buffer is ready when umem_ready returns 1; until then umem_prepare_step zeroes it (a new
 * owner) and cleans it (write-back with possible dirty lines) in slices. */
int umem_alloc(struct umem *, const struct umem_request *, struct umem_buf *out);
int umem_prepare_step(struct umem *, const struct umem_buf *, uint64_t max_bytes);   /* 1 ready, 0 more, negative failure */
int umem_ready(const struct umem *, const struct umem_buf *);
int umem_free(struct umem *, const struct umem_buf *);
/* Lock-free: the extent and state of a live buffer; UMEM_STALE when the handle is out of date. */
int umem_describe(const struct umem *, const struct umem_buf *, struct umem_extent *, int *state);
/* Record that the CPU wrote the buffer outside a mapping the ledger knows (the kernel's own writes). */
int umem_touch(struct umem *, const struct umem_buf *);
/* Move a buffer between holders and run the maintenance the policy needs; ranges null covers the buffer. */
int umem_handoff(struct umem *, const struct umem_buf *, enum umem_state from, enum umem_state to,
                 const struct umem_range *ranges, unsigned count);

int umem_map(struct umem *, const struct umem_buf *, const struct umem_space_ops *, void *space,
             uint64_t offset, uint64_t bytes, unsigned prot, uint64_t *address);
int umem_unmap(struct umem *, const struct umem_buf *, const struct umem_space_ops *, void *space, uint64_t address);

/* The handle of the allocated extent that starts at base (a kernel path that knows the address, not the handle). */
int umem_lookup(struct umem *, uint64_t base, struct umem_buf *out);
/* Give the tail of a buffer back to the ledger: the buffer keeps its first new_bytes (a page multiple, at least a page).
 * Refused with UMEM_BUSY unless the buffer is CPU-held and unmapped. The caller's handle keeps the old size; read the
 * new one with umem_describe. */
int umem_trim(struct umem *, const struct umem_buf *, uint64_t new_bytes);
/* The largest extent a request with these constraints (policy, align, phys_max, window, device_mask) could take:
 * *base is where it starts and *bytes its length. A following umem_alloc with UMEM_AT at *base takes exactly that. */
int umem_largest(struct umem *, const struct umem_request *constraints, uint64_t *base, uint64_t *bytes);
/* Owner of the byte at pa (0 when free or outside the ledger). */
uint16_t umem_owner_of(struct umem *, uint64_t pa);
/* The extents of one owner in address order; returns the count, calls out for the first `capacity`. */
unsigned umem_extents_of(struct umem *, uint16_t owner, struct umem_extent *out, unsigned capacity);
/* Call fn for each extent of the owner in address order; fn returns nonzero to stop. Returns the number visited. */
unsigned umem_for_each(struct umem *, uint16_t owner, int (*fn)(void *ctx, const struct umem_extent *), void *ctx);
uint64_t umem_free_bytes(const struct umem *);
/* Consistency walk for the host checks: 0 when every invariant holds, else a nonzero code naming the first broken one. */
int umem_check(struct umem *);
#endif
