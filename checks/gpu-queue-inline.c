/* SPDX-License-Identifier: MIT
 * Agreement of the queue's job slot and its compiled programs, for the GRID
 * and GRID_INLINE ABIs (include/gpu_queue.h). The slot's PDS data, PDS code
 * and CDM kernel word come from the driver's own encoders
 * (drivers/gpu/pvr_compute_job.c), called in the order the queue calls them.
 * An emulator runs the ordinary task's DOUT instructions over the data the
 * CDM fetches and fills the unified registers of all 32 lanes. Each program's
 * compiler record (sources.json, passed by checks/gpu-queue-inline.py) must
 * then agree with them. The GRID record is the compiler's GRID profile
 * contract, because no program in this repository uses that ABI; the
 * GRID_INLINE records are the shipped queue programs. Agreement means the local index in the register the compiler
 * allocated, every explicitly read register written by a constant, and the
 * register count within the CDM's unified allocation. The job's binding
 * addresses and count must reach the registers the kernel source reads.
 * Slot offsets come from include/pvr_compute_queue.h through the wrapper. */
#include "pvr_compute_job.h"
#include "gpu_queue.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SLOT_FENCE
#error "SLOT_FENCE, SLOT_CDM and SLOT_ARGUMENTS come from include/pvr_compute_queue.h"
#endif
#define CODE 128u
#define LANES 32u
#define REGS 64u

