#ifndef SUNSTONE_A733_CE_DIGEST_METHODS_H
#define SUNSTONE_A733_CE_DIGEST_METHODS_H
#include "a733_ce.h"
/* Additional A733 HASH SEL values. Include alongside a733_ce_hash.h when
 * requesting MD5 or SM3. MD5 is retained for hardware coverage and legacy
 * interoperability; it is unsuitable for collision-resistant signatures.
 * The older a733_ce_hash_* helpers describe SHA methods only. */
#define A733_CE_MD5 ((enum a733_ce_hash_method)0)
#define A733_CE_SM3 ((enum a733_ce_hash_method)6)
static inline unsigned a733_ce_digest_block(unsigned method)
{return method==A733_CE_MD5 || method==A733_CE_SM3 ? 64:a733_ce_hash_block(method);}
static inline unsigned a733_ce_digest_state(unsigned method)
{return method==A733_CE_MD5 ? 16:(method==A733_CE_SM3 ? 32:a733_ce_hash_state(method));}
static inline unsigned a733_ce_digest_size(unsigned method)
{return method==A733_CE_MD5 ? 16:(method==A733_CE_SM3 ? 32:a733_ce_hash_digest(method));}
#endif
