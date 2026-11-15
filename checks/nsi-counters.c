/* Host check of the NSI counter owner (drivers/dram/a733_nsi_counters.c) against a register model: reads before writes,
 * refusal of an enabled bank, the vendor write order with readbacks, live and held counts, restore, and the failure path
 * at every write and readback. */
#include "a733_nsi_counters.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

struct access { char kind; uintptr_t address; uint32_t value; };
struct model {
    uint32_t word[2][9];
    struct access log[512]; unsigned count;
    uint64_t now;
    unsigned fail_write_at, fail_read_at, writes, reads;   /* 1-based index of the access to fail; 0 none */
    int corrupt_readback;                                  /* the failing read returns a wrong value instead of failing */
    int stuck_clear;                                       /* CLR keeps the bit that was written */
};
static int locate(uintptr_t a, unsigned *b, unsigned *w)
{
    for (unsigned i = 0; i < 2; ++i) {
        const uintptr_t first = a733_nsi_counter_address(i, 0);
        if (a >= first && a < first + 36 && !((a - first) & 3)) { *b = i; *w = (unsigned)((a - first) / 4); return 1; }
    }
    return 0;
}
static int mread(void *ctx, uintptr_t a, uint32_t *v)
{
    struct model *m = ctx; unsigned b, w;
    CHECK(locate(a, &b, &w));
    ++m->reads;
    if (m->fail_read_at && m->reads == m->fail_read_at) {
        if (!m->corrupt_readback) return 0;
        *v = m->word[b][w] ^ 0x5a5a5a5au;
    } else *v = m->word[b][w];
    CHECK(m->count < 512); m->log[m->count++] = (struct access){'r', a, *v};
    return 1;
}
static int mwrite(void *ctx, uintptr_t a, uint32_t v)
{
    struct model *m = ctx; unsigned b, w;
    CHECK(locate(a, &b, &w));
    CHECK(w <= A733_NSI_W_CYCLE);   /* the owner writes only ENABLE, CLR and CYCLE */
    ++m->writes;
    CHECK(m->count < 512); m->log[m->count++] = (struct access){'w', a, v};
    if (m->fail_write_at && m->writes == m->fail_write_at) return 0;
    if (w == A733_NSI_W_CLR) {
        if (v & 1u) for (unsigned i = 3; i < 9; ++i) m->word[b][i] = 0;
        m->word[b][w] = m->stuck_clear ? v : (v & ~1u);
    } else m->word[b][w] = v;
    return 1;
}
static uint64_t mticks(void *ctx) { struct model *m = ctx; return m->now += 8; }
static const struct a733_nsi_counter_ops ops = {mread, mwrite, mticks};

static void reset_model(struct model *m)
{
    memset(m, 0, sizeof(*m));
    for (unsigned b = 0; b < 2; ++b) {
        m->word[b][A733_NSI_W_ENABLE] = 0x00000100u | b << 4;   /* other bits present; bit 0 clear */
        m->word[b][A733_NSI_W_CLR] = 0;
        m->word[b][A733_NSI_W_CYCLE] = 0x1234u + b;
        for (unsigned i = 3; i < 9; ++i) m->word[b][i] = 100u * b + i;
    }
}
static void count(struct model *m, unsigned bank, uint32_t add)
{ if (m->word[bank][A733_NSI_W_ENABLE] & 1u) for (unsigned i = 3; i < 9; ++i) m->word[bank][i] += add; }
static void restored(const struct model *m, const struct model *initial)
{
    for (unsigned b = 0; b < 2; ++b)
        for (unsigned w = 0; w < 3; ++w) CHECK(m->word[b][w] == initial->word[b][w]);
}