static uint32_t get32(const unsigned char *p, size_t o)
{ return (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24; }
static uint64_t get64(const unsigned char *p, size_t o) { return get32(p, o) | (uint64_t)get32(p, o + 4) << 32; }

struct lanes {
    uint32_t value[LANES][REGS];
    uint64_t written;           /* registers written, the same in every lane */
    uint64_t local;             /* registers holding the local index */
    unsigned unified;           /* registers the CDM allocates */
};

static int failures, quiet;
static int fail(const char *what, unsigned value)
{
    if (!quiet) printf("disagree: %s (%u)\n", what, value);
    ++failures;
    return 0;
}

/* The ordinary task: the PDS program's first instruction branches past the
 * coefficient task to word 10; DOUTW writes unified registers, DOUTU ends. */
static int emulate(const unsigned char *slot, const unsigned char *cdm, struct lanes *out)
{
    memset(out, 0, sizeof(*out));
    const uint32_t kernel = get32(cdm, 0), pds = get32(cdm, 4);
    const unsigned data_bytes = ((kernel >> 2) & 63u) * 16u;
    out->unified = ((kernel >> 12) & 63u) * 4u;
    if (get32(cdm, 8) != pds + CODE) return fail("code address", get32(cdm, 8) - pds);
    if (data_bytes > CODE) return fail("data overlaps code", data_bytes);
    if (get32(slot, CODE) != 0xc2c0000au) return fail("coefficient branch", get32(slot, CODE));
    int last = 0;
    for (unsigned pc = 10;; ++pc) {
        if (CODE + 4 * pc + 4 > SLOT_FENCE) return fail("code passes the fence program", pc);
        const uint32_t w = get32(slot, CODE + 4 * pc);
        if (w >> 28 != 0xfu) return fail("instruction other than DOUT", w);
        const unsigned dst = w & 7u, src0 = (w >> 8) & 0x7fu, src1 = (w >> 16) & 0xffu;
        if (dst == 2u) {
            if (!(w >> 26 & 1u)) return fail("DOUTU without END", w);
            if (!last) return fail("no LAST write before the USC task", pc);
            if (src0 != 0 || data_bytes < 8) return fail("task word", src0);
            return 1;
        }
        if (dst != 1u) return fail("DOUT other than DOUTW", w);
        if (last) return fail("write after LAST", pc);
        if (4u * src1 + 4u > data_bytes) return fail("control word outside the fetched data", src1);
        const uint32_t control = get32(slot, 4u * src1);
        if (control >> 28 & 1u) return fail("write to the common store", control);
        const unsigned ao = (control >> 13) & 0x1fffu, size = control & 3u;
        last = (int)(control >> 31);
        if (size == 3u) return fail("block size", control);
        const unsigned count = size == 2u ? 2u : 1u;
        if (ao + count > out->unified) return fail("register past the unified allocation", ao + count);
        for (unsigned r = ao; r < ao + count; ++r)
            if (out->written >> r & 1u) return fail("register written twice", r);
        for (unsigned lane = 0; lane < LANES; ++lane) {
            uint64_t v;
            if (src0 == 0x40u) v = lane;                  /* the local-index temp */
            else if (src0 < 0x40u) {
                if (8u * src0 + 8u > data_bytes) return fail("constant outside the fetched data", src0);
                v = get64(slot, 8u * src0);
            } else return fail("source operand", src0);
            if (size == 1u) v >>= 32;
            out->value[lane][ao] = (uint32_t)v;
            if (count == 2u) out->value[lane][ao + 1] = (uint32_t)(v >> 32);
        }
        for (unsigned r = ao; r < ao + count; ++r) out->written |= UINT64_C(1) << r;
        if (src0 == 0x40u) {
            if (count != 1u || size != 0u) return fail("local index width", control);
            out->local |= UINT64_C(1) << ao;
        }
    }
}

struct record { unsigned abi, local_start, local_count, mask, vtxins; };

/* The compiler record against the emulated registers. */
static int agree(const struct record *p, const struct lanes *l)
{
    int ok = 1;
    if (l->local != UINT64_C(1) << p->local_start || p->local_count != 1)
        ok = fail("local index register", p->local_start);
    if ((p->mask & ~l->written) || (p->mask & l->local)) ok = fail("explicit register not written by a constant", p->mask);
    if (p->vtxins > l->unified) ok = fail("compiler registers past the unified allocation", p->vtxins);
    if (p->vtxins != p->local_start + 1) ok = fail("register count", p->vtxins);
    return ok;
}

static uint64_t pair(const struct lanes *l, unsigned r) { return l->value[7][r] | (uint64_t)l->value[7][r + 1] << 32; }

/* One slot as the queue writes it: creation, then each job. */
struct slot { unsigned char bytes[512]; };
static void create(struct slot *s, unsigned abi, uint64_t slot_gpu, uint64_t binding0)
{
    const uint32_t zero[3] = {0, 0, 0};
    memset(s, 0, sizeof(*s));
    pvr_compute_job_grid_pds_abi(s->bytes, abi, 0x1234ull << 4, zero,
                                 abi == GPU_QUEUE_ABI_GRID_INLINE ? 0 : slot_gpu + SLOT_ARGUMENTS, binding0);
}
static int job(struct slot *s, unsigned abi, uint64_t slot_gpu, const struct gpu_queue_lease_info *lease,
               const struct gpu_queue_job *j)
{
    if (abi == GPU_QUEUE_ABI_GRID_INLINE) {
        if (!pvr_compute_job_grid_inline(s->bytes, lease, j)) return 0;
    } else {
        for (unsigned i = 1; i < j->bindings; ++i) {
            uint64_t a = lease[j->binding[i].lease].address + j->binding[i].address;
            memcpy(s->bytes + SLOT_ARGUMENTS + 8 * (i - 1), &a, 8);
        }
        memcpy(s->bytes + SLOT_ARGUMENTS + 8 * (j->bindings - 1), j->arguments, j->argument_bytes);
    }
    const uint32_t pds = (uint32_t)(slot_gpu & 0xffffffffu);
    if (!pvr_compute_job_grid_cdm_data(s->bytes + SLOT_CDM, pds, pds + SLOT_FENCE, j->groups, 7,
                                       pvr_compute_job_pds_data_units(abi))) return fail("CDM refused", abi);
    return 1;
}

static struct record parse(char **argv)
{
    struct record r;
    r.abi = (unsigned)strtoul(argv[0], NULL, 0); r.local_start = (unsigned)strtoul(argv[1], NULL, 0);
    r.local_count = (unsigned)strtoul(argv[2], NULL, 0); r.mask = (unsigned)strtoul(argv[3], NULL, 0);
    r.vtxins = (unsigned)strtoul(argv[4], NULL, 0);
    return r;
}

int main(int argc, char **argv)
{
    /* argv: the GRID contract record, then one or more GRID_INLINE program records, five words each:
     * abi local_start local_count mask vtxins */
    if (argc < 11 || (argc - 1) % 5) return 2;
    const unsigned programs = (unsigned)(argc - 6) / 5;
    struct record inline_records[8];
    if (programs > sizeof(inline_records) / sizeof(inline_records[0])) return 2;
    const struct record grid = parse(argv + 1);
    assert(grid.abi == GPU_QUEUE_ABI_GRID);
    for (unsigned i = 0; i < programs; ++i) {
        inline_records[i] = parse(argv + 6 + 5 * i);
        assert(inline_records[i].abi == GPU_QUEUE_ABI_GRID_INLINE);
    }
    const struct gpu_queue_lease_info lease[2] = {{.address = 0x4800000000ull, .bytes = 4u << 20},
                                                  {.address = 0x4900000000ull, .bytes = 2u << 20}};
    const uint64_t slot_gpu = 0xda00000400ull;
    struct gpu_queue_job j = {.bindings = 2, .argument_bytes = 8, .groups = {2048, 1, 1},
        .binding = {{.lease = 0, .address = 0x1000}, {.lease = 1, .address = 0x40}}};
    const uint32_t count = 262144u, mode = 1;
    memcpy(j.arguments, &count, 4);
    memcpy(j.arguments + 4, &mode, 4);
    const uint64_t binding0 = lease[0].address + 0x1000, binding1 = lease[1].address + 0x40;
    struct slot s;
    struct lanes l;
    unsigned cases = 0;

    /* GRID: the block address and binding 0 in registers 0-3, the local index in 4; the block at byte 320. */
    create(&s, GPU_QUEUE_ABI_GRID, slot_gpu, binding0);
    if (job(&s, GPU_QUEUE_ABI_GRID, slot_gpu, lease, &j) && emulate(s.bytes, s.bytes + SLOT_CDM, &l) && agree(&grid, &l)) {
        if (pair(&l, 0) != slot_gpu + SLOT_ARGUMENTS || pair(&l, 2) != binding0 ||
            get64(s.bytes, SLOT_ARGUMENTS) != binding1 || get32(s.bytes, SLOT_ARGUMENTS + 8) != count ||
            get32(s.bytes, SLOT_ARGUMENTS + 12) != mode)
            fail("GRID job registers", 0);
        for (unsigned lane = 0; lane < LANES; ++lane)
            if (l.value[lane][grid.local_start] != lane) fail("GRID local index", lane);
        ++cases;
    }
    /* Every inline program on this slot must disagree. */
    for (unsigned i = 0; i < programs; ++i) {
        const int before = failures;
        quiet = 1;
        int ok = emulate(s.bytes, s.bytes + SLOT_CDM, &l) && agree(&inline_records[i], &l);
        quiet = 0;
        if (ok || failures == before) { fail("inline program accepted on a GRID slot", i); }
        else { failures = before; ++cases; }
    }

    /* GRID_INLINE: block bytes 0-7 in 0-1, binding 0 in 2-3, bytes 8-15 in 4-5, the local index in 6. */
    create(&s, GPU_QUEUE_ABI_GRID_INLINE, slot_gpu, binding0);
    /* The shipped programs read binding 1 from 0-1, the output from 2-3, the count from 4, and the counter
     * build's add its mode from 5. */
    if (job(&s, GPU_QUEUE_ABI_GRID_INLINE, slot_gpu, lease, &j) && emulate(s.bytes, s.bytes + SLOT_CDM, &l)) {
        if (pair(&l, 0) != binding1 || pair(&l, 2) != binding0 || l.value[7][4] != count || l.value[7][5] != 1)
            fail("GRID_INLINE job registers", 0);
        for (unsigned i = SLOT_ARGUMENTS; i < sizeof(s.bytes); ++i)
            if (s.bytes[i]) fail("GRID_INLINE wrote the argument block", i);
        for (unsigned i = 0; i < programs; ++i) {
            if (!agree(&inline_records[i], &l)) continue;
            for (unsigned lane = 0; lane < LANES; ++lane)
                if (l.value[lane][inline_records[i].local_start] != lane) fail("GRID_INLINE local index", lane);
            ++cases;
        }
    }
    {
        const int before = failures;
        quiet = 1;
        int ok = emulate(s.bytes, s.bytes + SLOT_CDM, &l) && agree(&grid, &l);   /* the GRID contract */
        quiet = 0;
        if (ok || failures == before) { fail("GRID program accepted on an inline slot", 0); }
        else { failures = before; ++cases; }
    }

    /* A second job with another input and count, binding 0 unchanged: the queue rewrites only the block. */
    struct gpu_queue_job k = j;
    k.binding[1].address = 0x80040;
    const uint32_t count2 = 65536u, mode2 = 0;
    memcpy(k.arguments, &count2, 4);
    memcpy(k.arguments + 4, &mode2, 4);
    if (job(&s, GPU_QUEUE_ABI_GRID_INLINE, slot_gpu, lease, &k) && emulate(s.bytes, s.bytes + SLOT_CDM, &l) &&
        agree(&inline_records[0], &l)) {
        if (pair(&l, 0) != lease[1].address + 0x80040 || pair(&l, 2) != binding0 || l.value[7][4] != count2 ||
            l.value[7][5] != mode2)
            fail("second GRID_INLINE job registers", 0);
        ++cases;
    }

    /* Three bindings and no arguments fill the 16 bytes; one more byte is refused without a write. */
    struct gpu_queue_job three = j;
    three.bindings = 3; three.argument_bytes = 0;
    three.binding[2] = (struct gpu_queue_binding){.lease = 1, .address = 0x100};
    if (job(&s, GPU_QUEUE_ABI_GRID_INLINE, slot_gpu, lease, &three) && emulate(s.bytes, s.bytes + SLOT_CDM, &l)) {
        if (pair(&l, 0) != binding1 || pair(&l, 4) != lease[1].address + 0x100) fail("three-binding block", 0);
        ++cases;
    }
    struct slot saved = s;
    three.argument_bytes = 1;
    struct gpu_queue_job long_args = j;
    long_args.bindings = 1; long_args.argument_bytes = 17;
    if (pvr_compute_job_grid_inline(s.bytes, lease, &three) || pvr_compute_job_grid_inline(s.bytes, lease, &long_args) ||
        memcmp(&s, &saved, sizeof(s))) fail("17-byte block accepted", 0);
    else ++cases;
    long_args.argument_bytes = 16;
    if (!pvr_compute_job_grid_inline(s.bytes, lease, &long_args)) fail("16-byte block refused", 0);
    else ++cases;

    /* CDM data sizes: 7 and 8 units only. */
    unsigned char cdm[64], before[64];
    memset(cdm, 0xa5, sizeof(cdm)); memcpy(before, cdm, sizeof(cdm));
    if (pvr_compute_job_grid_cdm_data(cdm, 0, 0, j.groups, 7, 6) || pvr_compute_job_grid_cdm_data(cdm, 0, 0, j.groups, 7, 9) ||
        memcmp(cdm, before, sizeof(cdm))) fail("CDM data size outside 7-8 accepted", 0);
    else ++cases;
    if (pvr_compute_job_pds_data_units(GPU_QUEUE_ABI_GRID) != 7 || pvr_compute_job_pds_data_units(GPU_QUEUE_ABI_LOCAL) != 7 ||
        pvr_compute_job_pds_data_units(GPU_QUEUE_ABI_GRID_INLINE) != 8) fail("PDS data units", 0);
    else ++cases;

    printf("%s: %u cases, %d disagreements\n", failures ? "FAIL" : "PASS", cases, failures);
    return failures ? 1 : 0;
}
