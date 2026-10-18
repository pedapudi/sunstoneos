#ifndef SUNSTONE_STORAGE_DIGEST_H
#define SUNSTONE_STORAGE_DIGEST_H
#include <stddef.h>
#include <stdint.h>
/* Software SHA-256, using the existing observer compression function. */
struct storage_digest { uint32_t h[8]; uint64_t bytes; uint8_t tail[64]; unsigned used; };
void storage_digest_init(struct storage_digest *);
int storage_digest_add(struct storage_digest *,const void *,size_t);
void storage_digest_end(const struct storage_digest *,uint8_t[32]);
void storage_digest_bytes(const void *,size_t,uint8_t[32]);
#endif
