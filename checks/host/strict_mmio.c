#include "strict_mmio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct strict_reg *table;
static size_t entries;
static unsigned refused;
static void (*handler)(const char *message);

static void refuse(const char *format, uintptr_t address, unsigned size, const char *extra)
{
    char message[200];
    ++refused;
    snprintf(message, sizeof message, format, (unsigned long)address, size, extra ? extra : "");
    if (handler) { handler(message); return; }
    fprintf(stderr, "strict register model: %s\n", message);
    abort();
}

void strict_mmio_define(struct strict_reg *regs, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (!regs[i].cite || !regs[i].cite[0] || !regs[i].name) {
            fprintf(stderr, "strict register model: register %#lx has no source for its behavior\n",
                    (unsigned long)regs[i].address);
            abort();
        }
        if (regs[i].size != 1 && regs[i].size != 2 && regs[i].size != 4 && regs[i].size != 8) {
            fprintf(stderr, "strict register model: register %s has width %u\n", regs[i].name, regs[i].size);
            abort();
        }
        for (size_t j = 0; j < i; ++j)
            if (regs[j].address == regs[i].address) {
                fprintf(stderr, "strict register model: register %#lx defined twice\n", (unsigned long)regs[i].address);
                abort();
            }
    }
    table = regs;
    entries = count;
    refused = 0;
}

void strict_mmio_clear(void) { table = NULL; entries = 0; refused = 0; handler = NULL; }
void strict_mmio_set_refusal_handler(void (*h)(const char *message)) { handler = h; }
unsigned strict_mmio_refusals(void) { return refused; }

static struct strict_reg *find(uintptr_t address)
{
    for (size_t i = 0; i < entries; ++i)
        if (table[i].address == address) return &table[i];
    return NULL;
}

uint64_t strict_mmio_read(uintptr_t address, unsigned size)
{
    struct strict_reg *reg = find(address);
    if (!reg) { refuse("read of unmodeled offset %#lx (width %u)%s", address, size, ""); return 0; }
    if (reg->size != size) { refuse("read of %#lx with width %u, which differs from the modeled width%s", address, size, ""); return 0; }
    ++reg->reads;
    return reg->read ? reg->read(reg) : reg->value;
}

void strict_mmio_write(uintptr_t address, unsigned size, uint64_t value)
{
    struct strict_reg *reg = find(address);
    if (!reg) { refuse("write to unmodeled offset %#lx (width %u)%s", address, size, ""); return; }
    if (reg->size != size) { refuse("write to %#lx with width %u, which differs from the modeled width%s", address, size, ""); return; }
    ++reg->writes;
    if (reg->write) {
        if (reg->write(reg, value)) refuse("write to %#lx (width %u) of a value the model does not define%s", address, size, "");
    } else {
        reg->value = value;
    }
}
