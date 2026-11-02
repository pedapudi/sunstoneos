/* A strict register model for host checks.
 *
 * Every register the model defines carries the source of its behavior: a board measurement (a queue entry or a
 * fact of docs/hardware/a733-facts.json) or a reference source (Linux, TF-A, a vendor driver, an Arm or Allwinner
 * specification section). The model refuses what it does not define:
 *   - an access to an offset that is not defined,
 *   - an access of a width other than the one defined,
 *   - a write that the register's write hook declines (the hook models only values it can cite),
 *   - a definition without a source.
 * A refusal calls the handler; the default prints the message and aborts. A check that wants to observe a refusal
 * installs its own handler. The accessors then return zero and the write is dropped.
 *
 * Use through checks/host/mmio/a733_mmio.h, which routes the a733_mmio_* accessors of unchanged driver code here;
 * checks/host_link.py builds a module that way. */
#ifndef SUNSTONE_HOST_STRICT_MMIO_H
#define SUNSTONE_HOST_STRICT_MMIO_H
#include <stddef.h>
#include <stdint.h>

struct strict_reg {
    uintptr_t address;
    unsigned size;           /* defined access width in bytes: 1, 2, 4 or 8 */
    const char *name;
    const char *cite;        /* required: where the behavior of this register comes from */
    uint64_t value;          /* the stored word; initial value is a scenario input unless the cite says otherwise */
    uint64_t (*read)(struct strict_reg *reg);                   /* NULL: return value */
    int (*write)(struct strict_reg *reg, uint64_t value);       /* NULL: store value; nonzero: refuse */
    void *context;
    unsigned reads, writes;
};

void strict_mmio_define(struct strict_reg *regs, size_t count);
void strict_mmio_clear(void);
void strict_mmio_set_refusal_handler(void (*handler)(const char *message));
unsigned strict_mmio_refusals(void);
uint64_t strict_mmio_read(uintptr_t address, unsigned size);
void strict_mmio_write(uintptr_t address, unsigned size, uint64_t value);
#endif
