/* SPDX-License-Identifier: Apache-2.0 */
/* Cases for the shared range, deadline, MMIO, mapping-check and task-context
 * helpers. The host build runs every portable helper under ASan and UBSan and
 * compares the range predicates with 128-bit reference formulas over edge
 * values. The AArch64 and AArch32 builds compile the architecture-only helpers
 * into probe functions; checks/shared-helpers.py inspects their instructions. */
#include "a733_mmio.h"
#if defined(__aarch64__)
#include "a733_task_context.h"

/* Probes keep the helpers out of line so the runner can read each sequence. */
int probe_task_owner(TaskHandle_t owner) { return a733_task_context_owner_el1(owner); }
int probe_task_any(void) { return a733_task_context_any_no_el_check(); }
#endif
#if defined(__aarch64__) || defined(__arm__)
uint32_t probe_read32_both(uintptr_t a) { return a733_mmio_read32_dsb_sy_both(a); }
uint32_t probe_read32_after(uintptr_t a) { return a733_mmio_read32_dsb_sy_after(a); }
void probe_write32_both(uintptr_t a, uint32_t v) { a733_mmio_write32_dsb_sy_both(a, v); }
void probe_write32_after(uintptr_t a, uint32_t v) { a733_mmio_write32_dsb_sy_after(a, v); }
uint8_t probe_read8_both(uintptr_t a) { return a733_mmio_read8_dsb_sy_both(a); }
uint16_t probe_read16_both(uintptr_t a) { return a733_mmio_read16_dsb_sy_both(a); }
uint64_t probe_read64_both(uintptr_t a) { return a733_mmio_read64_dsb_sy_both(a); }
uint8_t probe_read8_after(uintptr_t a) { return a733_mmio_read8_dsb_sy_after(a); }
uint16_t probe_read16_after(uintptr_t a) { return a733_mmio_read16_dsb_sy_after(a); }
uint64_t probe_read64_after(uintptr_t a) { return a733_mmio_read64_dsb_sy_after(a); }
void probe_write8_both(uintptr_t a, uint8_t v) { a733_mmio_write8_dsb_sy_both(a, v); }
void probe_write16_both(uintptr_t a, uint16_t v) { a733_mmio_write16_dsb_sy_both(a, v); }
void probe_write64_both(uintptr_t a, uint64_t v) { a733_mmio_write64_dsb_sy_both(a, v); }
void probe_write8_after(uintptr_t a, uint8_t v) { a733_mmio_write8_dsb_sy_after(a, v); }
void probe_write16_after(uintptr_t a, uint16_t v) { a733_mmio_write16_dsb_sy_after(a, v); }
void probe_write64_after(uintptr_t a, uint64_t v) { a733_mmio_write64_dsb_sy_after(a, v); }
uint32_t probe_read32_plain(uintptr_t a) { return a733_mmio_read32(a); }
void probe_write32_plain(uintptr_t a, uint32_t v) { a733_mmio_write32(a, v); }
void probe_dsb_sy(void) { a733_mmio_dsb_sy(); }
#endif

#if !defined(__aarch64__) && !defined(__arm__)
#include "a733_mapping_check.h"
#include "a733_range.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(uintptr_t) == 8 && sizeof(size_t) == 8, "the host model assumes 64-bit addresses");

static unsigned cases;
#define CHECK(condition) do { ++cases; if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); } } while (0)

typedef unsigned __int128 wide;
static const wide TOP = UINT64_MAX;
static const void *P(uint64_t v) { return (const void *)(uintptr_t)v; }

/* Edge values: zero, small, adjacent, middle and the top of the space. */
static const uint64_t values[] = {
    0, 1, 2, 3, 4, 0x1000, 0x1001, 0x7fffffffffffffffu, 0x8000000000000000u,
    UINT64_MAX - 4096, UINT64_MAX - 4, UINT64_MAX - 3, UINT64_MAX - 2, UINT64_MAX - 1, UINT64_MAX,
};
#define VALUES (sizeof(values) / sizeof(values[0]))

static int ref_end_fits(uint64_t p, uint64_t n) { return (wide)p + n <= TOP; }
static int ref_last_fits(uint64_t p, uint64_t n) { return n && (wide)p + n - 1 <= TOP; }
static int ref_share(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return (wide)a < (wide)b + bn && (wide)b < (wide)a + an; }
static int ref_apart(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return (wide)a + an <= b || (wide)b + bn <= a; }

