/* Separate CPU/device memory plus an independent device page-table walker.
 * The register model exercises controller integration, not NPU arithmetic. */
#include "a733_npu_memory.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TABLE_BYTES (4096 + 5 * 16384)
#define BOOT_BYTES 524288
#define JOB_BYTES 1048576
#define DATA_BYTES 8192
#define TABLE_PA UINT64_C(0x8900000000)
#define BOOT_VA 0x10000000u
#define JOB_VA 0x10f80000u
#define INPUT_VA 0x13001000u
#define OUTPUT_VA 0x13fff000u
#define IDLE 0x7fffffffu
static _Alignas(4096) unsigned char arena[TABLE_BYTES], device_arena[TABLE_BYTES];
static _Alignas(4096) unsigned char boot_cpu[BOOT_BYTES], boot_device[BOOT_BYTES];
static _Alignas(4096) unsigned char job_cpu[JOB_BYTES], job_device[JOB_BYTES];
static _Alignas(4096) unsigned char input_cpu[DATA_BYTES], input_device[DATA_BYTES];
static _Alignas(4096) unsigned char output_guard[DATA_BYTES + 8192], output_device[DATA_BYTES];
#define OUTPUT_CPU (output_guard + 4096)
static struct a733_npu_memory memory;
static struct a733_npu_buffer input[4], maps[4];
static unsigned char *device_copies[4];
static struct a733_npu_command boot, job;
static struct a733_npu_controller controller;

struct model {
    unsigned translations, cache_calls, barriers, cache_failure, ordered, cleaned;
    uintptr_t bad_cpu;
    unsigned bad_kind;
    uint32_t regs[0x800 / 4];
    uint64_t now;
    unsigned pending, triggers;
};
static struct model model;

static int translate(void *cookie, uintptr_t cpu, int write, struct aarch64_translation *out)
{
    struct model *d = cookie;
    ++d->translations;
    uint64_t pa = 0;
    int writable = 0, found = 0;
    if (cpu >= (uintptr_t)arena && cpu - (uintptr_t)arena < sizeof(arena)) {
        pa = TABLE_PA + cpu - (uintptr_t)arena; writable = 1; found = 1;
    }
    for (unsigned i = 0; i < 4; ++i)
        if (cpu >= maps[i].cpu && cpu - maps[i].cpu < maps[i].bytes) {
            pa = maps[i].physical + cpu - maps[i].cpu;
            writable = maps[i].role == A733_NPU_BUFFER_WRITE; found = 1;
        }
    if (!found || (write && !writable)) return 0;
    *out = (struct aarch64_translation){pa, 0xff, 3};
    if (cpu == d->bad_cpu) {
        if (d->bad_kind == 1 || (d->bad_kind == 2 && write)) return 0;
        if (d->bad_kind == 3) out->physical += 4096;
        if (d->bad_kind == 4) out->attributes = 0x44;
        if (d->bad_kind == 5) out->shareability = 2;
    }
    return 1;
}
static int cache(void *cookie, enum aarch64_cache_op op, uintptr_t cpu, size_t bytes)
{
    struct model *d = cookie;
    assert(!(cpu & 63) && !(bytes & 63) && bytes);
    if (++d->cache_calls == d->cache_failure) return 0;
    d->ordered = 0;
    if (cpu == (uintptr_t)arena) {
        assert(op == AARCH64_CACHE_CLEAN && bytes <= sizeof(device_arena));
        d->cleaned |= 16; memcpy(device_arena, arena, bytes); return 1;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (cpu != maps[i].cpu) continue;
        assert(bytes == maps[i].bytes); d->cleaned |= 1u << i;
        if (maps[i].role == A733_NPU_BUFFER_WRITE)
            assert(op == AARCH64_CACHE_CLEAN_INVALIDATE || op == AARCH64_CACHE_INVALIDATE);
        else assert(op == AARCH64_CACHE_CLEAN);
        if (op == AARCH64_CACHE_INVALIDATE) memcpy((void *)cpu, device_copies[i], bytes);
        else memcpy(device_copies[i], (void *)cpu, bytes);
        return 1;
    }
    assert(0); return 0;
}
static void barrier(void *cookie)
{
    struct model *d = cookie; ++d->barriers; d->ordered = 1;
}
static const struct a733_npu_memory_ops ops = {translate, cache, barrier};

