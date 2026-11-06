/* The container checksum of a graph on an AArch64 core: nbg_checksum_verify run by the checks/nbg-checksum-device.py harness in
 * Unicorn, with the CRC32 instructions of the hardware path (drivers/npu/nbg_checksum.c). Freestanding; no library. */
#include "nbg_checksum.h"
void *memcpy(void *d, const void *s, unsigned long n) { unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, unsigned long n) { unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d; }
struct request { const void *data; unsigned long bytes, result, stored, computed; };
void check_entry(struct request *r)
{
    struct nbg_checksum sum = {0, 0};
    r->result = (unsigned long)nbg_checksum_verify(r->data, r->bytes, &sum);
    r->stored = sum.stored;
    r->computed = sum.computed;
}
