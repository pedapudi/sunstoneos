#include "pvr_ccb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_BYTES = PVR_CCB_HOST_SLOTS * PVR_CCB_COMMAND_BYTES };
struct model {
    unsigned char cpu[5][MAX_BYTES], device[5][MAX_BYTES], dirty[5][MAX_BYTES];
    unsigned calls, fail_at, kicks, releases, acquires;
    unsigned event_read, last_release;
    unsigned char expected[PVR_CCB_COMMAND_BYTES];
};
static struct model m, pristine;
static struct pvr_ccb q, fresh;
static uint32_t word(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put(unsigned char *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (8 * i));
}
static int operation(struct model *s)
{
    return ++s->calls != s->fail_at;
}
static int bounds(enum pvr_ccb_region r, uint32_t off, size_t n)
{
    size_t size = pvr_ccb_region_bytes(r);
    return size && off <= size && n <= size - off;
}
static int read_memory(void *context, enum pvr_ccb_region r, uint32_t off, void *out, size_t n)
{
    struct model *s = context;
    assert(bounds(r, off, n));
    if (n == 4) assert(!(off & 3));
    if (!operation(s)) return 0;
    memcpy(out, s->cpu[r] + off, n);
    if (r == PVR_CCB_FIRMWARE_COMMANDS) s->event_read = s->calls;
    return 1;
}
static int write_memory(void *context, enum pvr_ccb_region r, uint32_t off, const void *in, size_t n)
{
    struct model *s = context;
    assert(bounds(r, off, n));
    if (n == 4) assert(!(off & 3));
    if (r == PVR_CCB_HOST_CONTROL && off == 0 && n == 4) {
        unsigned slot = (word(in) - 1) & 127;
        assert(!memcmp(s->device[PVR_CCB_HOST_COMMANDS] + slot * 88, s->expected, 88));
        assert(word(s->device[PVR_CCB_RETURNS] + slot * 4) == 0);
    }
    if (r == PVR_CCB_FIRMWARE_CONTROL && off == 128 && n == 4)
        assert(s->event_read && s->last_release > s->event_read);
    if (!operation(s)) {
        /* A failed operation is allowed to have changed device-visible memory. */
        memcpy(s->device[r] + off, in, n);
        return 0;
    }
    memcpy(s->cpu[r] + off, in, n);
    memset(s->dirty[r] + off, 1, n);
    return 1;
}
static int synchronize(void *context, enum pvr_ccb_barrier kind)
{
    struct model *s = context;
    if (!operation(s)) return 0;
    if (kind == PVR_CCB_DEVICE_COMPLETE) return 1;
    if (kind == PVR_CCB_ACQUIRE) ++s->acquires;
    if (kind == PVR_CCB_RELEASE) { ++s->releases; s->last_release = s->calls; }
    for (unsigned r = 0; r < 5; ++r) {
        for (size_t i = 0; i < pvr_ccb_region_bytes((enum pvr_ccb_region)r); ++i) {
            if (kind == PVR_CCB_ACQUIRE && !s->dirty[r][i]) s->cpu[r][i] = s->device[r][i];
            if (kind == PVR_CCB_RELEASE && s->dirty[r][i]) {
                s->device[r][i] = s->cpu[r][i]; s->dirty[r][i] = 0;
            }
        }
    }
    return 1;
}
static int kick(void *context, uint32_t offset, uint32_t value)
{
    struct model *s = context;
    assert(offset == 0xb00 && value == 0);
    if (!operation(s)) return 0;
    unsigned next = word(s->device[PVR_CCB_HOST_CONTROL]), slot = (next - 1) & 127;
    assert(!memcmp(s->device[PVR_CCB_HOST_COMMANDS] + slot * 88, s->expected, 88));
    assert(word(s->device[PVR_CCB_RETURNS] + slot * 4) == 0);
    for (unsigned r = 0; r < 3; ++r)
        for (size_t i = 0; i < pvr_ccb_region_bytes((enum pvr_ccb_region)r); ++i) assert(!s->dirty[r][i]);
    ++s->kicks;
    return 1;
}
static const struct pvr_ccb_ops ops = {read_memory, write_memory, synchronize, kick};
static void restore(void)
{
    m = pristine; q = fresh; q.context = &m;
    m.calls = m.kicks = m.acquires = m.releases = 0;
    m.fail_at = 0;
}
static void complete(struct pvr_ccb_token token, uint32_t status)
{
    put(m.device[PVR_CCB_RETURNS] + token.slot * 4, status);
    put(m.device[PVR_CCB_HOST_CONTROL] + 128, (token.slot + 1) & 127);
}
static void event(uint32_t type, unsigned byte)
{
    unsigned w = word(m.device[PVR_CCB_FIRMWARE_CONTROL]);
    unsigned char *p = m.device[PVR_CCB_FIRMWARE_COMMANDS] + w * 88;
    memset(p, (int)byte, 88); put(p, type); put(p + 4, 0);
    put(m.device[PVR_CCB_FIRMWARE_CONTROL], (w + 1) & 31);
}
static void save(const char *dir, unsigned r)
{
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/region-%u.bin", dir, r);
    assert(n > 0 && (size_t)n < sizeof(path));
    FILE *f = fopen(path, "wb");
    size_t bytes = pvr_ccb_region_bytes((enum pvr_ccb_region)r);
    assert(f && fwrite(m.device[r], 1, bytes, f) == bytes && !fclose(f));
}
static void stopped(void)
{
    assert(q.phase == PVR_CCB_FAILED);
    unsigned calls = m.calls;
    unsigned char output[88]; struct pvr_ccb_token token;
    assert(pvr_ccb_submit(&q, m.expected, 1, 10, &token) == PVR_CCB_FAULT);
    assert(pvr_ccb_poll(&q, 1) == PVR_CCB_FAULT);
    assert(pvr_ccb_receive(&q, output) == PVR_CCB_FAULT);
    assert(pvr_ccb_start(&q, 1) == PVR_CCB_FAULT);
    assert(pvr_ccb_init(&q, &ops, &m, 2) == PVR_CCB_INVALID);
    assert(calls == m.calls);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(pvr_ccb_start(&q, 0) == PVR_CCB_INVALID);
    assert(pvr_ccb_init(&q, &ops, &m, 0) == PVR_CCB_INVALID && !m.calls);
    struct pvr_ccb_ops bad_ops = ops; bad_ops.barrier = NULL;
    assert(pvr_ccb_init(&q, &bad_ops, &m, 1) == PVR_CCB_INVALID && !m.calls);
    assert(pvr_ccb_init(&q, &ops, &m, 1) == PVR_CCB_OK);
    assert(q.phase == PVR_CCB_PREPARED);
    assert(word(m.device[0] + 132) == 127 && word(m.device[3] + 132) == 31);
    assert(word(m.device[0] + 136) == 88 && word(m.device[3] + 136) == 88);
    assert(pvr_ccb_start(&q, 0) == PVR_CCB_OK);
    pristine = m; fresh = q; restore();
    put(m.expected, PVR_CCB_HEALTH);
    pristine = m;

    struct pvr_ccb_token token, before;
    memset(&token, 0xa5, sizeof(token)); before = token;
    assert(pvr_ccb_submit(&q, m.expected, 0, 0, &token) == PVR_CCB_EXPIRED);
    assert(!memcmp(&token, &before, sizeof(token)) && !m.calls);
    unsigned char invalid[88] = {0};
    assert(pvr_ccb_submit(&q, invalid, 0, 10, &token) == PVR_CCB_INVALID);
    assert(pvr_ccb_submit(&q, m.expected, 0, 10, (struct pvr_ccb_token *)&q) == PVR_CCB_INVALID);
    assert(!m.calls);
    assert(pvr_ccb_submit(&q, m.expected, 1, 100, &token) == PVR_CCB_OK);
    unsigned submit_calls = m.calls;
    assert(token.epoch == 1 && token.serial == 1 && token.slot == 0 && m.kicks == 1);
    for (unsigned r = 0; r < 5; ++r) save(argv[1], r);
    uint32_t status = 0xa5a5a5a5;
    assert(pvr_ccb_collect(&q, token, &status) == PVR_CCB_AGAIN && status == 0xa5a5a5a5);
    assert(pvr_ccb_poll(&q, 2) == PVR_CCB_OK);
    complete(token, 7);
    assert(pvr_ccb_poll(&q, 3) == PVR_CCB_OK);
    assert(pvr_ccb_collect(&q, token, &status) == PVR_CCB_OK && status == 7);
    assert(pvr_ccb_collect(&q, token, &status) == PVR_CCB_INVALID);
    before = token; before.epoch++;
    assert(pvr_ccb_collect(&q, before, &status) == PVR_CCB_INVALID);

    /* Ring capacity, wrap, retained return slots and stale tokens. */
    restore();
    struct pvr_ccb_token tokens[128];
    for (unsigned i = 0; i < 127; ++i)
        assert(pvr_ccb_submit(&q, m.expected, 1, 100, &tokens[i]) == PVR_CCB_OK);
    unsigned kicks = m.kicks;
    assert(pvr_ccb_submit(&q, m.expected, 1, 100, &token) == PVR_CCB_AGAIN && m.kicks == kicks);
    for (unsigned i = 0; i < 127; ++i) complete(tokens[i], 1);
    assert(pvr_ccb_poll(&q, 2) == PVR_CCB_OK);
    assert(pvr_ccb_submit(&q, m.expected, 2, 100, &tokens[127]) == PVR_CCB_OK);
    complete(tokens[127], 1);
    assert(pvr_ccb_poll(&q, 3) == PVR_CCB_OK);
    assert(pvr_ccb_submit(&q, m.expected, 3, 100, &token) == PVR_CCB_AGAIN);
    assert(pvr_ccb_collect(&q, tokens[0], &status) == PVR_CCB_OK);
    assert(pvr_ccb_submit(&q, m.expected, 3, 100, &token) == PVR_CCB_OK);
    assert(token.slot == 0 && token.serial != tokens[0].serial);
    assert(pvr_ccb_collect(&q, tokens[0], &status) == PVR_CCB_INVALID);

    /* Firmware events wrap independently; one receive consumes one event. */
    restore();
    unsigned char output[88]; memset(output, 0xa5, sizeof(output));
    assert(pvr_ccb_receive(&q, output) == PVR_CCB_AGAIN && output[0] == 0xa5);
    for (unsigned i = 0; i < 34; ++i) {
        event(PVR_CCB_MAGIC | 103, i);
        assert(pvr_ccb_receive(&q, output) == PVR_CCB_OK);
        assert(word(output) == (PVR_CCB_MAGIC | 103) && output[87] == i);
        assert(word(m.device[3] + 128) == ((i + 1) & 31));
    }
    unsigned receive_calls;
    restore(); event(PVR_CCB_MAGIC | 103, 0x5a);
    assert(pvr_ccb_receive(&q, output) == PVR_CCB_OK); receive_calls = m.calls;
    restore(); event(PVR_CCB_MAGIC | 109, 1);
    assert(pvr_ccb_receive(&q, output) == PVR_CCB_RESTART && word(output) == (PVR_CCB_MAGIC | 109)); stopped();
    restore(); event(PVR_CCB_MAGIC | 112, 2);
    assert(pvr_ccb_receive(&q, output) == PVR_CCB_RESTART); stopped();
    restore(); event(0, 0);
    assert(pvr_ccb_receive(&q, output) == PVR_CCB_FAULT); stopped();

    /* Pending deadline, bad return bits, and corrupt cursor/ABI fields. */
    restore(); assert(pvr_ccb_submit(&q, m.expected, 1, 5, &token) == PVR_CCB_OK);
    assert(pvr_ccb_poll(&q, 4) == PVR_CCB_OK);
    assert(pvr_ccb_poll(&q, 3) == PVR_CCB_INVALID);
    assert(pvr_ccb_poll(&q, 5) == PVR_CCB_FAULT); stopped();
    assert(pvr_ccb_collect(&q, token, &status) == PVR_CCB_FAULT);
    restore(); assert(pvr_ccb_submit(&q, m.expected, 1, 5, &token) == PVR_CCB_OK);
    complete(token, 9); assert(pvr_ccb_poll(&q, 2) == PVR_CCB_FAULT); stopped();
    restore(); assert(pvr_ccb_submit(&q, m.expected, 1, 5, &token) == PVR_CCB_OK);
    complete(token, 1); m.calls = 0;
    assert(pvr_ccb_poll(&q, 2) == PVR_CCB_OK);
    unsigned poll_calls = m.calls;
    event(PVR_CCB_MAGIC | 109, 0);
    assert(pvr_ccb_receive(&q, output) == PVR_CCB_RESTART);
    unsigned calls = m.calls;
    assert(pvr_ccb_collect(&q, token, &status) == PVR_CCB_OK && status == 1 && calls == m.calls);
    const unsigned corruption[][3] = {{0,0,1},{0,128,128},{0,128,2},{0,132,63},
        {0,136,0},{3,0,32},{3,128,1},{3,132,7},{3,136,0}};
    for (unsigned i = 0; i < sizeof(corruption) / sizeof(corruption[0]); ++i) {
        restore(); put(m.device[corruption[i][0]] + corruption[i][1], corruption[i][2]);
        assert((corruption[i][0] ? pvr_ccb_receive(&q, output) : pvr_ccb_poll(&q, 1)) == PVR_CCB_FAULT);
        stopped();
    }
    /* Every callback stage can fail, including a write that already took effect. */
    for (unsigned i = 1; i <= submit_calls; ++i) {
        restore(); m.fail_at = i; memset(&token, 0xa5, sizeof(token)); before = token;
        assert(pvr_ccb_submit(&q, m.expected, 1, 100, &token) == PVR_CCB_FAULT);
        assert(!memcmp(&token, &before, sizeof(token)) && m.calls == i); stopped();
    }
    for (unsigned i = 1; i <= receive_calls; ++i) {
        restore(); event(PVR_CCB_MAGIC | 103, 0x5a); m.fail_at = i;
        memset(output, 0xa5, sizeof(output));
        assert(pvr_ccb_receive(&q, output) == PVR_CCB_FAULT && m.calls == i);
        for (unsigned j = 0; j < sizeof(output); ++j) assert(output[j] == 0xa5);
        stopped();
    }
    for (unsigned i = 1; i <= poll_calls; ++i) {
        restore(); assert(pvr_ccb_submit(&q, m.expected, 1, 100, &token) == PVR_CCB_OK);
        complete(token, 1); m.calls = 0; m.fail_at = i;
        assert(pvr_ccb_poll(&q, 2) == PVR_CCB_FAULT && m.calls == i); stopped();
    }
    /* Failed formatting cannot be retried against the same queue. */
    memset(&q, 0, sizeof(q)); memset(&m, 0, sizeof(m)); m.fail_at = 3;
    assert(pvr_ccb_init(&q, &ops, &m, 2) == PVR_CCB_FAULT); stopped();
    puts("PASS GPU firmware queue publication, completion ownership, wrap, events, deadlines and retained failures");
    printf("Injected callback failures: %u submission stages, %u event stages, %u polling stages, one initialization stage\n",
           submit_calls, receive_calls, poll_calls);
}
