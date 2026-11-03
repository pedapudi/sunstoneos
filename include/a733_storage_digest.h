#ifndef SUNSTONE_STORAGE_DIGEST_H
#define SUNSTONE_STORAGE_DIGEST_H
#include <stddef.h>
#include <stdint.h>
/* Software SHA-256, using the existing observer compression function. */
struct storage_digest { uint32_t h[8]; uint64_t bytes; uint8_t tail[64]; unsigned used; };
void storage_digest_init(struct storage_digest *);
int storage_digest_add(struct storage_digest *,const void *,size_t);
void storage_digest_end(const struct storage_digest *,uint8_t[32]);
/* Use the ARMv8 SHA-256 instructions for whole blocks in storage_digest_add_extensions, in an image that links
 * storage_digest_extensions.c and storage_digest_ce.S; returns whether they are in use. The caller checks
 * ID_AA64ISAR0_EL1.SHA2 on the core that hashes. Off by default. Never call either from EL3: storage_digest_add and
 * storage_digest_bytes are the EL3-safe path and contain no SIMD. */
int storage_digest_use_extensions(int on);
int storage_digest_add_extensions(struct storage_digest *,const void *,size_t);
void storage_digest_bytes(const void *,size_t,uint8_t[32]);
#endif