static void spans(void)
{
    for (unsigned i = 0; i < VALUES; ++i) for (unsigned j = 0; j < VALUES; ++j) {
        uint64_t p = values[i], n = values[j];
        CHECK(a733_span_excl(P(p), n) == (p && ref_end_fits(p, n)));
        CHECK(a733_span_excl_nonempty(P(p), n) == (p && n && ref_end_fits(p, n)));
        CHECK(a733_span_incl_nonempty(P(p), n) == (p && ref_last_fits(p, n)));
        CHECK(a733_span_incl_or_empty(P(p), n) == (!n || (p && ref_last_fits(p, n))));
        CHECK(a733_span_incl_or_null_empty(P(p), n) == (n ? p && ref_last_fits(p, n) : !p));
    }
    /* Named edges: the top byte separates the exclusive and inclusive forms. */
    CHECK(a733_span_excl(P(UINT64_MAX - 1), 1) && !a733_span_excl(P(UINT64_MAX), 1));
    CHECK(a733_span_excl(P(UINT64_MAX), 0) && !a733_span_excl(NULL, 0));
    CHECK(!a733_span_excl_nonempty(P(16), 0) && a733_span_excl_nonempty(P(16), 16));
    CHECK(a733_span_incl_nonempty(P(UINT64_MAX), 1) && !a733_span_incl_nonempty(P(UINT64_MAX), 2));
    CHECK(a733_span_incl_nonempty(P(1), UINT64_MAX) && !a733_span_incl_nonempty(P(2), UINT64_MAX));
    CHECK(a733_span_incl_or_empty(NULL, 0) && !a733_span_incl_or_empty(NULL, 1));
    CHECK(a733_span_incl_or_null_empty(NULL, 0) && !a733_span_incl_or_null_empty(P(16), 0));
    CHECK(!a733_span_incl_or_null_empty(NULL, 4) && a733_span_incl_or_null_empty(P(16), 4));
}

static void within(void)
{
    for (unsigned i = 0; i < VALUES; ++i) for (unsigned j = 0; j < VALUES; ++j)
        for (unsigned k = 0; k < VALUES; ++k) for (unsigned l = 0; l < VALUES; ++l) {
            uint64_t base = values[i], bytes = values[j], start = values[k], n = values[l];
            CHECK(a733_extent_within(base, bytes, start, n) ==
                  (start >= base && (wide)start + n <= (wide)base + bytes));
        }
    CHECK(a733_extent_within(0x1000, 0x100, 0x1100, 0));
    CHECK(!a733_extent_within(0x1000, 0x100, 0x1101, 0));
    CHECK(a733_extent_within(0x1000, 0x100, 0x1000, 0x100) && !a733_extent_within(0x1000, 0x100, 0x1000, 0x101));
    CHECK(!a733_extent_within(0x1000, 0x100, 0xfff, 1));
    CHECK(a733_extent_within(UINT64_MAX - 1, 1, UINT64_MAX - 1, 1));
    CHECK(a733_extent_within(UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX));
}