static void test_addresses(void)
{
    CHECK(a733_nsi_counter_address(0, 0) == 0x02020cc0u);
    CHECK(a733_nsi_counter_address(1, 0) == 0x020224c0u);
    CHECK(a733_nsi_counter_address(0, 8) == 0x02020ce0u);
    CHECK(a733_nsi_counter_writable(0x02020cc8u) && !a733_nsi_counter_writable(0x02020cccu));
    CHECK(!a733_nsi_counter_writable(0x02020c10u) && !a733_nsi_counter_readable(0x02020c14u));   /* mode, priority */
    CHECK(!a733_nsi_counter_readable(0x0a030020u));                                              /* no AXI2HIF */
    CHECK(!a733_nsi_counter_readable(0x02020cc2u));
}
/* Open reads all eighteen words and writes nothing. */
static void test_open_reads_first(void)
{
    struct model m; reset_model(&m);
    struct a733_nsi_counters c; memset(&c, 0, sizeof(c));
    uint32_t reset[2][9];
    CHECK(a733_nsi_counters_open(&c, &ops, &m, reset) == A733_NSI_C_OK);
    CHECK(m.count == 18 && m.writes == 0);
    for (unsigned i = 0; i < 18; ++i) CHECK(m.log[i].kind == 'r');
    CHECK(!memcmp(reset, m.word, sizeof(reset)));
    CHECK(a733_nsi_counters_open(&c, &ops, &m, reset) == A733_NSI_C_REFUSED);   /* one owner */
}
/* An enabled bank belongs to someone else: no write to either bank. */
static void test_busy_refusal(void)
{
    for (unsigned busy = 0; busy < 2; ++busy) {
        struct model m; reset_model(&m); m.word[busy][A733_NSI_W_ENABLE] |= 1u;
        struct a733_nsi_counters c; memset(&c, 0, sizeof(c));
        uint32_t reset[2][9];
        CHECK(a733_nsi_counters_open(&c, &ops, &m, reset) == A733_NSI_C_BUSY);
        CHECK(m.writes == 0 && reset[busy][A733_NSI_W_ENABLE] & 1u);
        uint32_t clear;
        CHECK(a733_nsi_counters_begin(&c, 0, &clear) == A733_NSI_C_REFUSED);
        CHECK(a733_nsi_counters_begin(&c, 1, &clear) == A733_NSI_C_REFUSED);
        CHECK(m.writes == 0);
    }
    /* A failed read during open writes nothing and refuses later calls. */
    struct model m; reset_model(&m); m.fail_read_at = 7;
    struct a733_nsi_counters c; memset(&c, 0, sizeof(c));
    CHECK(a733_nsi_counters_open(&c, &ops, &m, NULL) == A733_NSI_C_IO);
    uint32_t clear;
    CHECK(a733_nsi_counters_begin(&c, 0, &clear) == A733_NSI_C_REFUSED && m.writes == 0);
}
/* The vendor order, each write read back; live counting; the hold after disable; restore. */
static void test_phase(void)
{
    struct model m; reset_model(&m);
    const struct model initial = m;
    struct a733_nsi_counters c; memset(&c, 0, sizeof(c));
    CHECK(a733_nsi_counters_open(&c, &ops, &m, NULL) == A733_NSI_C_OK);
    const unsigned start = m.count;
    uint32_t clear = 0xffffffffu;
    CHECK(a733_nsi_counters_begin(&c, 1, &clear) == A733_NSI_C_OK);
    const uintptr_t e = a733_nsi_counter_address(1, 0), cl = e + 4, cy = e + 8;
    const struct access want[] = {
        {'w', cy, 0xffffffffu}, {'r', cy, 0xffffffffu},
        {'w', e, initial.word[1][0] & ~1u}, {'r', e, initial.word[1][0] & ~1u},
        {'r', cl, 0}, {'w', cl, 1}, {'r', cl, 0},
        {'w', e, initial.word[1][0] | 1u}, {'r', e, initial.word[1][0] | 1u},
    };
    CHECK(m.count - start == sizeof(want) / sizeof(*want));
    for (unsigned i = 0; i < sizeof(want) / sizeof(*want); ++i) {
        const struct access *a = &m.log[start + i];
        CHECK(a->kind == want[i].kind && a->address == want[i].address && a->value == want[i].value);
    }
    CHECK(clear == 0);
    CHECK(m.word[1][3] == 0 && m.word[0][3] == 3);   /* bank 1 cleared; bank 0 untouched */
    count(&m, 1, 500);
    uint32_t live[6], value[6], held[6];
    CHECK(a733_nsi_counters_sample(&c, 1, live) == A733_NSI_C_OK && live[0] == 500);
    CHECK(a733_nsi_counters_sample(&c, 0, live) == A733_NSI_C_REFUSED);   /* bank 0 not programmed */
    count(&m, 1, 500);
    CHECK(a733_nsi_counters_end(&c, 1, value, held) == A733_NSI_C_OK);
    CHECK(value[0] == 1000 && held[0] == 1000 && value[2] == 1000);
    restored(&m, &initial);
    CHECK(a733_nsi_counters_begin(&c, 1, &clear) == A733_NSI_C_OK);   /* a later phase programs again */
    CHECK(a733_nsi_counters_close(&c) == A733_NSI_C_OK);               /* close restores the programmed bank */
    restored(&m, &initial);
    CHECK(a733_nsi_counters_begin(&c, 0, &clear) == A733_NSI_C_REFUSED);
}
/* A CLR that keeps its bit is recorded and still restored to the reset value. */
static void test_stuck_clear(void)
{
    struct model m; reset_model(&m); m.stuck_clear = 1;
    const struct model initial = m;
    struct a733_nsi_counters c; memset(&c, 0, sizeof(c));
    CHECK(a733_nsi_counters_open(&c, &ops, &m, NULL) == A733_NSI_C_OK);
    uint32_t clear = 0, value[6], held[6];
    CHECK(a733_nsi_counters_begin(&c, 0, &clear) == A733_NSI_C_OK && clear == 1);
    CHECK(a733_nsi_counters_end(&c, 0, value, held) == A733_NSI_C_OK);
    restored(&m, &initial);
}
/* Every write and every readback after open fails in turn: the owner reports IO, restores each programmed bank to the
 * reset values (when the model lets it), and refuses further writes. */
