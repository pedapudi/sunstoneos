#ifndef SUNSTONE_PLATFORM_H
#define SUNSTONE_PLATFORM_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define _Noreturn [[noreturn]]
#endif
void platform_init(void);
void platform_puts(const char *text);
void platform_hex(uint64_t value);
_Noreturn void platform_exit(unsigned status);
_Noreturn void platform_exception(void);
#ifdef __cplusplus
#undef _Noreturn
}
#endif
#endif