static void pairs(void)
{
    for (unsigned i = 0; i < VALUES; ++i) for (unsigned j = 0; j < VALUES; ++j)
        for (unsigned k = 0; k < VALUES; ++k) for (unsigned l = 0; l < VALUES; ++l) {
            uint64_t a = values[i], an = values[j], b = values[k], bn = values[l];
            int fits = ref_end_fits(a, an) && ref_end_fits(b, bn);
            int disjoint = a && b && fits && ref_apart(a, an, b, bn);
            CHECK(a733_ranges_disjoint(P(a), an, P(b), bn) == disjoint);
            CHECK(a733_ranges_disjoint_nonempty(P(a), an, P(b), bn) == (an && bn && disjoint));
            CHECK(a733_ranges_disjoint_or_empty(P(a), an, P(b), bn) == (!an || !bn || disjoint));
            CHECK(a733_ranges_disjoint_optional_second(P(a), an, P(b), bn) ==
                  (a && ref_end_fits(a, an) && (!b || (ref_end_fits(b, bn) && !ref_share(a, an, b, bn)))));
            int checked = fits && ref_apart(a, an, b, bn);
            CHECK(a733_extents_disjoint_checked(a, an, b, bn) == checked);
            CHECK(a733_extents_overlap_or_wrap(a, an, b, bn) == !checked);
            CHECK(a733_ranges_overlap_or_wrap(P(a), an, P(b), bn) == !checked);
            CHECK(a733_extents_overlap_nonempty_or_wrap(a, an, b, bn) == (an && bn && !checked));
            CHECK(a733_extents_intersect(a, an, b, bn) == (an && bn && ref_share(a, an, b, bn)));
            CHECK(a733_extents_disjoint_prechecked(a, an, b, bn) == !a733_extents_overlap_prechecked(a, an, b, bn));
            if (fits) {
                CHECK(a733_extents_disjoint_prechecked(a, an, b, bn) == checked);
                CHECK(a733_extents_overlap_prechecked(a, an, b, bn) == !checked);
            }
        }
    /* Adjacent ranges are disjoint; one shared byte is not. */
    CHECK(a733_ranges_disjoint(P(0x1000), 0x10, P(0x1010), 0x10));
    CHECK(!a733_ranges_disjoint(P(0x1000), 0x11, P(0x1010), 0x10));
    /* NULL refuses even when the other range is valid. */
    CHECK(!a733_ranges_disjoint(NULL, 1, P(0x1000), 1) && !a733_ranges_disjoint(P(0x1000), 1, NULL, 1));
    /* A zero-length range strictly inside the other is not disjoint here,
     * but holds no byte for the _or_empty form. */
    CHECK(!a733_ranges_disjoint(P(0x1008), 0, P(0x1000), 0x10));
    CHECK(a733_ranges_disjoint(P(0x1000), 0, P(0x1000), 0x10));
    CHECK(a733_ranges_disjoint_or_empty(P(0x1008), 0, P(0x1000), 0x10));
    CHECK(a733_ranges_disjoint_or_empty(NULL, 0, NULL, 4));
    CHECK(!a733_ranges_disjoint_nonempty(P(0x1000), 0, P(0x2000), 1));
    /* A range that reaches past the top refuses (disjoint) or reports overlap. */
    CHECK(!a733_ranges_disjoint(P(UINT64_MAX - 1), 2, P(0x1000), 1));
    CHECK(a733_ranges_disjoint(P(UINT64_MAX - 1), 1, P(0x1000), 1));
    CHECK(a733_extents_overlap_or_wrap(UINT64_MAX, 1, 0, 1));
    CHECK(a733_ranges_overlap_or_wrap(NULL, 1, P(1), 1) == 0 && a733_ranges_overlap_or_wrap(NULL, 2, P(1), 1));
    CHECK(!a733_extents_overlap_nonempty_or_wrap(UINT64_MAX, 0, 0, UINT64_MAX));
    CHECK(a733_extents_overlap_nonempty_or_wrap(UINT64_MAX, 2, 0, 1));
    CHECK(a733_extents_disjoint_checked(0, 1, 1, 1));
    /* The optional second range: absent is disjoint, a present one is checked. */
    CHECK(a733_ranges_disjoint_optional_second(P(0x1000), 16, NULL, 99));
    CHECK(!a733_ranges_disjoint_optional_second(NULL, 16, NULL, 0));
    CHECK(!a733_ranges_disjoint_optional_second(P(0x1000), 16, P(0x1008), 1));
    CHECK(!a733_ranges_disjoint_optional_second(P(0x1000), 16, P(UINT64_MAX), 2));
    /* Distance form: ends beyond the top are compared as integers. */
    CHECK(a733_extents_intersect(UINT64_MAX - 1, 4, UINT64_MAX, 1));
    CHECK(!a733_extents_intersect(UINT64_MAX - 1, 4, 0, 1));
    CHECK(!a733_extents_intersect(5, 0, 0, 10));
    /* The prechecked forms do not detect wrap: a wrapping extent that covers
     * address 0 is reported disjoint from [0, 1). */
    CHECK(a733_extents_disjoint_prechecked(UINT64_MAX, 2, 0, 1));
    CHECK(!a733_extents_disjoint_checked(UINT64_MAX, 2, 0, 1));
}

