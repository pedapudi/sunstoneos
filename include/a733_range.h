/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_A733_RANGE_H
#define SUNSTONE_A733_RANGE_H
/* Address-range and deadline predicates in portable C.
 *
 * A range [p, p + n) holds n bytes. The predicates differ in four rules, and
 * each name states which rule it applies:
 *   - end: `excl` requires the exclusive end p + n to be representable, so a
 *     range cannot reach the top byte; `incl` requires only the last byte
 *     p + n - 1 to be representable.
 *   - zero length: accepted, refused, or treated as holding no byte.
 *   - NULL: pointer forms refuse NULL unless the name says otherwise; extent
 *     forms take integer addresses and have no NULL rule.
 *   - wrap: `checked` forms refuse or report a range whose end does not fit;
 *     `prechecked` forms compute with modular arithmetic and are correct only
 *     after the caller has established that both ends fit.
 * docs/shared-helpers.md maps the existing local helpers onto these names. */
#include <stddef.h>
#include <stdint.h>

/* --- Single ranges -------------------------------------------------------- */

/* p is non-NULL and p + n fits. Zero length is accepted. */
static inline int a733_span_excl(const void *p, size_t n)
{ return p && n <= UINTPTR_MAX - (uintptr_t)p; }

/* p is non-NULL, n is nonzero and p + n fits. */
static inline int a733_span_excl_nonempty(const void *p, size_t n)
{ return p && n && n <= UINTPTR_MAX - (uintptr_t)p; }

/* p is non-NULL, n is nonzero and the last byte p + n - 1 fits; the range may
 * end at the top byte of the address space. */
static inline int a733_span_incl_nonempty(const void *p, size_t n)
{ return p && n && (uintptr_t)p <= UINTPTR_MAX - (n - 1); }

/* A zero-length range is accepted with any pointer, including NULL; otherwise
 * as a733_span_incl_nonempty(). For optional caller buffers. */
static inline int a733_span_incl_or_empty(const void *p, size_t n)
{ return !n || (p && (uintptr_t)p <= UINTPTR_MAX - (n - 1)); }

/* A zero-length range is accepted only with a NULL pointer; otherwise as
 * a733_span_incl_nonempty(). For parameters where an empty buffer must be
 * absent. */
static inline int a733_span_incl_or_null_empty(const void *p, size_t n)
{ return n ? p && (uintptr_t)p <= UINTPTR_MAX - (n - 1) : p == NULL; }

/* [start, start + n) lies inside [base, base + bytes), compared as integers
 * without overflow. Zero n is accepted anywhere in [base, base + bytes]. */
static inline int a733_extent_within(uint64_t base, uint64_t bytes, uint64_t start, uint64_t n)
{ return start >= base && start - base <= bytes && n <= bytes - (start - base); }

/* --- Pairs of pointer ranges ---------------------------------------------- */

/* Both pointers are non-NULL, both exclusive ends fit, and the ranges share no
 * byte. Zero length is accepted; a zero-length range strictly inside the other
 * range counts as not disjoint. */
static inline int a733_ranges_disjoint(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return a && b && an <= UINTPTR_MAX - x && bn <= UINTPTR_MAX - y && (x + an <= y || y + bn <= x);
}

/* As a733_ranges_disjoint(), and both lengths are nonzero. */
static inline int a733_ranges_disjoint_nonempty(const void *a, size_t an, const void *b, size_t bn)
{ return an && bn && a733_ranges_disjoint(a, an, b, bn); }

/* A zero-length range holds no byte and is disjoint from everything, even
 * with a NULL pointer; otherwise as a733_ranges_disjoint(). */
static inline int a733_ranges_disjoint_or_empty(const void *a, size_t an, const void *b, size_t bn)
{ return !an || !bn || a733_ranges_disjoint(a, an, b, bn); }

/* The first range is required: non-NULL with a fitting exclusive end. A NULL
 * second range is absent and disjoint; otherwise its end must fit and the
 * ranges must share no byte. */
static inline int a733_ranges_disjoint_optional_second(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    if (!a || an > UINTPTR_MAX - x) return 0;
    if (!b) return 1;
    return bn <= UINTPTR_MAX - y && !(x < y + bn && y < x + an);
}