static void prepare(void)
{
    memset(&model, 0, sizeof(model)); memset(&memory, 0, sizeof(memory));
    memset(&controller, 0, sizeof(controller));
    memset(arena, 0xa5, sizeof(arena)); memset(device_arena, 0xcc, sizeof(device_arena));
    memset(boot_cpu, 0, sizeof(boot_cpu)); memset(job_cpu, 0x37, sizeof(job_cpu));
    memset(input_cpu, 0x12, sizeof(input_cpu)); memset(input_device, 0xe1, sizeof(input_device));
    memset(output_guard, 0x69, sizeof(output_guard)); memset(OUTPUT_CPU, 0xa5, DATA_BYTES);
    memset(output_device, 0xe2, sizeof(output_device));
    /* Intentionally unsorted. Both a command and a data range cross roots. */
    input[0] = (struct a733_npu_buffer){(uintptr_t)OUTPUT_CPU, UINT64_C(0x9400000000),
        DATA_BYTES, OUTPUT_VA, A733_NPU_BUFFER_WRITE};
    input[1] = (struct a733_npu_buffer){(uintptr_t)job_cpu, UINT64_C(0x9200000000),
        JOB_BYTES, JOB_VA, A733_NPU_BUFFER_COMMAND};
    input[2] = (struct a733_npu_buffer){(uintptr_t)input_cpu, UINT64_C(0x9300000000),
        DATA_BYTES, INPUT_VA, A733_NPU_BUFFER_READ};
    input[3] = (struct a733_npu_buffer){(uintptr_t)boot_cpu, UINT64_C(0x9100000000),
        BOOT_BYTES, BOOT_VA, A733_NPU_BUFFER_BOOTSTRAP};
    memcpy(maps, input, sizeof(maps));
    device_copies[0] = output_device; device_copies[1] = job_device;
    device_copies[2] = input_device; device_copies[3] = boot_device;
    assert(a733_npu_bootstrap(boot_cpu, sizeof(boot_cpu)));
    model.regs[0x20/4] = 0x9000; model.regs[0x24/4] = 0x9202;
    model.regs[0x30/4] = 0x1000003b; model.regs[0x28/4] = 0x20240101;
    model.regs[1] = IDLE;
}
static void initialize(void)
{
    assert(a733_npu_memory_init(&memory, arena, sizeof(arena), TABLE_PA, input, 4, &ops, &model));
    assert(a733_npu_memory_command(&memory, BOOT_VA, &boot));
    assert(a733_npu_memory_command(&memory, JOB_VA, &job));
    assert(memory.image.tables == 4 && memory.image.bytes == 4096 + 4 * 16384);
    assert(!model.cache_calls && !model.barriers);
}
static void fresh(void) { prepare(); initialize(); }

/* This walker resolves bus addresses through the device's copy of the tables.
 * It deliberately does not consult the memory policy's copied buffer list. */