static void test_failure_path(void)
{
    unsigned failures = 0;
    for (int corrupt = 0; corrupt < 2; ++corrupt)
        for (unsigned at = 1; at <= 40; ++at) {
            for (int on_write = 0; on_write < 2; ++on_write) {
                if (on_write && corrupt) continue;
                struct model m; reset_model(&m);
                const struct model initial = m;
                struct a733_nsi_counters c; memset(&c, 0, sizeof(c));
                CHECK(a733_nsi_counters_open(&c, &ops, &m, NULL) == A733_NSI_C_OK);
                m.corrupt_readback = corrupt;
                if (on_write) m.fail_write_at = at; else m.fail_read_at = m.reads + at;
                uint32_t clear, value[6], held[6];
                int r = a733_nsi_counters_begin(&c, 0, &clear);
                if (r == A733_NSI_C_OK) r = a733_nsi_counters_begin(&c, 1, &clear);
                if (r == A733_NSI_C_OK) { count(&m, 0, 7); r = a733_nsi_counters_sample(&c, 0, value); }
                if (r == A733_NSI_C_OK) r = a733_nsi_counters_end(&c, 0, value, held);
                if (r == A733_NSI_C_OK) r = a733_nsi_counters_end(&c, 1, value, held);
                if (r == A733_NSI_C_OK) { CHECK(a733_nsi_counters_close(&c) == A733_NSI_C_OK); restored(&m, &initial); continue; }
                CHECK(r == A733_NSI_C_IO && c.failed && c.fail_address);
                ++failures;
                /* The only failure is the injected access, so every restore write after it succeeds. */
                restored(&m, &initial);
                const unsigned writes = m.writes;
                CHECK(a733_nsi_counters_begin(&c, 0, &clear) == A733_NSI_C_REFUSED);
                CHECK(a733_nsi_counters_sample(&c, 0, value) == A733_NSI_C_REFUSED);
                CHECK(a733_nsi_counters_close(&c) == A733_NSI_C_IO);
                CHECK(m.writes == writes);   /* nothing left to restore */
            }
        }
    CHECK(failures >= 60);   /* the injected accesses reach every step of both phases */
}
/* A failing restore write is reported and the owner stays failed. */
static void test_restore_failure(void)
{
    struct model m; reset_model(&m);
    struct a733_nsi_counters c; memset(&c, 0, sizeof(c));
    CHECK(a733_nsi_counters_open(&c, &ops, &m, NULL) == A733_NSI_C_OK);
    uint32_t clear;
    CHECK(a733_nsi_counters_begin(&c, 0, &clear) == A733_NSI_C_OK);
    m.fail_write_at = m.writes + 2;   /* the restore's CYCLE write */
    CHECK(a733_nsi_counters_close(&c) == A733_NSI_C_IO && c.failed);
    CHECK(m.word[0][A733_NSI_W_ENABLE] == 0x100u);   /* the other restore writes still ran */
}
/* The call record carries status and the first failing address. */
static void test_call(void)
{
    struct model m; reset_model(&m);
    struct a733_nsi_counters c; memset(&c, 0, sizeof(c));
    struct a733_nsi_counter_call call = {.op = A733_NSI_OPEN};
    CHECK(a733_nsi_counters_call(&c, &ops, &m, &call) == A733_NSI_C_OK && call.status == A733_NSI_C_OK);
    CHECK(call.reset[1][A733_NSI_W_CYCLE] == 0x1235u);
    call = (struct a733_nsi_counter_call){.op = A733_NSI_BEGIN, .bank = 2};
    CHECK(a733_nsi_counters_call(&c, &ops, &m, &call) == A733_NSI_C_REFUSED);
    call = (struct a733_nsi_counter_call){.op = 99};
    CHECK(a733_nsi_counters_call(&c, &ops, &m, &call) == A733_NSI_C_REFUSED);
    m.fail_write_at = m.writes + 1;
    call = (struct a733_nsi_counter_call){.op = A733_NSI_BEGIN, .bank = 0};
    CHECK(a733_nsi_counters_call(&c, &ops, &m, &call) == A733_NSI_C_IO && call.fail_address == 0x02020cc8u);
}
int main(void)
{
    test_addresses();
    test_open_reads_first();
    test_busy_refusal();
    test_phase();
    test_stuck_clear();
    test_failure_path();
    test_restore_failure();
    test_call();
    puts("PASS nsi-counters: reads before writes, busy refusal, vendor order with readbacks, live and held counts, "
         "restore, and the failure path at every write and readback");
    return 0;
}