static void deadlines(void)
{
    uint64_t last = 10, out = 7;
    CHECK(a733_deadline_admit(10, &last, 11) && last == 10);
    CHECK(a733_deadline_admit(12, &last, 20) && last == 12);
    CHECK(!a733_deadline_admit(11, &last, 20) && last == 12);
    CHECK(!a733_deadline_admit(20, &last, 20) && last == 12);
    CHECK(!a733_deadline_admit(UINT64_MAX, &last, UINT64_MAX) && last == 12);
    CHECK(a733_deadline_admit(UINT64_MAX - 1, &last, UINT64_MAX) && last == UINT64_MAX - 1);
    last = 0;
    CHECK(!a733_deadline_admit(0, &last, 0) && last == 0);
    last = 5;
    CHECK(a733_monotonic_advance(5, &last) && last == 5);
    CHECK(a733_monotonic_advance(UINT64_MAX, &last) && last == UINT64_MAX);
    CHECK(!a733_monotonic_advance(UINT64_MAX - 1, &last) && last == UINT64_MAX);
    last = 3;
    CHECK(!a733_monotonic_advance(0, &last) && last == 3);
    CHECK(a733_deadline_after(1, 2, &out) && out == 3);
    CHECK(a733_deadline_after(UINT64_MAX - 5, 5, &out) && out == UINT64_MAX);
    out = 7;
    CHECK(!a733_deadline_after(UINT64_MAX - 5, 6, &out) && out == 7);
    CHECK(!a733_deadline_after(UINT64_MAX, 1, &out) && out == 7);
    CHECK(a733_deadline_after(UINT64_MAX, 0, &out) && out == UINT64_MAX);
    CHECK(a733_deadline_after(0, UINT64_MAX, &out) && out == UINT64_MAX);
    CHECK(!a733_deadline_after(1, UINT64_MAX, &out));
}

static void mmio(void)
{
    _Alignas(8) unsigned char bytes[16];
    memset(bytes, 0, sizeof(bytes));
    uintptr_t base = (uintptr_t)bytes;
    a733_mmio_write8(base + 1, 0xa5);
    CHECK(a733_mmio_read8(base + 1) == 0xa5 && bytes[0] == 0 && bytes[2] == 0);
    a733_mmio_write16(base + 2, 0x1234);
    CHECK(a733_mmio_read16(base + 2) == 0x1234 && bytes[4] == 0);
    a733_mmio_write32(base + 4, 0xdeadbeefu);
    CHECK(a733_mmio_read32(base + 4) == 0xdeadbeefu);
    a733_mmio_write64(base + 8, UINT64_C(0x0123456789abcdef));
    CHECK(a733_mmio_read64(base + 8) == UINT64_C(0x0123456789abcdef));
    CHECK(a733_mmio_read8(base + 1) == 0xa5 && a733_mmio_read32(base + 4) == 0xdeadbeefu);
}

/* Translation model: identity except where a case overrides one page. */
struct model_page { uint64_t page; int write, fail; uint64_t delta; unsigned attributes, shareability; };
static struct model_page overrides[4];
static unsigned override_count, default_attributes, default_shareability, calls;
static uintptr_t called[64];
static int called_write[64];
static void model_reset(unsigned attributes, unsigned shareability)
{
    override_count = calls = 0;
    default_attributes = attributes;
    default_shareability = shareability;
}
static void model_override(uint64_t page, int write, int fail, uint64_t delta, unsigned attributes, unsigned shareability)
{ overrides[override_count++] = (struct model_page){page, write, fail, delta, attributes, shareability}; }
static int model_translate(uintptr_t address, int write, struct aarch64_translation *t)
{
    if (calls < 64) { called[calls] = address; called_write[calls] = write; }
    ++calls;
    *t = (struct aarch64_translation){address, default_attributes, default_shareability};
    for (unsigned i = 0; i < override_count; ++i)
        if (overrides[i].page == (address >> 12) && overrides[i].write == write) {
            if (overrides[i].fail) return 0;
            *t = (struct aarch64_translation){address + overrides[i].delta, overrides[i].attributes,
                                              overrides[i].shareability};
        }
    return 1;
}
/* Physical address offset used by the non-identity cases. */
static int model_offset(uintptr_t address, int write, struct aarch64_translation *t)
{
    int ok = model_translate(address, write, t);
    t->physical += 0x100000;
    return ok;
}

