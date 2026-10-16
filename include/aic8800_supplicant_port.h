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
#if __STDC_HOSTED__
#include <stdio.h>
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
#define CONFIG_CRYPTO_INTERNAL
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