static uint32_t load32(const unsigned char *p)
{
    return p[0] + (uint32_t)p[1] * 256 + (uint32_t)p[2] * 65536 + (uint32_t)p[3] * 16777216;
}
static unsigned char *bus(uint64_t pa, size_t bytes)
{
    if (pa >= TABLE_PA && pa - TABLE_PA <= sizeof(device_arena) &&
        bytes <= sizeof(device_arena) - (pa - TABLE_PA)) return device_arena + (pa - TABLE_PA);
    for (unsigned i = 0; i < 4; ++i)
        if (pa >= maps[i].physical && pa - maps[i].physical <= maps[i].bytes &&
            bytes <= maps[i].bytes - (pa - maps[i].physical))
            return device_copies[i] + (pa - maps[i].physical);
    return NULL;
}
static unsigned char *walk(uint32_t va, int write)
{
    uint32_t root = load32(device_arena + (va >> 24) * 4);
    if (!(root & 1)) return NULL;
    assert((root & 15) == 1); /* This publisher selects only 4 KiB pages. */
    uint64_t table = (TABLE_PA & UINT64_C(0xff00000000)) | (root & 0xffffffc0u);
    unsigned char *entry = bus(table + ((va >> 12) & 4095) * 4, 4);
    assert(entry);
    uint32_t leaf = load32(entry);
    if (!(leaf & 1) || (write && !(leaf & 4))) return NULL;
    uint64_t pa = (leaf & 0xfffff000u) | ((uint64_t)(leaf & 0xff0u) << 28) | (va & 4095);
    return bus(pa, 1);
}
static int reg_read(void *cookie, unsigned offset, uint32_t *out)
{
    struct model *d = cookie; assert(offset < sizeof(d->regs));
    *out = d->regs[offset / 4];
    if (offset == 0x10) d->regs[offset / 4] = 0;
    return 1;
}
static int reg_write(void *cookie, unsigned offset, uint32_t value)
{
    struct model *d = cookie; assert(offset < sizeof(d->regs));
    d->regs[offset / 4] = value;
    if (offset == 0x3a8 && (value & 1)) d->regs[0x388/4] = 0;
    if (offset == 0x3a4) {
        assert(d->ordered && memory.state == A733_NPU_MEMORY_DEVICE);
        assert(d->regs[0x3b4/4] == memory.image.pd_entry);
        uint32_t command = d->regs[0x654/4];
        assert(walk(command, 0) && walk(command + A733_NPU_FETCH_BYTES - 1, 0));
        assert(!walk(command, 1));
        d->pending = 1; d->regs[1] = 0; ++d->triggers;
    }
    return 1;
}
static uint64_t time_us(void *cookie) { return ((struct model *)cookie)->now; }
static void delay(void *cookie, unsigned us) { ((struct model *)cookie)->now += us; }
static int publish(void *cookie, const struct a733_npu_mmu_image *image,
                   const struct a733_npu_command *cmd, int bootstrap)
{
    assert(cookie == &model);
    return a733_npu_memory_publish(&memory, image, cmd, bootstrap);
}
static int finish(void *cookie, const struct a733_npu_command *cmd, int canceled)
{
    struct model *d = cookie; assert(d->regs[1] == IDLE && !d->pending && !canceled);
    return a733_npu_memory_finish(&memory, cmd);
}
static const struct a733_npu_ops controller_ops = {
    reg_read, reg_write, time_us, delay, barrier, publish, finish
};
static void complete_device(int arithmetic)
{
    assert(model.pending);
    if (arithmetic) for (uint32_t i = 0; i < DATA_BYTES; ++i) {
        unsigned char *in = walk(INPUT_VA + i, 0), *out = walk(OUTPUT_VA + i, 1);
        assert(in && out); *out = *in ^ 0x5a;
    }
    model.regs[0x10/4] = 1;
}
static void drain_device(void) { model.pending = 0; model.regs[1] = IDLE; }
static void guards(void)
{
    for (unsigned i = 0; i < 4096; ++i)
        assert(output_guard[i] == 0x69 && output_guard[4096 + DATA_BYTES + i] == 0x69);
}
static void retained(void)
{
    assert(memory.state == A733_NPU_MEMORY_FAULT);
    unsigned before = model.translations + model.cache_calls + model.barriers;
    assert(!a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
    assert(!a733_npu_memory_finish(&memory, &boot));
    assert(!a733_npu_memory_command(&memory, JOB_VA, &job));
    assert(!a733_npu_memory_init(&memory, arena, sizeof(arena), TABLE_PA, input, 4, &ops, &model));
    assert(before == model.translations + model.cache_calls + model.barriers);
    guards();
}
static void refused_init(void *at, size_t bytes, uint64_t pa)
{
    struct a733_npu_memory original = memory;
    assert(!a733_npu_memory_init(&memory, at, bytes, pa, input, 4, &ops, &model));
    assert(!memcmp(&memory, &original, sizeof(memory)));
    for (size_t i = 0; i < sizeof(arena); ++i) assert(arena[i] == 0xa5);
    assert(!model.cache_calls && !model.barriers); guards();
}

int main(void)
{
    fresh();
    /* Caller metadata can be discarded; private copies define publication. */
    memset(input, 0, sizeof(input));
    assert(a733_npu_open(&controller, &controller_ops, &model, &memory.image, &boot, 10000));
    assert(!walk(0, 0) && !walk(0x400000, 0) && !walk(INPUT_VA - 4096, 0));
    for (unsigned i = 0; i < 4; ++i) {
        assert(walk(maps[i].device, 0) == device_copies[i]);
        assert(walk(maps[i].device + maps[i].bytes - 1, 0) == device_copies[i] + maps[i].bytes - 1);
    }
    complete_device(0); drain_device();
    assert(a733_npu_poll(&controller) == A733_NPU_READY);
    memset(input_cpu, 0x7d, DATA_BYTES);
    assert(a733_npu_submit(&controller, &job, 10000));
    assert(input_device[0] == 0x7d);
    complete_device(1);
    unsigned before = model.cache_calls;
    assert(a733_npu_poll(&controller) == A733_NPU_RUNNING);
    assert(model.cache_calls == before && OUTPUT_CPU[0] == 0xa5);
    drain_device(); assert(a733_npu_poll(&controller) == A733_NPU_DONE);
    for (unsigned i = 0; i < DATA_BYTES; ++i) assert(OUTPUT_CPU[i] == (0x7d ^ 0x5a));
    void *token = NULL; int canceled = -1;
    assert(a733_npu_reap(&controller, &token, &canceled) && token == job.token && !canceled);
    assert(a733_npu_close(&controller)); guards();

    /* The bootstrap publication maintains the tables and every buffer; a job
     * publication maintains only READ and WRITE buffers. A CONSTANT input is
     * published once and is not cleaned again, so a later CPU write stays
     * invisible to the device model. */
    for (unsigned constant = 0; constant < 2; ++constant) {
        prepare();
        if (constant) input[2].role = maps[2].role = A733_NPU_BUFFER_CONSTANT;
        initialize();
        assert(a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
        assert(model.cleaned == 31 && model.cache_calls == 5);
        assert(a733_npu_memory_finish(&memory, &boot));
        memset(input_cpu, 0x3c, DATA_BYTES); model.cleaned = 0; model.cache_calls = 0;
        unsigned translations = model.translations;
        assert(a733_npu_memory_publish(&memory, &memory.image, &job, 0));
        assert(model.cleaned == (constant ? 1u : 5u) && model.cache_calls == (constant ? 1u : 2u));
        assert(model.translations > translations); /* Mappings are still checked per job. */
        assert(input_device[0] == (constant ? 0x12 : 0x3c));
        assert(a733_npu_memory_finish(&memory, &job));
    }
    /* Session groups limit job maintenance to the command's own group plus
     * shared group 0. Bootstrap publication and finish still cover every
     * buffer. Another session's WRITE buffer keeps its CPU contents. */
    for (unsigned shared = 0; shared < 2; ++shared) {
        fresh();
        assert(a733_npu_memory_group(&memory, JOB_VA, 1) && a733_npu_memory_group(&memory, INPUT_VA, 1));
        assert(a733_npu_memory_group(&memory, OUTPUT_VA, shared ? 1 : 2));
        assert(a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
        assert(model.cleaned == 31 && model.cache_calls == 5);
        model.cache_calls = 0;
        assert(a733_npu_memory_finish(&memory, &boot) && model.cache_calls == 1);
        assert(!a733_npu_memory_group(&memory, OUTPUT_VA, 1)); /* Fixed once published. */
        memset(OUTPUT_CPU, 0x44, DATA_BYTES); memset(input_cpu, 0x3c, DATA_BYTES);
        model.cleaned = 0; model.cache_calls = 0;
        assert(a733_npu_memory_publish(&memory, &memory.image, &job, 0));
        assert(model.cleaned == (shared ? 5u : 4u) && model.cache_calls == (shared ? 2u : 1u));
        assert(input_device[0] == 0x3c && output_device[0] == (shared ? 0x44 : 0xa5));
        memset(output_device, 0x21, DATA_BYTES); model.cache_calls = 0;
        assert(a733_npu_memory_finish(&memory, &job) && model.cache_calls == shared);
        assert(OUTPUT_CPU[0] == (shared ? 0x21 : 0x44));
        guards();
    }
    fresh();
    assert(!a733_npu_memory_group(&memory, JOB_VA, 0) && !a733_npu_memory_group(&memory, JOB_VA, 256));
    assert(!a733_npu_memory_group(&memory, BOOT_VA, 1) && !a733_npu_memory_group(&memory, INPUT_VA + 4096, 1));
    assert(!a733_npu_memory_group(NULL, JOB_VA, 1));
    for (unsigned i = 0; i < 4; ++i) assert(!memory.groups[i]);
    memory.state = A733_NPU_MEMORY_FAULT; assert(!a733_npu_memory_group(&memory, JOB_VA, 1));

    /* A job publication before any bootstrap publication is refused. */
    fresh(); assert(!a733_npu_memory_publish(&memory, &memory.image, &job, 0));
    assert(!model.cache_calls); retained();

    /* CPU translations can change after admission, including interior pages. */
    for (unsigned kind = 1; kind <= 5; ++kind) {
        fresh(); model.bad_cpu = (uintptr_t)OUTPUT_CPU + 4096; model.bad_kind = kind;
        assert(!a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
        assert(!model.cache_calls); retained();
    }
    fresh(); model.bad_cpu = (uintptr_t)arena + sizeof(arena) - 4096; model.bad_kind = 3;
    assert(!a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
    assert(!model.cache_calls); retained();

    /* Every table component is checked, including otherwise unused bytes. */
    const size_t corruptions[] = {0, 1024, 4096 + 4095 * 4,
        4096, 4096 + 2, 4096 + 16384, 4096 + 3 * 16384};
    for (unsigned i = 0; i < sizeof(corruptions) / sizeof(corruptions[0]); ++i) {
        fresh(); arena[corruptions[i]] ^= 4;
        assert(!a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
        assert(!model.cache_calls); retained();
    }
    fresh(); struct a733_npu_mmu_image wrong_image = memory.image; wrong_image.physical += 4096;
    assert(!a733_npu_memory_publish(&memory, &wrong_image, &boot, 1)); retained();
    fresh(); struct a733_npu_command wrong = boot; wrong.device += 4096;
    assert(!a733_npu_memory_publish(&memory, &memory.image, &wrong, 1)); retained();
    fresh(); wrong = boot; --wrong.readable_bytes;
    assert(!a733_npu_memory_publish(&memory, &memory.image, &wrong, 1)); retained();
    fresh(); wrong = boot; wrong.token = &boot;
    assert(!a733_npu_memory_publish(&memory, &memory.image, &wrong, 1)); retained();
    fresh(); assert(!a733_npu_memory_publish(&memory, &memory.image, &job, 1)); retained();
    fresh(); assert(!a733_npu_memory_publish(&memory, &memory.image, &boot, 0)); retained();
    fresh(); assert(!a733_npu_memory_command(&memory, INPUT_VA, &wrong));
    assert(!a733_npu_memory_command(&memory, JOB_VA, (void *)arena));
    assert(!a733_npu_memory_command(&memory, JOB_VA, (void *)OUTPUT_CPU));
    assert(!a733_npu_memory_command(&memory, JOB_VA, &memory.active));
    prepare(); input[2].role = maps[2].role = A733_NPU_BUFFER_CONSTANT; initialize();
    assert(!a733_npu_memory_command(&memory, INPUT_VA, &wrong));

    /* Failure at each cache operation must stop subsequent maintenance. */
    for (unsigned failure = 1; failure <= 5; ++failure) {
        fresh(); model.cache_failure = failure;
        assert(!a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
        assert(model.cache_calls == failure && !model.barriers); retained();
    }
    fresh(); assert(a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
    model.cache_failure = model.cache_calls + 1;
    assert(!a733_npu_memory_finish(&memory, &boot)); retained();
    fresh(); assert(a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
    model.bad_cpu = (uintptr_t)OUTPUT_CPU + 4096; model.bad_kind = 2;
    before = model.cache_calls;
    assert(!a733_npu_memory_finish(&memory, &boot) && model.cache_calls == before); retained();
    fresh(); assert(a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
    assert(!a733_npu_memory_finish(&memory, &job)); retained();
    fresh(); assert(!a733_npu_memory_finish(&memory, &boot)); retained();
    fresh(); assert(a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
    assert(!a733_npu_memory_publish(&memory, &memory.image, &boot, 1)); retained();

    /* Admission failure is atomic, even after mapping checks have succeeded. */
    prepare(); refused_init(arena, 4096, TABLE_PA);
    prepare(); input[0].cpu = input[2].cpu; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[0].physical = input[2].physical; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[0].device = input[2].device; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[0].device = 0x400000; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[0].physical = TABLE_PA; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[0].cpu = (uintptr_t)arena; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[0].device = 0xfffff000; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[0].physical = (UINT64_C(1) << 40) - 4096; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[0].cpu = UINTPTR_MAX - 4095; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[3].role = A733_NPU_BUFFER_READ; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[1].role = A733_NPU_BUFFER_BOOTSTRAP; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[1].bytes = 4096; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[0].role = (enum a733_npu_buffer_role)-1; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); input[0].role = (enum a733_npu_buffer_role)(A733_NPU_BUFFER_CONSTANT + 1);
    refused_init(arena, sizeof(arena), TABLE_PA);
    /* CONSTANT is not a command role: it cannot stand in for a bootstrap. */
    prepare(); input[3].role = A733_NPU_BUFFER_CONSTANT; refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); model.bad_cpu = (uintptr_t)input_cpu + 4096; model.bad_kind = 1;
    refused_init(arena, sizeof(arena), TABLE_PA);
    prepare(); refused_init(arena + 64, sizeof(arena) - 64, TABLE_PA);
    prepare(); refused_init(arena, sizeof(arena), TABLE_PA + 64);
    prepare(); refused_init(arena, sizeof(arena), (UINT64_C(1) << 40) - 4096);

    /* Final virtual page and missing neighboring entries remain distinct. */
    prepare(); input[0].device = 0xffffe000; maps[0].device = input[0].device;
    initialize(); assert(a733_npu_memory_publish(&memory, &memory.image, &boot, 1));
    assert(walk(UINT32_MAX, 1) == output_device + DATA_BYTES - 1);
    assert(!walk(0xffffdfff, 0));
    assert(a733_npu_memory_finish(&memory, &boot)); guards();
    puts("PASS NPU private mappings, canonical tables, noncoherent publication, per-job and per-session cache scope, controller ownership, and fault retention");
    return 0;
}
