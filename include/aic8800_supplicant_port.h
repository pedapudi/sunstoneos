#ifndef SUNSTONE_AIC8800_SUPPLICANT_PORT_H
#define SUNSTONE_AIC8800_SUPPLICANT_PORT_H
/* Force-included only for the selected external RSN/crypto translation units.
 * Replace its hosted umbrella include without changing pinned source files.
 * The OS adapter implements allocation/timers/entropy; no POSIX networking. */
#define INCLUDES_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <limits.h>
#if __STDC_HOSTED__ || defined(SUNSTONE_AIC8800_SAE)
/* SAE images compile the OpenSSL headers, which include the C library
 * headers; declare them before the free and abort macros below. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#else
typedef struct sunstone_rsn_unused_file FILE;
typedef ptrdiff_t ssize_t;
#endif
#define OS_NO_C_LIB_DEFINES
#define CONFIG_NO_STDOUT_DEBUG
#define CONFIG_NO_WPA_MSG
#define CONFIG_NO_RANDOM_POOL
#define CONFIG_NO_RC4
#define CONFIG_NO_TKIP
#ifdef SUNSTONE_AIC8800_SAE
/* SAE groups 19, 20 and 21 through crypto_openssl.c; SAE-PK stays off. */
#define CONFIG_SAE
#define CONFIG_SHA256
#define CONFIG_ECC
#define CONFIG_SHA384
#define CONFIG_SHA512
/* The minimal OpenSSL build omits MD4, DES and FILE-based PEM input, which
 * only unused crypto_openssl.c functions call. The native link discards
 * those functions; a reachable use must fail the link. */
struct evp_md_st; struct evp_cipher_st; struct evp_pkey_st; struct x509_st;
const struct evp_md_st *EVP_md4(void);
const struct evp_cipher_st *EVP_des_ecb(void);
struct evp_pkey_st *PEM_read_PUBKEY(FILE *,struct evp_pkey_st **,int (*)(char *,int,int,void *),void *);
struct evp_pkey_st *PEM_read_PrivateKey(FILE *,struct evp_pkey_st **,int (*)(char *,int,int,void *),void *);
struct x509_st *PEM_read_X509(FILE *,struct x509_st **,int (*)(char *,int,int,void *),void *);
#else
#define CONFIG_CRYPTO_INTERNAL
#endif
#ifndef __BYTE_ORDER
#define __BYTE_ORDER __BYTE_ORDER__
#define __LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#define __BIG_ENDIAN __ORDER_BIG_ENDIAN__
#endif
#ifndef bswap_16
#define bswap_16(x) __builtin_bswap16(x)
#define bswap_32(x) __builtin_bswap32(x)
#define bswap_64(x) __builtin_bswap64(x)
#endif
/* Only unreachable status-formatting functions use hosted formatting. Native
 * library links discard them; an unresolved use must fail the native link. */
int snprintf(char *,size_t,const char *,...);
int vsnprintf(char *,size_t,const char *,va_list);
void *memcpy(void *,const void *,size_t);
void *memmove(void *,const void *,size_t);
void *memset(void *,int,size_t);
int memcmp(const void *,const void *,size_t);
size_t strlen(const char *);
int strcmp(const char *,const char *);
int strncmp(const char *,const char *,size_t);
char *strchr(const char *,int);
char *strstr(const char *,const char *);
int atoi(const char *);
long strtol(const char *,char **,int);
int abs(int);
void qsort(void *,size_t,size_t,int (*)(const void *,const void *));
void os_free(void *);
#define free(p) os_free(p)
_Noreturn void aic8800_supplicant_abort(void);
#define abort() aic8800_supplicant_abort()
int isspace(int);
#endif