/* Fail-closed overlap of pointer ranges: true when either exclusive end does
 * not fit or the ranges share a byte. There is no NULL rule: NULL is address
 * zero. Exactly the pointer-width form of a733_extents_overlap_or_wrap(). */
static inline int a733_ranges_overlap_or_wrap(const void *a, size_t an, const void *b, size_t bn)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return an > UINTPTR_MAX - x || bn > UINTPTR_MAX - y || (x < y + bn && y < x + an);
}

/* --- Pairs of integer extents --------------------------------------------- */
/* Extent forms compute in 64 bits. They match a pointer-width check only where
 * uintptr_t has 64 bits, so 32-bit callers (the AArch32 SRAM programs) must not
 * use them to check pointer wrap. */

/* Both exclusive ends fit and the extents share no address. Zero length is
 * accepted as in a733_ranges_disjoint(). Address zero is an ordinary address. */
static inline int a733_extents_disjoint_checked(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return an <= UINT64_MAX - a && bn <= UINT64_MAX - b && (a + an <= b || b + bn <= a); }

/* Fail-closed overlap: true when either exclusive end does not fit or the
 * extents share an address. Exactly !a733_extents_disjoint_checked(). Use it
 * where a true result rejects the request. */
static inline int a733_extents_overlap_or_wrap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return an > UINT64_MAX - a || bn > UINT64_MAX - b || (a < b + bn && b < a + an); }

/* As a733_extents_overlap_or_wrap(), except that a zero-length extent never
 * overlaps, even when its end would not fit. */
static inline int a733_extents_overlap_nonempty_or_wrap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return an && bn && a733_extents_overlap_or_wrap(a, an, b, bn); }

/* The mathematical intervals [a, a + an) and [b, b + bn) intersect, computed
 * from distances so that no sum is formed. Zero length never intersects. An
 * end beyond UINT64_MAX is not refused and does not wrap to low addresses. */
static inline int a733_extents_intersect(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return an && bn && (a <= b ? b - a < an : a - b < bn); }

/* Modular comparison with no wrap check. Correct only after the caller has
 * established that a + an and b + bn both fit; a wrapping input can report
 * disjoint ranges that overlap. Exactly !a733_extents_overlap_prechecked(). */
static inline int a733_extents_disjoint_prechecked(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return a + an <= b || b + bn <= a; }

/* Modular comparison with no wrap check; the precondition of
 * a733_extents_disjoint_prechecked() applies. */
static inline int a733_extents_overlap_prechecked(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{ return a < b + bn && b < a + an; }

/* --- Monotonic deadlines ---------------------------------------------------- */

/* Admit a clock sample: refuse a sample earlier than *last or at or after
 * deadline, and leave *last unchanged on refusal. On admission store the
 * sample in *last. The caller samples the clock and refuses a failed sample
 * itself. */
static inline int a733_deadline_admit(uint64_t now, uint64_t *last, uint64_t deadline)
{
    if (now < *last || now >= deadline) return 0;
    *last = now;
    return 1;
}

/* Advance a monotonic sample without a deadline: refuse a sample earlier than
 * *last, otherwise store it. Callers that store an expired sample before
 * comparing it with one or more deadlines use this, then compare. */
static inline int a733_monotonic_advance(uint64_t now, uint64_t *last)
{
    if (now < *last) return 0;
    *last = now;
    return 1;
}

/* *out = now + delta when the sum fits. Refuse an overflowing sum and leave
 * *out unchanged. */
static inline int a733_deadline_after(uint64_t now, uint64_t delta, uint64_t *out)
{
    if (now > UINT64_MAX - delta) return 0;
    *out = now + delta;
    return 1;
}

/* now + delta, or UINT64_MAX when the sum does not fit. For bounds that later
 * samples are compared with: a saturated bound never expires. */
static inline uint64_t a733_deadline_after_saturating(uint64_t now, uint64_t delta)
{ return delta > UINT64_MAX - now ? UINT64_MAX : now + delta; }
#endif
