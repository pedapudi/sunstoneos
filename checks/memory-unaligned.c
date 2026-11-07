/* Entry for checks/memory-unaligned.py: one call of a memory routine on an AArch64 core model. The routines are the ones under
 * test, linked as they are in an image: the relaxed ones for ops 0 to 2, the strict runtime ones for ops 3 to 5. */
#include <stddef.h>
void *ss_memcpy_unaligned(void *, const void *, size_t);
void *ss_memmove_unaligned(void *, const void *, size_t);
void *ss_memset_unaligned(void *, int, size_t);
void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);
struct request { unsigned long op; void *d; const void *s; unsigned long n; };
void check_entry(struct request *r)
{
    switch (r->op) {
    case 0: ss_memcpy_unaligned(r->d, r->s, r->n); break;
    case 1: ss_memmove_unaligned(r->d, r->s, r->n); break;
    case 2: ss_memset_unaligned(r->d, (int)(unsigned long)r->s, r->n); break;
    case 3: memcpy(r->d, r->s, r->n); break;
    case 4: memmove(r->d, r->s, r->n); break;
    default: memset(r->d, (int)(unsigned long)r->s, r->n); break;
    }
}