static void mappings(void)
{
    const struct a733_mapping_rule wb_write = {A733_MAPPING_WRITE, 1, {{A733_MAIR_NORMAL_WB, A733_SH_INNER}}, 0, 1};
    const struct a733_mapping_rule nc_both = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_NORMAL_NC, A733_SH_OUTER | A733_SH_INNER}}, 1, 1};
    const struct a733_mapping_rule device_rw = {A733_MAPPING_READ_WRITE, 1, {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 0, 1};
    const struct a733_mapping_rule device_any = {A733_MAPPING_READ, 2,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}, {A733_MAIR_DEVICE_NGNRE, A733_SH_ANY}}, 0, 1};
    const struct a733_mapping_rule two_rw = {A733_MAPPING_READ_WRITE, 2,
        {{A733_MAIR_NORMAL_WB, A733_SH_ANY}, {A733_MAIR_NORMAL_NC, A733_SH_ANY}}, 0, 1};
    const struct a733_mapping_rule unplaced = {A733_MAPPING_WRITE, 2,
        {{A733_MAIR_NORMAL_WB, A733_SH_ANY}, {A733_MAIR_NORMAL_NC, A733_SH_ANY}}, 0, 0};
    struct aarch64_translation t = {0x1000, A733_MAIR_NORMAL_WB, 3};

    /* Single translation results. */
    CHECK(a733_mapping_translation_ok(&t, 0x1000, &wb_write));
    CHECK(!a733_mapping_translation_ok(&t, 0x2000, &wb_write));
    t.shareability = 2;
    CHECK(!a733_mapping_translation_ok(&t, 0x1000, &wb_write));
    t.shareability = 7;
    CHECK(!a733_mapping_translation_ok(&t, 0x1000, &device_any));
    t = (struct aarch64_translation){0x1000, A733_MAIR_DEVICE_NGNRE, 2};
    CHECK(a733_mapping_translation_ok(&t, 0x1000, &device_any));
    t.attributes = A733_MAIR_NORMAL_NC;
    CHECK(!a733_mapping_translation_ok(&t, 0x1000, &device_any));
    CHECK(a733_mapping_translation_ok(&t, 0x9999, &unplaced));

    /* One address: access selection, order and failures. */
    model_reset(A733_MAIR_NORMAL_WB, 3);
    CHECK(a733_mapping_check(model_translate, 0x40000000, 0x40000000, &wb_write));
    CHECK(calls == 1 && called_write[0] == 1);
    model_reset(A733_MAIR_NORMAL_NC, 2);
    CHECK(a733_mapping_check(model_translate, 0x41c00000, 0x41c00000, &nc_both));
    CHECK(calls == 2 && called_write[0] == 0 && called_write[1] == 1);
    model_override(0x41c00, 1, 0, 0, A733_MAIR_NORMAL_NC, 3);
    CHECK(!a733_mapping_check(model_translate, 0x41c00000, 0x41c00000, &nc_both));
    CHECK(a733_mapping_check(model_translate, 0x41c00000, 0x41c00000, &device_rw) == 0);
    model_reset(A733_MAIR_NORMAL_NC, 0);
    CHECK(!a733_mapping_check(model_translate, 0x41c00000, 0x41c00000, &nc_both));
    model_reset(A733_MAIR_DEVICE_NGNRNE, 0);
    CHECK(a733_mapping_check(model_translate, 0x02000000, 0x02000000, &device_rw));
    model_override(0x02000, 0, 1, 0, 0, 0);
    calls = 0;
    CHECK(!a733_mapping_check(model_translate, 0x02000000, 0x02000000, &device_rw) && calls == 1);
    model_reset(A733_MAIR_DEVICE_NGNRNE, 0);
    model_override(0x02000, 1, 0, 0x1000, 0, 0);
    CHECK(!a733_mapping_check(model_translate, 0x02000000, 0x02000000, &device_rw));
    /* Read and write must carry the same attribute even when both are accepted. */
    model_reset(A733_MAIR_NORMAL_WB, 3);
    model_override(0x50000, 1, 0, 0, A733_MAIR_NORMAL_NC, 3);
    CHECK(!a733_mapping_check(model_translate, 0x50000000, 0x50000000, &two_rw));
    CHECK(a733_mapping_check(model_translate, 0x50001000, 0x50001000, &two_rw));
    CHECK(a733_mapping_check(model_offset, 0x50000000, 0x12345678, &unplaced));
    struct a733_mapping_rule bad = wb_write;
    bad.access = 0;
    CHECK(!a733_mapping_check(model_translate, 0x1000, 0x1000, &bad));
    bad.access = 4;
    CHECK(!a733_mapping_check(model_translate, 0x1000, 0x1000, &bad));
    CHECK(!a733_mapping_check(NULL, 0x1000, 0x1000, &wb_write));

    /* Stride walk from cpu itself. */
    model_reset(A733_MAIR_NORMAL_WB, 3);
    CHECK(a733_mapping_check_stride(model_translate, 0x41000000, 0x41000000, 0x3000, 4096, &wb_write));
    CHECK(calls == 3 && called[0] == 0x41000000 && called[2] == 0x41002000);
    model_reset(A733_MAIR_NORMAL_WB, 3);
    CHECK(a733_mapping_check_stride(model_translate, 0x41000800, 0x41000800, 0x1000, 4096, &wb_write));
    CHECK(calls == 1 && called[0] == 0x41000800); /* the second page is not visited */
    model_reset(A733_MAIR_NORMAL_WB, 3);
    model_override(0x41001, 1, 1, 0, 0, 0);
    CHECK(a733_mapping_check_stride(model_translate, 0x41000800, 0x41000800, 0x1000, 4096, &wb_write));
    CHECK(!a733_mapping_check_pages(model_translate, 0x41000800, 0x41000800, 0x1000, &wb_write));
    model_reset(A733_MAIR_NORMAL_WB, 3);
    CHECK(a733_mapping_check_stride(model_offset, 0x42000000, 0x42100000, 256, 64, &wb_write) && calls == 4);
    CHECK(!a733_mapping_check_stride(model_offset, 0x42000000, 0x42000000, 256, 64, &wb_write));
    CHECK(!a733_mapping_check_stride(model_translate, 0x41000000, 0x41000000, 0, 4096, &wb_write));
    CHECK(!a733_mapping_check_stride(model_translate, 0x41000000, 0x41000000, 4096, 0, &wb_write));
    CHECK(!a733_mapping_check_stride(model_translate, UINTPTR_MAX - 10, UINTPTR_MAX - 10, 20, 4096, &wb_write));
    CHECK(!a733_mapping_check_stride(model_translate, UINTPTR_MAX - 4095, UINTPTR_MAX - 4095, 4096, 4096, &wb_write));
    model_reset(A733_MAIR_NORMAL_WB, 3);
    CHECK(a733_mapping_check_stride(model_translate, UINTPTR_MAX - 4096, UINTPTR_MAX - 4096, 4096, 4096, &wb_write));
    CHECK(a733_mapping_check_stride(model_translate, 0x1000, 0x1000, SIZE_MAX - 0x1000, SIZE_MAX, &wb_write));

    /* Page walk: cpu, then following page bases through the last byte. */
    model_reset(A733_MAIR_NORMAL_WB, 3);
    CHECK(a733_mapping_check_pages(model_translate, 0x41000800, 0x41000800, 0x1801, &wb_write));
    CHECK(calls == 3 && called[0] == 0x41000800 && called[1] == 0x41001000 && called[2] == 0x41002000);
    model_reset(A733_MAIR_NORMAL_WB, 3);
    CHECK(a733_mapping_check_pages(model_translate, 0x41000fff, 0x41000fff, 1, &wb_write) && calls == 1);
    model_reset(A733_MAIR_NORMAL_WB, 3);
    CHECK(a733_mapping_check_pages(model_translate, UINTPTR_MAX - 4095, UINTPTR_MAX - 4095, 4096, &wb_write));
    CHECK(calls == 1);
    model_reset(A733_MAIR_NORMAL_WB, 3);
    CHECK(a733_mapping_check_pages(model_translate, UINTPTR_MAX - 4096, UINTPTR_MAX - 4096, 4097, &wb_write));
    CHECK(calls == 2 && called[1] == UINTPTR_MAX - 4095);
    CHECK(!a733_mapping_check_pages(model_translate, UINTPTR_MAX - 4095, UINTPTR_MAX - 4095, 4097, &wb_write));
    CHECK(!a733_mapping_check_pages(model_translate, 0x41000000, 0x41000000, 0, &wb_write));
    model_reset(A733_MAIR_NORMAL_WB, 3);
    CHECK(a733_mapping_check_pages(model_offset, 0x42000010, 0x42100010, 0x2000, &wb_write) && calls == 3);
    model_reset(A733_MAIR_NORMAL_WB, 3);
    model_override(0x42001, 1, 0, 0, A733_MAIR_NORMAL_WB, 2);
    CHECK(!a733_mapping_check_pages(model_translate, 0x42000010, 0x42000010, 0x2000, &wb_write) && calls == 2);
}

int main(void)
{
    spans();
    within();
    pairs();
    deadlines();
    mmio();
    mappings();
    printf("PASS shared helpers: %u checks\n", cases);
    return 0;
}
#endif
